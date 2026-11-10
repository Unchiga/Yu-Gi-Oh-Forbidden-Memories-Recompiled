# Mod API 3: settings, gameplay hooks, content and state

The existing portable ELF32 object format and API 1/2 layout remain supported.
API 3 appends services to `MemoriesModHost`; it does not enlarge `MemoriesMod`.
Check `host->api >= 3` before using them. Build code mods with
`python3 tools/pc/build_mod.py <directory>` and ship the object plus `mod.json`.
Both supported game builds consume the same object. Direct game-symbol access
remains available for advanced changes; it is coupled to the game build's
headers and implementation, unlike the managed event interface.

Worked examples are in `examples/mods/gameplay-rules` and
`examples/mods/card-pack`. They are not enabled or bundled automatically.

## Manifest and compatibility

```json
{
  "id": "my-rules",
  "name": "My rules",
  "version": "1.2",
  "author": "An author",
  "description": "What this changes.",
  "min_api": 3,
  "game": "slus_01411",
  "requires": [{"id": "base-rules", "min_version": "1.0", "max_version": "2.9"}],
  "after": ["visual-effects"],
  "conflicts": ["other-rules"],
  "priority": 10,
  "library": "rules",
  "settings": [
    {"key": "damage", "label": "Damage", "type": "int", "default": 100,
     "min": 0, "max": 300, "step": 10, "suffix": "%"},
    {"key": "enabled_rule", "label": "Extra rule", "type": "bool", "default": 1},
    {"key": "mode", "label": "Mode", "type": "choice", "default": 0,
     "choices": ["Classic", "Custom"]},
    {"key": "button", "label": "Action", "type": "key", "default": 1024}
  ]
}
```

IDs are 1–63 ASCII letters, digits, hyphens or underscores. `requires` accepts
IDs or objects with inclusive numeric dotted version bounds. `after` is an
optional ordering edge when both mods are enabled; `requires` also enforces
presence and activation. Mods caught in a cycle (and mods waiting on them) are
not loaded; the rest still are. Explicit conflicts are rejected
before applying a set. The user can override priority with `mod.<id>.order`;
lower numbers load first, but cannot bypass dependencies. Equal priorities
retain discovery order. `min_api` and `game` are checked before executing code.
An enabled dependency that fails initialization prevents its dependent from
starting during startup.

The loader accommodates 256 discovered mods. Named integer settings grow with
the collection instead of dropping entries after 128. Overlong keys or failed
allocation cause saving to fail rather than report a successful incomplete save.

Settings use `host->setting` as before: it answers with the fallback the code
mod passes for a key nobody has stored. What the game reads from a manifest
itself (a `setting` on a pack entry or a `frame_for` rule, `full_bleed`,
`count_setting`) is the setting's declared `default` until the player changes
it, the value the Mods window shows. Keys use letters, digits, `_`, `-`;
`order` is reserved. `description` supplies help, and `restart: true` marks an
option that cannot be updated live. A `key` value is a normalized pad-button
bitmask (Select through Square), not an OS keyboard scancode; keyboard and
controller mappings are configured in Controls. Options are validated before
persistence. Missing defaults use zero; integer bounds default to 0–100.
Use integers in documented units for fractional quantities, e.g. thousandths.

Data replacements run before byte patches. Within either kind, later loaded
entries win. The manager lists everything two enabled mods both change, from
disc files and texture images to cards, rules, limits and text, and how load
order settles it ([When mods overlap](modding.md#when-mods-overlap)); for code
it lists functions two mods hook and events two mods subscribe to, not what
native code changes by writing memory.
Data override preparation is atomic per mod: one invalid entry rolls back every
prepared override from that mod. Warning text on a successfully active mod does
not make it permanently impossible to re-enable. A mod whose data could not be
put in place (a replacement file missing, say) is tried again when the player
removes it and applies it again; one whose manifest or code failed to load
stays failed until the next launch.

## Gameplay hooks

Call `host->subscribe(host, event, priority, callback)` during initialization.
It returns a token, or zero on failure. `unsubscribe` removes that mod's token.
Higher priorities run first; equal priorities run in registration order. A before
callback may edit the event's arguments. Setting `handled = 1` bypasses the
original operation and stops the remaining before callbacks. `result` supplies
its return value where applicable. All after callbacks observe the final result;
the dispatcher discards their edits to the event structure.

Same-event recursion bypasses subscribers, so a replacement can call the game's
original public function to get its normal behavior. Subscriptions added during
a dispatch begin with the next dispatch; removed subscriptions stop immediately.
Only active, successfully initialized mods receive callbacks. Failed initialization
clears both legacy callbacks and managed registrations. Disabling retains the
object and registrations for later reactivation; `applied(0)` remains the place
for a mod to undo resources or changes it owns.

There are at most 4096 subscriptions. Dispatch does not allocate. Input callbacks
run from the VBlank service, potentially in interrupt context: do no allocation,
file I/O, registration or other non-reentrant work there. Other callbacks run
synchronously at the game operation. Keep them short. `host->pad` reads normalized
input before managed input hooks, so inspecting it cannot recurse into a hook.

| Event | Fields and operation |
|---|---|
| `INPUT` | `a` pad port, `b` button bits; before can edit `b`, or replace with `result`; after receives the bits passed to the game. With View > Japanese buttons on, Cross and Circle are exchanged between the two (`button_layout.h`): before sees the controller's bits, and a Cross a before-hook adds reaches the game as Circle |
| `DAMAGE` | `a` affected side, `b` damage, `c` 0 battle / 1 spell or reflected recovery; initial `result` is current LP; handled replaces remaining LP; after observes clamped remaining LP before the caller stores it |
| `REWARD` | `a` awarded card ID; edit it to replace the reward, or handle to cancel; invalid IDs are rejected |
| `STARCHIP` | API 6: `a` the configured end-of-duel prize (`limits.starchip_prize`, otherwise rank tier + 1); edit it to change the award, or handle to skip adding. After observes `result` as the amount actually credited (0 if handled or already at the `limits.starchips` cap, 999999 by default). The results display shows the configured prize, one icon and `xN` past eight; changing `a` in this event does not change that already displayed prize. See [rewards](gameplay-tables.md#values-atk-def-lp-starchips-and-more) |
| `FUSION` | `a`, `b` input card IDs before base-card mapping; handled `result` is the resulting card, or zero to forbid a fusion. Unhandled, the mods' `fusions` rules ([gameplay tables](gameplay-tables.md)) come next |
| `EFFECT` | Start: `a` presented card ID, `b` second-handler flag, `c=0`; update: `a` current effect card, `b` effect flags, `c=1`, `result` is returned flags. A custom multi-frame effect owns its flags and completion |
| `MONSTER` | API 10: `a` card ID, `b` duel record (0–29; side `b / 15`, monster zones `b % 15` = 5–9), `c` one of `MEMORIES_MONSTER_SUMMON`, `FLIP`, `DRAW`, `COMBAT`, `DESTROYED`, `DESTROY_OPPONENT`. Fires for every monster, with or without declarative effects. Before: `handled` skips its own `monster_effects` for this occasion; a callback may start its own card effect. After observes. See [monster effects](more-cards.md#monster-effects) and `mod_types.h` for timing; `face_up` is a continuous modifier, not an event |
| `AI` | Wraps the legacy `func_800279BC` selector; normal hand/field AI calls `AiScript_Run` directly. This event alone does not replace those decisions; see [AI hook research](ai-hard-mode-research.md#7-implementing-a-hard-mode-mod-in-this-port). A handled legacy call owns the selection record and supplies `result` |
| `SCENE` | Wraps `Main_ApplyMenuSelection`: `a` menu selection, `b` prior main mode; modify `a`, or handle the transition yourself. A title menu item's `MENU` result arrives here as `a` |
| `MENU` | API 9: an item of the title's menus chosen ([The title's menus](modding.md#the-titles-menus)): `a` the item (0-10 the entries, 11 on the buttons, named by `host->menu_item`), `b` the menu (0 first, 1 second), `c` the item's `value`, `result` -1. Before: handle to replace the item's action, and set `result` to a selection to leave the title with; after observes |
| `EQUIP` | `a` equip card, `b` monster; handled `result` nonzero lets the equip apply, zero refuses it. Unhandled, the mods' `equips` rules and then the disc's table decide |
| `SETTINGS` | After only: `a` manager mod index, `b` option index, `c` new value after applying the settings batch |
| `SAVE` | Before registered mod buffers are serialized: pack pointer-free state into your registered buffer |
| `LOAD` | After save-state restoration and legacy reset callbacks: rebuild runtime caches from the registered buffer |
| `SLOT_SAVE`, `SLOT_LOAD` | API 4, after only: the save slot menu saved or loaded the running game; `a` slot (from 0), `b` the slot's token, `c` the save's sequence. The load is reported once the game holds the loaded save |

These are concrete extension points, not an automatic replacement mechanism for
every exported function. Other game code can still be called or data edited via
the SDK; adding another managed event should include a documented call site and
regression coverage. Native mods run inside the game process.

## Card definitions and identities

Each card entry can have an `id`, local to its mod. Runtime IDs remain compact
numbers; persistence uses `mod-id:entry-id:n`, where `n` starts at 1 for entries
that generate several cards. Without an explicit ID the fallback is `entry-N`,
using the zero-based manifest index: keep entry order stable in such old packs.
Duplicate identities and invalid keys are diagnosed. Always give new entries
explicit IDs and keep them stable across releases.

`host->card_id(host, "example-cards:moon-dragon:1")` resolves a stable identity
after the startup card build. It returns zero before the registry is ready or
when the card is unavailable. Names and artwork are presentation, not identity.

`host->duelist_id(host, "shadow-duelists:dark-simon")` (API 5) does the same for a
duelist a mod added, once the duelist list is built. An added duelist's id
depends on which mods are applied, in what order and what slots they asked
for, so it cannot be written down in advance. Zero for a duelist that is not
here this run, and for one of the disc's own, whose id already names it
(`notes/more-duelists.md`).

An entry's `fusions` list accepts `{ "with": <retail-id-or-stable-identity>,
"result": <retail-id-or-stable-identity> }`. A result of zero forbids that
combination. Explicit recipes are checked before base-card fusion tables.
`model` and `effect` independently choose retail model/effect IDs, rather than
forcing both to match `copy`. Managed effect/fusion hooks can implement behavior
that has no retail counterpart. New arbitrary 3D geometry still requires a code
mod or a replacement of the corresponding model data; `model` is a retail ID.

New sidecar sections persist identities for ownership, seen flags and decks.
Runtime ids follow the load order, so changing it (or which mods are applied)
remaps the same cards. A missing card falls back to its
retail base in a deck; its stored ownership/seen flags survive saves made with
other card packs, and return when it is reinstalled.

Old sidecars have numeric IDs only. By default their ambiguous ownership is not
assigned to current cards, decks fall back to the recorded retail base, and the
old numeric lines are carried over unchanged (a copy is kept as `.txt.legacy`)
while new progress is still saved under stable identities. To migrate, restore the **original card mod set
and entry order**, with the card mods in the load order of the folders' names
(the order the ids were given in before they followed the load order: no
`mod.<id>.order` or `priority` and no `after` or `requires` between them that
moves one),
launch once with `MEMORIES_MIGRATE_CARD_IDS=1`, load the save,
and save again. Retained sections are then converted to identities. The game cannot infer a lost historical
mod order; keep that backup if the original set is uncertain.

## Save states

`host->register_state(host, buffer, size, version)` registers one block per mod,
at most 1 MiB. Keep the allocation alive until shutdown; store values/indices,
not pointers. Bump the version whenever the layout changes. A failed registration
returns zero. Use SAVE/LOAD events for packing or rebuilding richer state.

Before changing game memory, loading checks the active mod identities, manifests,
loaded code hashes, actual activation order, effective declared options and stored
extra mod settings, plus each registered buffer's size and version. The code
hash covers only what the loader loads (allocated sections, their relocations and
global names), not debugging information, so rebuilding the same code after a
header change or in another folder keeps states loadable. A mismatch
rejects the state and asks for its original profile. Old vanilla states remain
usable; old modded states without a compatibility record are rejected. This does
not hash every external texture or data asset: bump the manifest version when
changing shipped assets. Normal memory-card saves use identity remapping instead
of requiring an identical active mod set.

## Validation

`pc_mods_manager`, `pc_mods_window`, `pc_cards_identity`, `pc_settings`, and
`pc_mods_lifecycle` cover the new paths. The lifecycle script runs actual rejected
native objects on Linux and Windows/Wine; its Linux half also checks the real
save-state chunk reader and compatibility preflight. The ordinary loader fuzz
fixtures and deterministic game smoke cases remain in place. PC-only hooks are
excluded from retail compilation; `make match` remains the exact-match gate.
