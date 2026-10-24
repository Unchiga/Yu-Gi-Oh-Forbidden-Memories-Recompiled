# Yu-Gi-Oh! Forbidden Memories Recompiled: Complete Mod API & FM Editor Documentation

This document provides exhaustive, authoritative reference documentation for modding **Yu-Gi-Oh! Forbidden Memories Recompiled (PC Port)**. It covers two foundational systems:

1. **The Native Mod API & Runtime Engine Architecture** — the C interface (APIs 1 through 18), host services, hooks, callbacks, event dispatching, memory isolation, audio subsystem, overlay drawing, and declarative manifest specifications.
2. **The FM Editor (`tools/pc/fm_editor`)** — all editor tabs, gameplay tables, data structures, limits, visual editors, audio systems, and game tweaks made possible by the standalone graphical suite.

---

# Table of Contents

- [Part 1: Engine Architecture & Mod Lifecycle](#part-1-engine-architecture--mod-lifecycle)
  - [1.1 The "Mod is the Diff" Paradigm](#11-the-mod-is-the-diff-paradigm)
  - [1.2 Mod Discovery and Load Hierarchy](#12-mod-discovery-and-load-hierarchy)
  - [1.3 Overlap Resolution & Deterministic Ordering](#13-overlap-resolution--deterministic-ordering)
  - [1.4 The Mod Manifest (`mod.json`)](#14-the-mod-manifest-modjson)
  - [1.5 Custom User Settings System](#15-custom-user-settings-system)
- [Part 2: The C/Native Mod API Reference](#part-2-the-cnative-mod-api-reference)
  - [2.1 API Version Progression (1 to 18)](#21-api-version-progression-1-to-18)
  - [2.2 Plugin Lifecycle & Entry Point (`MemoriesMod`)](#22-plugin-lifecycle--entry-point-memoriesmod)
  - [2.3 The Host Interface (`MemoriesModHost`)](#23-the-host-interface-memoriesmodhost)
    - [2.3.1 Core Environment & Logging](#231-core-environment--logging)
    - [2.3.2 Sandboxed Virtual File I/O](#232-sandboxed-virtual-file-io)
    - [2.3.3 Mod Settings Persistence & Overrides](#233-mod-settings-persistence--overrides)
    - [2.3.4 Disc Discovery & Sector Streaming](#234-disc-discovery--sector-streaming)
    - [2.3.5 Input Subsystem & Pad Polling](#235-input-subsystem--pad-polling)
    - [2.3.6 Precision Clocks & Guest-Address Mapping (API 2)](#236-precision-clocks--guest-address-mapping-api-2)
    - [2.3.7 Event Subscriptions & Save-State Storage (API 3)](#237-event-subscriptions--save-state-storage-api-3)
    - [2.3.8 Function Hooking & Dynamic Linking (API 4)](#238-function-hooking--dynamic-linking-api-4)
    - [2.3.9 Inter-Mod Service Sharing (API 4)](#239-inter-mod-service-sharing-api-4)
    - [2.3.10 High-Resolution 2D Canvas Overlay (API 4)](#2310-high-resolution-2d-canvas-overlay-api-4)
    - [2.3.11 Duelist Resolution (API 5)](#2311-duelist-resolution-api-5)
    - [2.3.12 Card Notes & RPG Tag Querying (API 7)](#2312-card-notes--rpg-tag-querying-api-7)
    - [2.3.13 Engine Limit Introspection (API 8)](#2313-engine-limit-introspection-api-8)
    - [2.3.14 Dynamic Menu Identification (API 9)](#2314-dynamic-menu-identification-api-9)
    - [2.3.15 Dynamic Sound Synthesis & Audio Streaming (API 12)](#2315-dynamic-sound-synthesis--audio-streaming-api-12)
  - [2.4 Managed Gameplay Events Dispatcher](#24-managed-gameplay-events-dispatcher)
    - [2.4.1 Event Flow, Priorities, and Control Fields](#241-event-flow-priorities-and-control-fields)
    - [2.4.2 Complete Event Catalog](#242-complete-event-catalog)
  - [2.5 Function Hooking Subsystem Details](#25-function-hooking-subsystem-details)
  - [2.6 Save States & Binary Fingerprinting](#26-save-states--binary-fingerprinting)
- [Part 3: Data Mods & Asset Replacements](#part-3-data-mods--asset-replacements)
  - [3.1 Virtual Disc Replacement (`data`)](#31-virtual-disc-replacement-data)
  - [3.2 Named Asset Replacement (`assets`)](#32-named-asset-replacement-assets)
  - [3.3 High-Definition Texture Packs (`textures`)](#33-high-definition-texture-packs-textures)
  - [3.4 Audio Streams & Sound Effects (`audio`)](#34-audio-streams--sound-effects-audio)
  - [3.5 Text Replacement & Multi-Language Translations (`text`, `font`)](#35-text-replacement--multi-language-translations-text-font)
- [Part 4: The FM Editor — Complete Tweaks & Systems Guide](#part-4-the-fm-editor--complete-tweaks--systems-guide)
  - [4.1 Cards Tab (Card Expansion, Attributes, & Logic)](#41-cards-tab-card-expansion-attributes--logic)
    - [4.1.1 Card Extension vs Replacement (`copy` vs `replace`)](#411-card-extension-vs-replacement-copy-vs-replace)
    - [4.1.2 Stats, Types, Attributes, Levels, & Guardian Stars](#412-stats-types-attributes-levels--guardian-stars)
    - [4.1.3 Type Switching & Cross-Type Morphing](#413-type-switching--cross-type-morphing)
    - [4.1.4 3D Models, Sound Effects, & Exodia Logic](#414-3d-models-sound-effects--exodia-logic)
    - [4.1.5 Attack Trap Threshold Overrides per Card](#415-attack-trap-threshold-overrides-per-card)
    - [4.1.6 The Monster Effects System](#416-the-monster-effects-system)
    - [4.1.7 Mod API 19: Modular Conditions & Battle Immunity System](#417-mod-api-19-modular-conditions--battle-immunity-system)
    - [4.1.8 Card Text Engine, Inline Icons, & Text Colors](#418-card-text-engine-inline-icons--text-colors)
    - [4.1.9 Developer Notes & Tag Extraction](#419-developer-notes--tag-extraction)
  - [4.2 Art Tab (Card Artwork & Visual Pipeline)](#42-art-tab-card-artwork--visual-pipeline)
    - [4.2.1 Specifications, Palettes, and Quantization](#421-specifications-palettes-and-quantization)
    - [4.2.2 High-Definition Rendering Support](#422-high-definition-rendering-support)
    - [4.2.3 Title Plate Typography Generator](#423-title-plate-typography-generator)
    - [4.2.4 3D Field Cutout Art (`field_art`)](#424-3d-field-cutout-art-field_art)
  - [4.3 Fusions & Bulk Fusions Tab](#43-fusions--bulk-fusions-tab)
    - [4.3.1 Declarative Fusion Rules](#431-declarative-fusion-rules)
    - [4.3.2 Non-Commutative Fusions & Fusion Blocking](#432-non-commutative-fusions--fusion-blocking)
    - [4.3.3 Table Clearing (`remove: "all"`)](#433-table-clearing-remove-all)
    - [4.3.4 The Bulk Fusions Automation Engine](#434-the-bulk-fusions-automation-engine)
  - [4.4 Equips Tab](#44-equips-tab)
    - [4.4.1 Eligibility Matrices (`add`, `remove`, `replace`)](#441-eligibility-matrices-add-remove-replace)
    - [4.4.2 Stat Bonuses, Asymmetric Boosts, & Conditional Boosts](#442-stat-bonuses-asymmetric-boosts--conditional-boosts)
    - [4.4.3 Global Default Bonus (`equip_bonus_default`)](#443-global-default-bonus-equip_bonus_default)
  - [4.5 Rituals Tab](#45-rituals-tab)
    - [4.5.1 Multi-Tribute Rituals (1 to 5 Tributes)](#451-multi-tribute-rituals-1-to-5-tributes)
    - [4.5.2 Tribute Origins: Field, Hand, and Both](#452-tribute-origins-field-hand-and-both)
    - [4.5.3 Condition-Based Tributes & Fusion Groups](#453-condition-based-tributes--fusion-groups)
  - [4.6 Duelists Tab & Roster System](#46-duelists-tab--roster-system)
    - [4.6.1 Roster File Hierarchy & Added Duelists](#461-roster-file-hierarchy--added-duelists)
    - [4.6.2 Free Duel Grid Placement (`slot`) & Collision Rules](#462-free-duel-grid-placement-slot--collision-rules)
    - [4.6.3 40-Card Deck Pools & Fixed Decks (`decks`)](#463-40-card-deck-pools--fixed-decks-decks)
    - [4.6.4 Drop Pools: SA-POW, BCD, SA-TEC (`drops`)](#464-drop-pools-sa-pow-bcd-sa-tec-drops)
    - [4.6.5 Custom Duelist Portraits & 16-Color Quantization](#465-custom-duelist-portraits--16-color-quantization)
    - [4.6.6 Large Character Busts & Emotional Expressions](#466-large-character-busts--emotional-expressions)
    - [4.6.7 AI Strategy, Search Depth, & Omniscience (`sight`)](#467-ai-strategy-search-depth--omniscience-sight)
    - [4.6.8 Rank Score Evaluation Calibration (`ranks`)](#468-rank-score-evaluation-calibration-ranks)
    - [4.6.9 Complex Unlock Conditions (`unlock`)](#469-complex-unlock-conditions-unlock)
  - [4.7 UI Tab (Interface, Menus, Frames, & Boards)](#47-ui-tab-interface-menus-frames--boards)
    - [4.7.1 Title Screen Customization](#471-title-screen-customization)
    - [4.7.2 Interactive Menus & Custom Button Actions](#472-interactive-menus--custom-button-actions)
    - [4.7.3 Duel HUD Layout, Sizing, & Visibility](#473-duel-hud-layout-sizing--visibility)
    - [4.7.4 3D Duel Board Arenas & Terrain Textures](#474-3d-duel-board-arenas--terrain-textures)
    - [4.7.5 Frame Styles & Full Card Layout Engine](#475-frame-styles--full-card-layout-engine)
  - [4.8 Starter Decks Tab](#48-starter-decks-tab)
    - [4.8.1 Fixed 40-Card Starter Decks](#481-fixed-40-card-starter-decks)
    - [4.8.2 Probabilistic Starter Pools](#482-probabilistic-starter-pools)
  - [4.9 Campaign Map Tab](#49-campaign-map-tab)
    - [4.9.1 Binary Location Records Structure](#491-binary-location-records-structure)
    - [4.9.2 Camera ViewState & Target Vectors](#492-camera-viewstate--target-vectors)
    - [4.9.3 Town Millennium Puzzle Coordinates](#493-town-millennium-puzzle-coordinates)
    - [4.9.4 Exits, Directional Inputs, Story Flags, & Animations](#494-exits-directional-inputs-story-flags--animations)
  - [4.10 Values Tab (Core Game Limits & Engine Mechanics)](#410-values-tab-core-game-limits--engine-mechanics)
    - [4.10.1 Stat & Life Point Boundaries](#4101-stat--life-point-boundaries)
    - [4.10.2 Magic & Trap Constants](#4102-magic--trap-constants)
    - [4.10.3 Inventory Capacities & Trunk Overflow Conversion](#4103-inventory-capacities--trunk-overflow-conversion)
    - [4.10.4 StarChip Economy & Drop Locks](#4104-starchip-economy--drop-locks)
    - [4.10.5 Rank Scoring Constants](#4105-rank-scoring-constants)
    - [4.10.6 Password Shop Pricing & Overrides](#4106-password-shop-pricing--overrides)
    - [4.10.7 Terrain Combat Multipliers](#4107-terrain-combat-multipliers)
  - [4.11 Guardian Stars Tab](#411-guardian-stars-tab)
    - [4.11.1 Expanding Beyond the 10 Retail Stars (Up to 15 Stars)](#4111-expanding-beyond-the-10-retail-stars-up-to-15-stars)
    - [4.11.2 Custom Matchup Advantages & Mirror Match Rules](#4112-custom-matchup-advantages--mirror-match-rules)
    - [4.11.3 Summoning Star Selection Policies](#4113-summoning-star-selection-policies)
    - [4.11.4 Batch Rule Automation ("Set stars by rule...")](#4114-batch-rule-automation-set-stars-by-rule)
  - [4.12 Packs Tab (Card Booster Packs & In-Game Shop)](#412-packs-tab-card-booster-packs--in-game-shop)
    - [4.12.1 Booster Pack Packaging & Visuals](#4121-booster-pack-packaging--visuals)
    - [4.12.2 Economic Models & Multi-Currency Costs](#4122-economic-models--multi-currency-costs)
    - [4.12.3 Rarity Tiers, Drop Probabilities, & Weighted Pools](#4123-rarity-tiers-drop-probabilities--weighted-pools)
    - [4.12.4 Guaranteed Slots & Pity Mechanics](#4124-guaranteed-slots--pity-mechanics)
    - [4.12.5 Unlocks, Limited Stock, & Shop Environment Tuning](#4125-unlocks-limited-stock--shop-environment-tuning)
  - [4.13 Tower Mode Tab (Gauntlet Dungeons & Visual Novel)](#413-tower-mode-tab-gauntlet-dungeons--visual-novel)
    - [4.13.1 Floor-by-Floor Progression (Up to 99 Floors)](#4131-floor-by-floor-progression-up-to-99-floors)
    - [4.13.2 Floor Constraints & Duel Modifiers](#4132-floor-constraints--duel-modifiers)
    - [4.13.3 Cutscene Scripting Syntax & Stage Choreography](#4133-cutscene-scripting-syntax--stage-choreography)
    - [4.13.4 Rewards, Checkpoints, & Failure Rules](#4134-rewards-checkpoints--failure-rules)
  - [4.14 Mod Info, Diagnostics, & Conflict Matrix](#414-mod-info-diagnostics--conflict-matrix)
    - [4.14.1 Mod Validation & Error Preflight](#4141-mod-validation--error-preflight)
    - [4.14.2 Conflict Detection Engine](#4142-conflict-detection-engine)
  - [4.15 Mod Conversion, Disc Import, & Migration Tools](#415-mod-conversion-disc-import--migration-tools)
    - [4.15.1 Direct Retail Disc Extraction](#4151-direct-retail-disc-extraction)
    - [4.15.2 Converting Old `.ygomods` Packages](#4152-converting-old-ygomods-packages)
    - [4.15.3 Card Identity Migration (`MEMORIES_MIGRATE_CARD_IDS`)](#4153-card-identity-migration-memories_migrate_card_ids)
    - [4.15.4 CLI Verification & Headless Validation](#4154-cli-verification--headless-validation)

---

# Part 1: Engine Architecture & Mod Lifecycle

## 1.1 The "Mod is the Diff" Paradigm

In Yu-Gi-Oh! Forbidden Memories Recompiled, the PC port does not modify the retail disc images or replace the core executables. Instead, all modding operates under the strict principle: **The mod is the diff.**

- The engine initializes by reading retail gameplay data and disc assets directly from the original disc (`SLUS_014.11` / `SLES_014.11`) or uncompressed `game/` dump.
- A mod is a self-contained directory containing a `mod.json` manifest.
- When a mod is applied, its `mod.json` overlays retail tables. When saved, the editor writes **only** the properties that differ from retail defaults.
- Multiple mods can be loaded concurrently; their changes are layered in a deterministic, user-controlled order.

## 1.2 Mod Discovery and Load Hierarchy

Mods are discovered automatically at startup from two locations:

1. **Bundled Mods Directory**: `mods/` located beside the game executable (`memories-pc.exe` or `memories-pc`).
2. **User Mods Directory**: The system application data directory (or the path set by `MEMORIES_MODS_DIR`).

Up to **256 distinct mods** can be discovered in a single session (`MODS_MAX = 256`). Whether a mod is active is tracked by the player settings file under `mod.<id>`.

## 1.3 Overlap Resolution & Deterministic Ordering

When two or more mods modify the same game entity (such as the same retail card, the same drop pool, the same texture, or hook the same function):

1. **Dependency Constraints (`requires`)**: Required mods load before the dependent mod.
2. **Topological Order (`after`)**: Explicit sequence edges ensure mod A loads after mod B without requiring presence.
3. **Priority Numbers (`priority`)**: Manifest priorities dictate sequence (lower numbers load earlier).
4. **User Override (`mod.<id>.order`)**: The player can force execution order from the in-game Mods window.
5. **Rule of the Later Mod**: Data replacements run before byte patches. For conflicting scalar properties (e.g. card ATK), **the later loaded mod wins**. For additive collections (e.g. drop weights), modifications accumulate.

## 1.4 The Mod Manifest (`mod.json`)

The manifest must be valid UTF-8 JSON. The top-level schema accepts the following core keys:

```json
{
  "id": "my-custom-mod",
  "name": "My Custom Expansion",
  "version": "1.0.0",
  "author": "Modder Name",
  "description": "Comprehensive gameplay expansion and rebalance.",
  "min_api": 18,
  "game": "slus_01411",
  "priority": 10,
  "requires": [
    { "id": "core-engine-pack", "min_version": "1.0.0", "max_version": "2.0.0" }
  ],
  "after": ["texture-overhaul"],
  "conflicts": ["incompatible-mod"],
  "library": "my_mod_logic",
  "settings": [
    {
      "key": "starting_lp",
      "label": "Starting Life Points",
      "type": "int",
      "default": 8000,
      "min": 1000,
      "max": 30000,
      "step": 500,
      "suffix": " LP",
      "restart": true,
      "description": "Initial duel Life Points."
    }
  ],
  "cards": [],
  "fusions": [],
  "equips": [],
  "rituals": [],
  "drops": {},
  "decks": {},
  "limits": {},
  "starter": [],
  "starter_pools": [],
  "packs": [],
  "pack_shop": {},
  "tower": {},
  "guardian_stars": {},
  "title": {},
  "menu": {},
  "ui": {},
  "card_layout": {},
  "data": [],
  "assets": {},
  "textures": {},
  "audio": {}
}
```

### Manifest Header Fields

| Key           | Type    | Description                                                                                                    |
| ------------- | ------- | -------------------------------------------------------------------------------------------------------------- |
| `id`          | String  | 1–63 ASCII characters (`a-z`, `A-Z`, `0-9`, `_`, `-`). Unique namespace identifier.                            |
| `name`        | String  | Display name shown in Mods menu and UI.                                                                        |
| `version`     | String  | Semantic version string (e.g. `1.2.0`).                                                                        |
| `author`      | String  | Author or development team.                                                                                    |
| `description` | String  | Multi-line description displayed in the UI.                                                                    |
| `min_api`     | Integer | Minimum Mod API version required by the game host (1–19).                                                      |
| `game`        | String  | Restricts to specific retail binary (`slus_01411` or `sles_01411`).                                            |
| `priority`    | Integer | Default load weight; lower values initialize first (default: 0).                                               |
| `requires`    | Array   | Required mod dependencies. Items are either string IDs or objects with `id`, `min_version`, and `max_version`. |
| `after`       | Array   | List of mod IDs that this mod must load after if both are enabled.                                             |
| `conflicts`   | Array   | List of mod IDs that cannot be loaded simultaneously with this mod.                                            |
| `library`     | String  | Name of the native code object without extension (`.elf`/`.so`/`.dll`).                                        |
| `libraries`   | Object  | Architecture-specific binaries (e.g. `{"macos-arm64": "lib_arm64"}`).                                          |
| `settings`    | Array   | Declares user-configurable options exposed in the settings menu.                                               |

## 1.5 Custom User Settings System

Declared under `"settings"`, options appear directly in the in-game GUI:

- **`int`**: Integer sliders with `min`, `max`, `step`, and optional `suffix` (e.g. `"%"`).
- **`bool`**: Boolean checkboxes (`0` or `1`).
- **`choice`**: Dropdown select list with an array of string `choices`.
- **`key`**: Controller pad button binding bitmask.

If an option has `"restart": true`, changing it informs the player that an engine restart is required before the new value is applied to static tables. At runtime, settings can be read by code mods using `host->setting(...)` or overridden by environment variables: `MEMORIES_MOD_<ID>_<KEY>`.

---

# Part 2: The C/Native Mod API Reference

Native code mods are compiled into portable ELF32 objects or shared objects using `tools/pc/build_mod.py`. The mod exports a single initialization symbol:

```c
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);
```

## 2.1 API Version Progression (1 to 19)

The host version is defined by `MEMORIES_MOD_API` in `mod_types.h`. As new engine features were introduced, the host interface was expanded strictly by appending function pointers to `MemoriesModHost`:

| API Version | Major Introductions & Capabilities                                                                                                                                                                                                                                                                                                             |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **API 1**   | Baseline API: `applied`, logging (`log`, `log_enabled`), sandboxed file I/O (`open_asset`, `open_data`), settings (`setting`, `set_setting`), disc access (`disc_file_start`, `disc_read`), controller polling (`pad`).                                                                                                                        |
| **API 2**   | Monotonic microsecond clock (`now_us`), guest-sized fixed virtual memory mapping (`map_fixed`). Environment variable setting overrides.                                                                                                                                                                                                        |
| **API 3**   | Managed event subscription framework (`subscribe`, `unsubscribe`), save-state persistent memory registration (`register_state`), stable card identity resolution (`card_id`).                                                                                                                                                                  |
| **API 4**   | Arbitrary game function hooking/unhooking (`hook`, `unhook`), dynamic symbol resolution (`symbol`), inter-mod provider registration (`provide`, `find`), 2D hardware-scaled overlay rendering (`overlay`, `overlay_size`, `draw_text`, `text_width`, `fill`), slot save/load events.                                                           |
| **API 5**   | Stable duelist identity resolution (`duelist_id`).                                                                                                                                                                                                                                                                                             |
| **API 6**   | `MEMORIES_EVENT_STARCHIP` post-duel prize interceptor.                                                                                                                                                                                                                                                                                         |
| **API 7**   | Card notes inspector (`card_notes`) and key-value tag parser (`card_tag`).                                                                                                                                                                                                                                                                     |
| **API 8**   | Engine limit introspection query (`limit`).                                                                                                                                                                                                                                                                                                    |
| **API 9**   | Title menu introspection (`menu_item`) and `MEMORIES_EVENT_MENU`.                                                                                                                                                                                                                                                                              |
| **API 10**  | Continuous and triggered monster events (`MEMORIES_EVENT_MONSTER`).                                                                                                                                                                                                                                                                            |
| **API 11**  | Manifest features marker for extended card definitions.                                                                                                                                                                                                                                                                                        |
| **API 12**  | Dynamic PCM audio clipping subsystem (`sound_add`, `sound_play`, `sound_free`).                                                                                                                                                                                                                                                                |
| **API 13**  | Declarative card drops locking (`limits.card_drops`).                                                                                                                                                                                                                                                                                          |
| **API 14**  | Tower Mode system, gauntlet rules, and story script engine.                                                                                                                                                                                                                                                                                    |
| **API 15**  | Declarative custom monster types (IDs 24–255) and icons (`card_types`).                                                                                                                                                                                                                                                                        |
| **API 16**  | Declarative custom attributes (IDs 8–255) and icons (`attributes`).                                                                                                                                                                                                                                                                            |
| **API 17**  | Duelist character busts (`bust`, `replace_bust`) and emotional expression naming.                                                                                                                                                                                                                                                              |
| **API 18**  | Advanced card frames (`card_layout.frame_styles`), part repositioning, custom label hiding/tinting, and card layout overrides.                                                                                                                                                                                                                 |
| **API 19**  | Extensible Monster Effects Conditions System and Battle Immunity (`do: "battle_immunity"`). Supports up to 4 modular conditions per effect (`named_card`, `first_attacked`, `stat_threshold`, `lowest_atk`, `highest_atk`) with configurable evaluation modes (`all` / `any`), stat recursion protection, and FM Editor GUI condition builder. |

## 2.2 Plugin Lifecycle & Entry Point (`MemoriesMod`)

When the engine loads a code mod, it passes a pointer to `MemoriesMod`:

```c
typedef struct {
    unsigned api;            /* Set to MEMORIES_MOD_API */
    const char *name;        /* Optional mod name override */
    void (*frame)(void);     /* Invoked once per presented video frame */
    void (*applied)(int on); /* Invoked when mod is toggled active (1) or inactive (0) */
    void (*reset)(void);     /* Invoked on save-state load or soft reset */
    void (*shutdown)(void);  /* Invoked when the game process exits */
    void (*overlay)(void);   /* Invoked during UI drawing pass (API 4) */
    unsigned (*overlay_signature)(void); /* Return unique integer when overlay needs redraw */
} MemoriesMod;
```

### Lifecycle Callback Contracts

- **`MemoriesModInit`**: Called once upon initial load. Fills in the `MemoriesMod` struct. Must return `1` on success, or `0` to abort load.
- **`frame`**: Called every display frame after game scene logic completes, immediately before GPU buffer swap. Ideal for timers, custom physics, or background polling.
- **`applied`**: Called with `1` when enabled, `0` when disabled. When disabled, the mod should disable internal systems and free transient resources.
- **`reset`**: Called when the engine restores a save-state. Any cached pointers or state derived from previous game RAM must be invalidated or recomputed.
- **`shutdown`**: Process termination cleanup.
- **`overlay`**: Called when the 2D menu overlay is drawn. Only active while the mod is applied.
- **`overlay_signature`**: Return a checksum or counter that increments when the overlay contents change. If it returns the same value, the engine reuses the cached overlay canvas texture.

## 2.3 The Host Interface (`MemoriesModHost`)

The `MemoriesModHost` structure is the complete service gateway provided by the engine to the mod:

```c
struct MemoriesModHost {
    unsigned api;
    const char *id;
    const char *directory;
    void *reserved;

    int (*applied)(const MemoriesModHost *host);
    void (*log)(const MemoriesModHost *host, const char *format, ...);
    int (*log_enabled)(const MemoriesModHost *host);

    FILE *(*open_asset)(const MemoriesModHost *host, const char *relative);
    FILE *(*open_data)(const MemoriesModHost *host, const char *relative, const char *mode);

    int (*setting)(const MemoriesModHost *host, const char *key, int fallback);
    void (*set_setting)(const MemoriesModHost *host, const char *key, int value);

    int (*disc_file_start)(const MemoriesModHost *host, const char *iso_path);
    int (*disc_read)(const MemoriesModHost *host, int lba, int sectors, void *out);

    unsigned short (*pad)(const MemoriesModHost *host, int port);

    /* API 2 */
    uint64_t (*now_us)(const MemoriesModHost *host);
    void *(*map_fixed)(const MemoriesModHost *host, uintptr_t address, size_t size);

    /* API 3 */
    int (*subscribe)(const MemoriesModHost *, unsigned event, int priority, MemoriesModCallback);
    void (*unsubscribe)(const MemoriesModHost *, int token);
    int (*register_state)(const MemoriesModHost *, void *data, size_t size, unsigned version);
    int (*card_id)(const MemoriesModHost *, const char *identity);

    /* API 4 */
    int (*hook)(const MemoriesModHost *, void *function, void *replacement, void **original);
    void (*unhook)(const MemoriesModHost *, int token);
    void *(*symbol)(const MemoriesModHost *, const char *name);
    int (*provide)(const MemoriesModHost *, const char *name, void *pointer);
    void *(*find)(const MemoriesModHost *, const char *qualified);
    void (*overlay_size)(const MemoriesModHost *, int *width, int *height, int *scale);
    void (*draw_text)(const MemoriesModHost *, int x, int middle, const char *text, uint32_t rgb, int scale);
    int (*text_width)(const MemoriesModHost *, const char *text, int scale);
    void (*fill)(const MemoriesModHost *, int x, int y, int w, int h, uint32_t rgb, unsigned alpha);

    /* API 5 */
    int (*duelist_id)(const MemoriesModHost *, const char *identity);

    /* API 7 */
    const char *(*card_notes)(const MemoriesModHost *, int id);
    int (*card_tag)(const MemoriesModHost *, int id, const char *key, char *out, size_t size);

    /* API 8 */
    long (*limit)(const MemoriesModHost *, const char *name);

    /* API 9 */
    const char *(*menu_item)(const MemoriesModHost *, int index);

    /* API 12 */
    int (*sound_add)(const MemoriesModHost *, const int16_t *samples, size_t frames, int channels, unsigned rate);
    int (*sound_play)(const MemoriesModHost *, int sound, int volume, int pan);
    void (*sound_free)(const MemoriesModHost *, int sound);
};
```

### 2.3.1 Core Environment & Logging

- `host->applied(host)`: Returns `1` if the current mod is actively enabled, `0` otherwise.
- `host->log(host, format, ...)`: Emits a message to the logging stream under channel `[mods]`.
- `host->log_enabled(host)`: Queries whether mod logging is currently enabled to avoid expensive string formatting.

### 2.3.2 Sandboxed Virtual File I/O

- `host->open_asset(host, relative)`: Opens a read-only stream to a file packaged inside the mod's installation directory. Paths cannot start with `/`, `\`, drive letters, or contain `..`.
- `host->open_data(host, relative, mode)`: Opens a read/write stream to persistent mod-private storage in the user's data directory.

### 2.3.3 Mod Settings Persistence & Overrides

- `host->setting(host, key, fallback)`: Reads an integer configuration value from `mod.<id>.<key>`. If an environment variable `MEMORIES_MOD_<ID>_<KEY>` exists, it takes absolute precedence.
- `host->set_setting(host, key, value)`: Updates the stored integer setting in the user's profile.

### 2.3.4 Disc Discovery & Sector Streaming

- `host->disc_file_start(host, iso_path)`: Resolves an absolute disc path (e.g. `"\\DATA\\MODEL.MRG;1"`) to its starting Logical Block Address (LBA) sector index. Returns `-1` if not found.
- `host->disc_read(host, lba, sectors, out)`: Reads raw 2048-byte CD-ROM sectors directly into buffer `out`. Does not block or interfere with the game's streaming drive thread.

### 2.3.5 Input Subsystem & Pad Polling

- `host->pad(host, port)`: Returns the raw normalized 16-bit pad mask for controller `0` or `1`. Reads controller state directly before managed input hooks are processed.

### 2.3.6 Precision Clocks & Guest-Address Mapping (API 2)

- `host->now_us(host)`: Returns high-resolution monotonic elapsed time in microseconds.
- `host->map_fixed(host, address, size)`: Maps zeroed read/write memory at an exact guest virtual address. The requested address must be aligned to 64 KiB boundaries. Returns `NULL` if the requested range is occupied.

### 2.3.7 Event Subscriptions & Save-State Storage (API 3)

- `host->subscribe(host, event, priority, callback)`: Registers a callback for a managed gameplay event. Priority governs execution order (higher numbers run earlier). Returns a non-zero token on success.
- `host->unsubscribe(host, token)`: Unregisters an active callback.
- `host->register_state(host, data, size, version)`: Registers a pointer-free persistent block (up to 1 MiB) that is automatically serialized into game save states. Version mismatches reject invalid states.
- `host->card_id(host, identity)`: Resolves a stable string identifier (e.g. `"my-mod:moon-dragon:1"`) to its current runtime card ID (1–32766).

### 2.3.8 Function Hooking & Dynamic Linking (API 4)

- `host->hook(host, function, replacement, &original)`: Hooks a native game function pointer. Automatically updates `*original` to point to the superseded function body, allowing chain-wrapping.
- `host->unhook(host, token)`: Removes an installed hook and restores the call chain.
- `host->symbol(host, name)`: Dynamically looks up an exported game or runtime symbol by its string name.

### 2.3.9 Inter-Mod Service Sharing (API 4)

- `host->provide(host, name, pointer)`: Exports a symbol or interface structure from this mod under `"mod-id:name"`.
- `host->find(host, qualified)`: Queries an exported symbol provided by another mod using its qualified name.

### 2.3.10 High-Resolution 2D Canvas Overlay (API 4)

Available exclusively within `MemoriesMod.overlay`:

- `host->overlay_size(host, &width, &height, &scale)`: Returns canvas dimensions and UI scaling factor.
- `host->draw_text(host, x, middle, text, rgb, scale)`: Draws text centered vertically around `middle`.
- `host->text_width(host, text, scale)`: Measures the bounding width of a string in pixels.
- `host->fill(host, x, y, w, h, rgb, alpha)`: Draws a solid or blended alpha rectangle (alpha: 0–255).

### 2.3.11 Duelist Resolution (API 5)

- `host->duelist_id(host, identity)`: Resolves a stable duelist string identifier (`"mod-id:duelist-key"`) to its current runtime slot index (0–127).

### 2.3.12 Card Notes & RPG Tag Querying (API 7)

- `host->card_notes(host, id)`: Retrieves the combined raw developer notes string for card `id`.
- `host->card_tag(host, id, key, out, size)`: Extracts the string value of a tag formatted as `<key: value>` from the card's notes. Returns tag string length or `-1` if not found.

### 2.3.13 Engine Limit Introspection (API 8)

- `host->limit(host, name)`: Queries the active limit or constant in force for the session.
  - Recognized keys: `"attack"`, `"defense"`, `"life_points"`, `"life_points_max"`, `"two_player_start"`, `"two_player_max"`, `"two_player_step"`, `"starchips"`, `"chest"`, `"free_duel_record"`, `"two_player_record"`, `"deck_copies"`, `"swords_turns"`, `"crush_card"`, `"spellbinding_circle"`, `"shadow_spell"`, `"rank_score.start"`, `"rank_score.exodia"`, `"rank_score.deck_out"`, `"starchip_prize.S"`, `"starchip_prize.A"`, `"starchip_prize.B"`, `"starchip_prize.C"`, `"starchip_prize.D"`, `"new_game_starchips"`, `"card_drops"`.

### 2.3.14 Dynamic Menu Identification (API 9)

- `host->menu_item(host, index)`: Converts a title menu item index into its identifier: retail entries `"new_game"` through `"save"` (0–10), or custom buttons `"mod-id:button-id"` (11+).

### 2.3.15 Dynamic Sound Synthesis & Audio Streaming (API 12)

- `host->sound_add(host, samples, frames, channels, rate)`: Uploads signed 16-bit interleaved PCM audio. Returns an integer sound handle, or `-1` on error.
- `host->sound_play(host, sound, volume, pan)`: Plays a sound handle with volume (0–255) and pan (-128 left to +128 right). Returns non-zero on success.
- `host->sound_free(host, sound)`: Stops playback and releases audio buffers.

---

## 2.4 Managed Gameplay Events Dispatcher

### 2.4.1 Event Flow, Priorities, and Control Fields

Event subscriptions use the `MemoriesModEvent` structure:

```c
typedef struct {
    unsigned type;   /* Event ID: MEMORIES_EVENT_* */
    unsigned phase;  /* MEMORIES_BEFORE or MEMORIES_AFTER */
    int a, b, c;     /* Context-dependent parameters */
    int result;      /* Operation return value */
    int handled;     /* Set to 1 in BEFORE phase to short-circuit */
} MemoriesModEvent;

typedef void (*MemoriesModCallback)(MemoriesModEvent *event);
```

#### Dispatch Rules

1. **`MEMORIES_BEFORE`**: Callbacks run in descending priority order. Any callback may modify `event->a`, `b`, `c`, or `result`. Setting `event->handled = 1` immediately aborts remaining before callbacks and bypasses the retail engine routine.
2. **Execution**: The retail engine routine executes using arguments passed through before-callbacks (unless handled).
3. **`MEMORIES_AFTER`**: Callbacks observe final outcome. Any edits made to `event` in this phase are ignored by the engine.

### 2.4.2 Complete Event Catalog

| Event Constant             | Val | Arguments & Behavior                                                                                                                                                                                                 |
| -------------------------- | --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `MEMORIES_EVENT_INPUT`     | 0   | `a`: Controller port (0 or 1). `b`: Input button bitmask. Before-callbacks can mutate `b` or override `result`. Runs during VBlank interrupt context: no allocation or disk I/O permitted.                           |
| `MEMORIES_EVENT_DAMAGE`    | 1   | `a`: Side (0 = player, 1 = CPU). `b`: Damage amount. `c`: Damage kind (0 = Battle, 1 = Spell / Reflected Effect). Initial `result`: Current Life Points. Handled: Replaces remaining LP. After: Observes clamped LP. |
| `MEMORIES_EVENT_REWARD`    | 2   | `a`: Awarded Card ID. Before: Change `a` to substitute reward, or set `handled = 1` to cancel reward entirely.                                                                                                       |
| `MEMORIES_EVENT_FUSION`    | 3   | `a`: First Card ID. `b`: Second Card ID. Before: Set `handled = 1` and `result = Card ID` to force a specific fusion, or `result = 0` to explicitly forbid fusion.                                                   |
| `MEMORIES_EVENT_EFFECT`    | 4   | Multi-frame spell/card effect step. Start: `a` = Card ID, `b` = handler flag, `c` = 0. Update: `a` = Card ID, `b` = state flags, `c` = 1. `result`: returned effect flags.                                           |
| `MEMORIES_EVENT_AI`        | 5   | Wraps AI decision selector (`func_800279BC`). Handled: Caller takes complete control of the AI decision record and supplies `result`.                                                                                |
| `MEMORIES_EVENT_SCENE`     | 6   | Wraps main scene transitions (`Main_ApplyMenuSelection`). `a`: Menu selection ID. `b`: Previous main mode. Handled: Intercepts and customizes scene routing.                                                         |
| `MEMORIES_EVENT_SAVE`      | 7   | Dispatched immediately before save-state serialization. Used to pack dynamic pointers and state into registered memory buffers.                                                                                      |
| `MEMORIES_EVENT_LOAD`      | 8   | Dispatched immediately after save-state restoration. Rebuild internal indices, caches, and runtime bindings.                                                                                                         |
| `MEMORIES_EVENT_SETTINGS`  | 9   | Dispatched after options change. `a`: Mod index. `b`: Setting option index. `c`: New applied integer value.                                                                                                          |
| `MEMORIES_EVENT_EQUIP`     | 10  | `a`: Equip Card ID. `b`: Target Monster Card ID. Handled: `result = 1` permits equip; `result = 0` forbids equip. Overrides gameplay tables.                                                                         |
| `MEMORIES_EVENT_SLOT_SAVE` | 11  | After-only. Dispatched when save slot menu saves game. `a`: Slot index (from 0). `b`: Slot token. `c`: Save sequence counter.                                                                                        |
| `MEMORIES_EVENT_SLOT_LOAD` | 12  | After-only. Dispatched when save slot menu finishes restoring game data into RAM.                                                                                                                                    |
| `MEMORIES_EVENT_STARCHIP`  | 13  | Post-duel StarChip prize award (`Mods_AwardStarchips`). `a`: StarChip prize amount. Edit `a` to adjust reward, or set `handled = 1` to skip award. After: `result` contains balance credited.                        |
| `MEMORIES_EVENT_MENU`      | 14  | Dispatched when a title menu item is selected. `a`: Item index (0–10 retail, 11+ custom mod buttons). `b`: Menu index (0 = first, 1 = second). `c`: Item value. Handled: `result` specifies custom exit transition.  |
| `MEMORIES_EVENT_MONSTER`   | 15  | Field monster lifecycle trigger. `a`: Card ID. `b`: Duel record index (0–29: `b / 15` is side, `b % 15` is zone 5–9). `c`: Trigger phase (`MEMORIES_MONSTER_*`). Handled: Skips declarative monster effects.         |

#### Monster Trigger Phases (`event->c`)

- `MEMORIES_MONSTER_SUMMON` (0): Face-up summon, fusion output, or ritual monster arrival.
- `MEMORIES_MONSTER_FLIP` (1): Attacked while face-down (resolves after damage step, before destruction).
- `MEMORIES_MONSTER_DRAW` (2): Beginning of owner's turn after drawing.
- `MEMORIES_MONSTER_COMBAT` (3): Initiating attack or being attacked, prior to damage calculation.
- `MEMORIES_MONSTER_DESTROYED` (4): Destruction by battle or card effect.
- `MEMORIES_MONSTER_DESTROY_OPPONENT` (5): Victorious monster after destroying opponent's monster in battle.

---

## 2.5 Function Hooking Subsystem Details

Hooking allows any C function within `src/game/` to be intercepted:

```c
static void (*original_draw)(void);

static void my_draw_field(void) {
    /* Custom logic before */
    original_draw();
    /* Custom logic after */
}

/* In MemoriesModInit */
host->hook(host, Duel_DrawFieldCards, my_draw_field, (void**)&original_draw);
```

- **Storage Requirement**: The `original` pointer passed to `host->hook` must reside in static or persistent memory, not on the stack. The host dynamically rewrites `*original` when mods are activated or deactivated.
- **Recursion Avoidance**: Calling `original(...)` routes directly to the next hook in the chain or the original game code without triggering the caller's hook again.

## 2.6 Save States & Binary Fingerprinting

Save states include a security and compatibility header:

- The loader hashes active mod IDs, version numbers, manifest settings, and the compiled executable code sections.
- Debug symbols and comments are excluded from the hash, ensuring recompilations of the same source code remain save-state compatible.
- Mismatched mod sets reject the save state cleanly with an informative dialog rather than corrupting game memory.

---

# Part 3: Data Mods & Asset Replacements

## 3.1 Virtual Disc Replacement (`data`)

Mods can replace any raw CD-ROM sector without unpacking the disc:

```json
"data": [
  { "file": "\\DATA\\CARD_NAME.BIN;1", "replace": "files/CARD_NAME.BIN" }
]
```

- Sectors are intercepted in the virtual CD drive handler (`libds.c`).
- Sector requests are satisfied from replacement files with zero memory allocation, operating safely within interrupt contexts.

## 3.2 Named Asset Replacement (`assets`)

Assets can be overridden by symbolic name without hardcoded disc sector addresses:

```json
"assets": {
  "title_logo": "images/custom_logo.png",
  "folder": "textures/cards"
}
```

## 3.3 High-Definition Texture Packs (`textures`)

HD texture packs replace internal PSX VRAM pages with 32-bit PNGs:

```json
"textures": {
  "rank": 4,
  "manifest": "textures/manifest.json"
}
```

Textures are automatically sampled at 2x, 4x, or 8x internal rendering resolutions when console upscaling is enabled.

## 3.4 Audio Streams & Sound Effects (`audio`)

Replaces BGM tracks and sound effects with WAV, OGG, or FLAC audio:

```json
"audio": {
  "29520": "music/custom_duel_theme.ogg"
}
```

## 3.5 Text Replacement & Multi-Language Translations (`text`, `font`)

Custom language translations can be packaged as data mods:

```json
"text": ["lang/dialog_es.txt"],
"font": "fonts/custom_font.ttf"
```

Replaces dialogue, menu text, and card descriptions with complete UTF-8 character encoding support.

---

# Part 4: The FM Editor — Complete Tweaks & Systems Guide

The standalone editor in `tools/pc/fm_editor` provides graphical tools to manage every modifiable parameter of the engine.

---

## 4.1 Cards Tab (Card Expansion, Attributes, & Logic)

![Cards Tab Architecture](https://raw.githubusercontent.com/Unchiga/Yu-Gi-Oh-Forbidden-Memories-Recompiled/master/docs/screenshots/fm-editor/cards.png)

### 4.1.1 Card Extension vs Replacement (`copy` vs `replace`)

- **`replace`**: Overrides a retail card (1–722). Keeps the retail card's ID and drop placements while modifying name, stats, artwork, type, stars, or effects.
- **`copy`**: Creates a brand new card with an ID starting at 723 up to 32,766. Inherits all unassigned fields from its specified base card. Requires a stable `"id"` string for persistent saves across changing mod load orders.

### 4.1.2 Stats, Types, Attributes, Levels, & Guardian Stars

- **ATK & DEF**: 0 to 5,110 (printed stats are stored as 9-bit values multiplied by 10). Stats can be scaled up to 32,767 in-game via limits.
- **Level**: 0 to 12 stars.
- **Types**: All retail types (Dragon, Spellcaster, Zombie, Warrior, Beast-Warrior, Beast, Winged Beast, Fiend, Fairy, Insect, Dinosaur, Reptile, Fish, Sea Serpent, Machine, Thunder, Aqua, Pyro, Rock, Plant, Magic, Trap, Ritual, Equip) plus custom types (24–255).
- **Attributes**: Light, Dark, Water, Fire, Earth, Wind, plus custom attributes (8–255).
- **Guardian Stars**: Primary and secondary stars chosen from Mars, Jupiter, Saturn, Uranus, Pluto, Neptune, Mercury, Sun, Moon, Venus, plus custom stars (11–15). Single-star cards use the same star for both or set the second to none.

### 4.1.3 Type Switching & Cross-Type Morphing

Cards are not restricted to their retail categories:

- A monster can be changed into a Magic, Trap, Ritual, or Equip card by setting its type and binding an appropriate retail `effect`.
- A spell can be changed into a monster, receiving ATK/DEF, level, and guardian stars.

### 4.1.4 3D Models, Sound Effects, & Exodia Logic

- **`model`**: Selects the 3D polygon model used in battle animations (1–722).
- **`effect`**: Selects the card effect script executed during play (e.g. Raigeki, Dark Hole).
- **`exodia`**: Boolean flag. When set on a replacement for cards 17–21, preserves instant-win condition behavior.

### 4.1.5 Attack Trap Threshold Overrides per Card

The editor allows setting `trap_threshold` directly on an individual card (0–65,535 ATK):

```json
{
  "replace": "Bear Trap",
  "trap_threshold": 2000
}
```

Overrides global attack trap thresholds on a per-card basis.

### 4.1.6 The Monster Effects System

Declarative triggered abilities attached to monsters via `"monster_effects"`:

```json
"monster_effects": [
  {
    "when": "summon",
    "do": "boost",
    "target": "own",
    "attack": 500,
    "defense": 500,
    "type": "Dragon"
  },
  {
    "when": "flip",
    "do": "destroy",
    "target": "battle"
  },
  {
    "when": "draw",
    "do": "heal",
    "amount": 300
  },
  {
    "when": "combat",
    "do": "boost",
    "target": "self",
    "attack": 1000
  },
  {
    "when": "destroyed",
    "do": "damage",
    "amount": 500
  },
  {
    "when": "face_up",
    "do": "boost",
    "target": "own",
    "attack": 300,
    "for_each": { "whose": "all", "attribute": "Dark" }
  }
]
```

#### Monster Effect Parameter Reference

- **`when`**: `summon` (face-up play), `flip` (attacked face-down), `draw` (owner's draw phase), `combat` (pre-battle engagement), `destroyed` (battle or effect removal), `destroy_opponent` (after destroying opposing monster), `face_up` (continuous static field aura).
- **`do`**: `boost` (modify ATK/DEF), `heal` (restore LP), `damage` (inflict direct LP damage), `destroy` (remove monsters), `magic` (trigger an existing spell effect by ID), `battle_immunity` (API 19: cannot be destroyed by battle while face up), `piercing` (API 19: piercing battle damage when attacking a defense position monster).
- **`target`**: `self`, `own`, `others`, `opponent`, `all`, `battle`.
- **`for_each`**: Dynamic scaling multiplier counting qualifying monsters on `own`, `opponent`, or `all` field zones.

---

### 4.1.7 Mod API 19: Modular Conditions, Battle Immunity, & Piercing Damage System

Mod API 19 introduces extensible monster effects conditions, the battle immunity effect (`do: "battle_immunity"`), and the piercing battle damage effect (`do: "piercing"`).

#### 1. Cannot Be Destroyed by Battle (`do: "battle_immunity"`)

- **Trigger**: Configured under `when: "face_up"` (continuous effect while the monster is face up on the field).
- **Effect Behavior**:
  - The monster cannot be destroyed by battle as an attacker or defender.
  - Damage calculation still applies normally: if the monster's ATK/DEF is lower than the opposing monster's ATK, the difference is deducted from the player's Life Points as standard battle damage.
  - Non-battle destruction still applies normally: spells (e.g. Raigeki), traps, and monster destruction effects remove the card from the field as usual.
  - When battle destruction would occur, the PC runtime engine intercepts the internal game destruction registers (`D_8009B1B0` for 2D duel view, changed from `-1` [destroyed] to `1` [struck and stays]; `D_8009B208` / `D_8009B209` for 3D battle arena, cleared to `-1`). This prevents destruction without modifying any decompiled retail game C code.

#### 2. Cannot Be Destroyed by Battle Modifiers (`modifier`, `reset`, attacker criteria)

Attack count protections and attacker filtering criteria are strictly **battle immunity modifiers**, not general conditions:
- **C Implementation & Mod ABI Preservation**:
  - To preserve binary compatibility for mods built against v0.3.0 (where `sizeof(MonsterEffect)` is exactly 20 bytes), `MonsterEffect` remains at its original 20-byte size.
  - The API 19 extended data (modifiers, attacker criteria, and conditions) is stored in a separate parallel table indexed identically (`MonsterEffectEx`), queried via `Cards_MonsterEffectsEx(int id, const MonsterEffectEx **effects_ex)` and `Cards_CardEffectsEx(int id, const MonsterEffectEx **effects_ex)`.
  - The existing accessors `Cards_MonsterEffects` and `Cards_CardEffects` continue to return the 20-byte `MonsterEffect` array so legacy and v0.3.0 mods iterating through it read the correct entries without offset skew.
  - `MonsterEffect_DoNames` growth from 5 to 7 is accepted in `tools/pc/mod_compat.txt` for the added `battle_immunity` and `piercing` actions.
  - Fields in `MonsterEffectEx`: `battle_modifier`, `battle_reset`, `attacker_type`, `attacker_attribute`, `attacker_stat`, `attacker_stat_op`, `attacker_amount`, `attacker_level`, `attacker_level_op`, `cond_count`, `cond_mode`, `conditions`.
  - `MONSTER_BATTLE_MOD_ALWAYS` (`"always"`, default): Unconditional battle immunity while conditions are met.
  - `MONSTER_BATTLE_MOD_FIRST_ATTACKED` (`"first_attacked"`): Survives 1 attack, destroyed on the 2nd attack.
  - `MONSTER_BATTLE_MOD_AFTER_2_ATTACKS` (`"after_2_attacks"` / `"destroyed_after_2_attacks"`): Survives up to 2 attacks, destroyed on the 3rd attack.
  - `MONSTER_BATTLE_MOD_AFTER_3_ATTACKS` (`"after_3_attacks"` / `"destroyed_after_3_attacks"`): Survives up to 3 attacks, destroyed on the 4th attack.
  - `MONSTER_BATTLE_MOD_ATTACKED_BY_TYPE` (`"attacked_by_type"` / `"attacked_type"`): Battle immunity applies only if attacked by a monster of the specified monster type (`attacker_type` / `type`).
  - `MONSTER_BATTLE_MOD_ATTACKED_BY_STAT` (`"attacked_by_stat"` / `"attacked_stat"`): Battle immunity applies only if attacked by a monster with higher or lower ATK or DEF than a threshold.
    - Configured via:
      - `attacker_stat`: `"atk"` or `"def"`
      - `attacker_stat_op`: comparison operator (`"<"`, `"<="`, `">"`, `">="`, `"=="`; default `">"`). In UI: "Higher than (>)", "Lower than (<)", "Higher or equal (>=)", "Lower or equal (<=)", "Equal to (=)".
      - `attacker_amount`: threshold integer (0–65535).
    - Uses effective in-battle calculated stats via `Duel_CalcCardStats()` and modular `compare_op()`.
  - `MONSTER_BATTLE_MOD_ATTACKED_BY_LEVEL` (`"attacked_by_level"` / `"attacked_level"`): Battle immunity applies only if attacked by a monster with higher or lower Level than a threshold.
    - Configured via:
      - `attacker_level_op`: comparison operator (`"<"`, `"<="`, `">"`, `">="`, `"=="`; default `">"`). In UI: "Higher than (>)", "Lower than (<)", "Higher or equal (>=)", "Lower or equal (<=)", "Equal to (=)".
      - `attacker_level`: Level threshold integer (1–12).
    - Modularly evaluated using `Cards_Level()` and `compare_op()`.
  - `MONSTER_BATTLE_MOD_ATTACKED_BY_MONSTER` (`"attacked_by_monster"` / `"attacked_by"`): Combined attacker filtering modifier. Checks all configured criteria together (attacker type, attribute, higher/lower ATK/DEF than amount, and higher/lower Level than amount).

```json
{
  "when": "face_up",
  "do": "battle_immunity",
  "modifier": "attacked_by_monster",
  "attacker_type": "Dragon",
  "attacker_attribute": "LIGHT",
  "attacker_stat": "atk",
  "attacker_stat_op": "<",
  "attacker_amount": 2500,
  "attacker_level_op": ">=",
  "attacker_level": 7
}
```

> [!IMPORTANT]
> **Attacker vs. Defender Rule for Battle Modifiers**:
> Battle immunity modifiers (attack counts as well as attacker criteria) are evaluated exclusively when this card is **attacked** as a defender in combat (`record == S.trap_battle_record[1]`). When this card is the attacker initiating an attack (`record == S.trap_battle_record[0]`), battle immunity does not apply, preventing attacking monsters from surviving counter-attacks while preserving defender-only shielding against matching opposing monsters.

> [!IMPORTANT]
> **Single Attack Count Modifier Rule**:
> Monsters can **ONLY have one battle immunity effect with an attack count modifier** (`first_attacked`, `destroyed_after_2_attacks`, `destroyed_after_3_attacks`) across all their effects. Attacker criteria modifiers (`attacked_by_type`, `attacked_by_attribute`, `attacked_by_stat`, `attacked_by_level`, `attacked_by_monster`) do not count towards this limit and can be freely used without restriction. The FM Editor and runtime engine enforce this distinction across dialogs and manifest diagnostics.

#### 3. Piercing Battle Damage (`do: "piercing"`)

- **Trigger**: Configured under `when: "face_up"` (continuous effect active while the monster is face up on the field).
- **Target**: `target: "self"` (default).
- **Effect Behavior**:
  - When this monster attacks an opponent's monster that is in **Defense Position** (`D_8009B178[1] & 0x800`), standard damage calculation is performed:
    - If the attacking monster's ATK exceeds the defending monster's DEF (`attacker_atk > defender_def`), the difference (`diff = attacker_atk - defender_def`) is inflicted directly to the defending player's Life Points (`Mods_DamageLife`).
    - The battle damage popup is triggered via `D_8009B1A4[1] = diff`, and duel statistics are updated (`rank.effective_attacks++`).
    - If the attacking monster's ATK is less than or equal to the defending monster's DEF, no piercing damage is dealt (defense recoil damage still applies as normal retail behavior).
    - If the defending monster is in **Attack Position**, standard retail battle damage calculation applies (piercing logic does not trigger).
  - **Visual & Audio Presentation during 2D Battle (Zoomed-in Screen)**:
    - During battle when both cards are zoomed in on screen, retail's battle sequencer (`src/game/duel_scene_battle.c`) executes State 8 for hit/damage presentation.
    - For Defense Position monsters without piercing damage, retail leaves `D_8009B1A4[1]` at 0 and Effect 2 (`src/overlays/duel_effects/effect_2.c`) defaults to config 6 (blunt defense sparks without a slash animation and with the damage number omitted).
    - When piercing damage is inflicted:
      - The piercing damage amount `S.piercing_damage` is maintained in `D_8009B1A4[1]` across battle states 4 through 8.
      - In State 8, retail allocates `req = DuelEffect_AllocateRequest(2)` for the defender (`D_8009B1B9 == 1`) at card coordinates `(0xF0, 0x78)`.
      - `req->field_12` receives the piercing damage value `diff`, and `req->field_1A` is configured with the slash tier (`diff / 1000`: 0 for light slash, 1 for medium slash, 2 for heavy slash).
      - The corresponding slash sound effect `SD_SEPlayFull(req->field_1A + 0x10)` is played (SE 0x10, 0x11, or 0x12).
      - In Effect 2 (`func_80153200`), because `number != 0`, `work->config` is initialized to the appropriate slash configuration `&D_8015AF18[req->field_1A]`, `work->number_color` is initialized to red, and `func_801566D4(-number, ...)` renders the damage number in red on screen.
      - The slash polygon is rendered across the second card with matching particle sparkles and card shake, reproducing the identical effect as if attacking an Attack Position monster.
  - **Single-Application Lifecycle & Multi-Frame Safety**:
    - Battle damage calculation is scoped strictly to presentation states 4 through 8 (`battle_step >= 4 && battle_step <= 8`).
    - The engine sets `S.piercing_applied = 1` immediately upon the first damage application so subsequent frames of the combat exchange never re-apply damage.
    - Multi-frame battle cleanup states (such as State 11 sprite fade-outs, which run across 16–32 consecutive frames) are barred from resetting `S.piercing_applied` or calculating damage, preventing unintended multi-hit LP drain.
    - Piercing damage state is cleanly reset only when beginning a new combat exchange in `MonsterEffects_Battle()` or when transitioning out of `PHASE_BATTLE`.
  - The runtime engine intercepts battle states in `MonsterEffects_Update()` within the battle phase, applying damage, popups, and stats seamlessly without modifying any decompiled retail game C code.

#### 4. Piercing Battle Damage Modifiers (`modifier`, defender criteria)

Just like battle immunity modifiers, piercing damage filtering criteria are strictly **piercing modifiers**, not general conditions:
- **No Condition List Presence**: They are excluded from `MonsterConditionType` in C and do not appear in the conditions list (neither in the JSON `conditions` array nor visually in the FM Editor's Conditions list/dialog).
- **Strict Effect Scope**: They apply **exclusively** to `do: "piercing"`.
- **C Implementation**: Stored directly on `MonsterEffect` (`piercing_modifier`, `defender_type`, `defender_attribute`, `defender_stat`, `defender_stat_op`, `defender_amount`, `defender_level_op`, `defender_level`).
  - `MONSTER_PIERCING_MOD_ALWAYS` (`"always"`, default): Piercing damage applies whenever this monster attacks any defense position monster (as long as face-up conditions are satisfied).
  - `MONSTER_PIERCING_MOD_ATTACKS_TYPE` (`"attacks_type"` / `"defender_type"`): Piercing damage applies only if the defending monster is of the specified monster type (`defender_type` / `card_type` / `type`).
  - `MONSTER_PIERCING_MOD_ATTACKS_ATTRIBUTE` (`"attacks_attribute"` / `"defender_attribute"`): Piercing damage applies only if the defending monster is of the specified attribute (`defender_attribute` / `attribute`).
  - `MONSTER_PIERCING_MOD_ATTACKS_STAT` (`"attacks_stat"` / `"defender_stat"`): Piercing damage applies only if the defending monster has higher, lower, or equal ATK or DEF compared to a threshold value:
    - Configured via:
      - `defender_stat`: `"atk"` or `"def"`
      - `defender_stat_op`: comparison operator (`"<"`, `"<="`, `">"`, `">="`, `"=="`; default `">"`). In UI: "Higher than (>)", "Lower than (<)", "Higher or equal (>=)", "Lower or equal (<=)", "Equal to (=)".
      - `defender_amount`: threshold integer (0–65535).
    - Uses effective in-battle calculated stats via `Duel_CalcCardStats()` and modular `compare_op()`.
  - `MONSTER_PIERCING_MOD_ATTACKS_LEVEL` (`"attacks_level"` / `"defender_level"`): Piercing damage applies only if the defending monster has higher, lower, or equal Level compared to a threshold value:
    - Configured via:
      - `defender_level_op`: comparison operator (`"<"`, `"<="`, `">"`, `">="`, `"=="`; default `">"`). In UI: "Higher than (>)", "Lower than (<)", "Higher or equal (>=)", "Lower or equal (<=)", "Equal to (=)".
      - `defender_level`: Level threshold integer (1–12).
    - Modularly evaluated using `Cards_Level()` and `compare_op()`.
  - `MONSTER_PIERCING_MOD_ATTACKS_MONSTER` (`"attacks_monster"` / `"defender_monster"`): Combined defender filtering modifier. Checks all configured defender criteria together (defender type, attribute, higher/lower ATK/DEF than amount, and higher/lower Level than amount).

```json
{
  "when": "face_up",
  "do": "piercing",
  "modifier": "attacks_monster",
  "defender_type": "Dragon",
  "defender_attribute": "LIGHT",
  "defender_stat": "def",
  "defender_stat_op": "<=",
  "defender_amount": 2000,
  "defender_level_op": "<=",
  "defender_level": 6
}
```

> [!IMPORTANT]
> **Attacker vs. Defender Rule for Piercing Modifiers**:
> Piercing damage modifiers (defender criteria) are evaluated exclusively when this card is the **attacker** initiating combat (`record == S.trap_battle_record[0]`). If this card is attacked by an opposing monster, piercing damage does not apply.

#### 5. The Modular Conditions Engine (`conditions`, `condition_mode`)

Continuous effects under `when: "face_up"` (`do: "boost"`, `do: "battle_immunity"`, and `do: "piercing"`) can specify conditions that must be satisfied for the effect to remain active:

```json
{
  "when": "face_up",
  "do": "piercing",
  "modifier": "attacks_stat",
  "defender_stat": "def",
  "defender_stat_op": "<=",
  "defender_amount": 1500,
  "conditions": [
    {
      "type": "control_type",
      "card_type": "Dragon",
      "mode": "at_least_one"
    }
  ],
  "condition_mode": "all"
}
```

- **Strict Face-Up Scope (`when: "face_up"`)**: Conditions (`"conditions"`) and condition combinator mode (`"condition_mode"`) are strictly exclusive to continuous face-up monster effects (`when: "face_up"` on monsters):
  - Spells and traps (`card_effects`) cannot have conditions; specifying `"conditions"` or `"condition_mode"` on a spell/trap emits a diagnostic note (`cards[%d]: card_effects[%d]: a spell or trap cannot have conditions`) and rejects the entry.
  - Non-face-up monster effects (`summon`, `flip`, `draw`, `combat`, `destroyed`, `destroy_opponent`) cannot have conditions; specifying `"conditions"` or `"condition_mode"` emits a diagnostic note (`cards[%d]: monster_effects[%d]: "conditions" goes only with "face_up"`) and rejects the entry.
  - In the FM Editor, the effect normalizer enforces this constraint and rejects any effect definition outside `when: "face_up"` that contains conditions or condition_mode.
- **`condition_mode`**: Logical combinator when multiple conditions are present.
  - `"all"` (default): Logical AND — every condition in the list must be true.
  - `"any"`: Logical OR — at least one condition in the list must be true.
- **Capacity**: Up to 4 conditions per effect (`MONSTER_CONDITIONS_MAX = 4`) inside the canonical `"conditions"` array.
- **Strict Canonical Naming**: All undocumented and legacy aliases have been pruned across the engine parser and FM editor. Unrecognized keys, condition types, operators, and modifiers emit a diagnostic note (`Mods_Note`) and are rejected. Canonical names support case-insensitive, hyphen-insensitive, and underscore-insensitive variations handled automatically by `named()`.
- **Stat Recursion Guard**: Because conditions like `lowest_atk` and `stat_threshold` query current effective stats via `Duel_CalcCardStats()`, the engine employs a re-entrancy flag (`s_evaluating_stats`) inside `MonsterEffects_Stats()` to prevent infinite recursion during condition evaluation.

#### 6. Condition Types Specification

| Condition Type | Parameters | Evaluation Logic |
| -------------- | ---------- | ---------------- |
| **`named_card`** | `card`: Card ID or name<br>`whose`: `"all"` (default), `"own"`, or `"opponent"` | Evaluates to true if the specified card is currently face-up on the field within the designated field zone (`"own"`, `"opponent"`, or `"all"`). |
| **`stat_threshold`** | `stat`: `"atk"` (default) or `"def"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`<br>`value`: Threshold integer (0–9999) | Compares this monster's current effective ATK or DEF against `value` using the specified comparison operator. |
| **`lowest_atk`** | _(none)_ | Evaluates to true if this monster has strictly lower current ATK than every other face-up monster on the field. If another face-up monster has equal or lower ATK, ties disqualify and the condition evaluates to false. |
| **`highest_atk`** | _(none)_ | Evaluates to true if this monster has strictly higher current ATK than every other face-up monster on the field. Ties disqualify and evaluate to false. |
| **`fewer_monsters`** | _(none)_ | Evaluates to true if you (the controller of this card) currently control strictly fewer monsters in the monster zone than your opponent (`own_count < opp_count`). Having 0 monsters is explicitly taken into consideration (e.g. 0 < 1 is true). If both players control equal numbers of monsters (ties such as 0 == 0 or 1 == 1), evaluates to false. |
| **`more_monsters`** | _(none)_ | Evaluates to true if you (the controller of this card) currently control strictly more monsters in the monster zone than your opponent (`own_count > opp_count`). Having 0 monsters is explicitly taken into consideration (e.g. 1 > 0 is true). If both players control equal numbers of monsters (ties such as 0 == 0 or 1 == 1), evaluates to false. |
| **`control_stat`** | `presence`: `"control"` (default) or `"not_control"`<br>`scope`: `"own"` (default) or `"other"` (exclude self)<br>`stat`: `"atk"` (default) or `"def"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`, `"=="`<br>`value`: Threshold integer (0–9999) | Evaluates to true if you control (or do not control, if `presence: "not_control"`) a face-up monster satisfying the ATK or DEF comparison (`<`, `<=`, `>`, `>=`, `==`). When `scope` is `"other"`, only other monsters you control are evaluated (excluding this card itself). |
| **`control_level`** | `presence`: `"control"` (default) or `"not_control"`<br>`scope`: `"own"` (default) or `"other"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`, `"=="`<br>`level`: Level integer (1–12) | Evaluates to true if you control (or do not control, if `presence: "not_control"`) a face-up monster with the specified Level comparison. Modularly leverages `Cards_Level(id)`. When `scope` is `"other"`, ignores this card itself. |
| **`control_type`** | `whose`: `"own"` (default), `"opponent"`, or `"all"`<br>`presence`: `"control"` (default) or `"not_control"`<br>`scope`: `"own"` (default) or `"other"` (exclude self)<br>`card_type`: Monster type string or ID (e.g. `"Dragon"`)<br>`mode`: `"at_least_one"` (default) or `"only"` | Evaluates to true if a monster of the specified Type is present (or is not present, if `presence: "not_control"`) on the designated field (`"own"`, `"opponent"`, or `"all"`). If `mode: "at_least_one"`, true if at least one monster matches. If `mode: "only"`, true only if all monsters on that field (with total count > 0) are of that type. When `scope` is `"other"`, ignores this card itself. |
| **`control_attribute`** | `whose`: `"own"` (default), `"opponent"`, or `"all"`<br>`presence`: `"control"` (default) or `"not_control"`<br>`scope`: `"own"` (default) or `"other"` (exclude self)<br>`attribute`: Monster attribute string or ID (e.g. `"LIGHT"`, `"DARK"`)<br>`mode`: `"at_least_one"` (default) or `"only"` | Evaluates to true if a monster of the designated Attribute is present (or is not present, if `presence: "not_control"`) on the designated field (`"own"`, `"opponent"`, or `"all"`). When `scope` is `"other"`, ignores this card itself. |
| **`field_spell`** | `field`: `"any"` (default), `"forest"`, `"wasteland"`, `"mountain"`, `"sogen"`, `"umi"`, or `"yami"` (or retail card IDs 330–335)<br>`presence`: `"active"` (default) or `"not_active"` | Evaluates to true if a field spell is currently active on the field, or if a specific field spell is active or not active:<br>• **When `field` is `"any"`**: If `presence` is `"active"`, evaluates to true if any field spell is active on the field (`gDuel_bTerrain != 0`, i.e. 1..6). If `presence` is `"not_active"`, evaluates to true if no field spell is active (`gDuel_bTerrain == 0`, normal arena terrain).<br>• **When `field` is a specific field spell** (Forest, Wasteland, Mountain, Sogen, Umi, Yami): If `presence` is `"active"`, evaluates to true only if that specific field spell is currently active (`gDuel_bTerrain == field_id`). If `presence` is `"not_active"`, evaluates to true if any other terrain is active or if no field spell is active (`gDuel_bTerrain != field_id`).<br>• **Validation**: Unrecognized field spell names (e.g. a typo such as `"mountian"`) emit a `Mods_Note` and are rejected during loading rather than silently defaulting to `"any"`. |
| **`lower_lp`** | _(none)_ | Evaluates to true if you (the controller of this card) currently have strictly lower Life Points than your opponent (`my_lp < opp_lp`). If both players have equal Life Points (a tie, including 8000 == 8000), evaluates to false. |
| **`higher_lp`** | _(none)_ | Evaluates to true if you (the controller of this card) currently have strictly higher Life Points than your opponent (`my_lp > opp_lp`). If both players have equal Life Points (a tie, including 8000 == 8000), evaluates to false. |

> [!NOTE]
> **Slot Occupancy & Re-entrancy Safety in Conditions**:
> • Condition evaluations verify slot occupancy via `slot_has_monster()` checking `D_801A7AD8[r].flags & DUEL_CARD_FLAG_OCCUPIED`, ensuring empty board zones with stale `S.card` data are never falsely counted as monsters.
> • Stat comparisons employ `s_evaluating_stats` re-entrancy guards to prevent recursion loops.

> [!NOTE]
> **Zero Monsters and Ties in Monster Count and LP Comparisons**:
> • When evaluating `fewer_monsters` and `more_monsters`, having no monsters on a side is treated as controlling 0 monsters (e.g., `1 > 0` is true, `0 < 1` is true). Equal monster counts (ties such as `0 == 0`, `1 == 1`) evaluate to false.
> • When evaluating `lower_lp` and `higher_lp`, equal Life Points (a tie, e.g. `8000 == 8000` at the start of a duel) evaluate to false. Only strictly lower (`my_lp < opp_lp`) or strictly higher (`my_lp > opp_lp`) activates the condition.

#### 7. FM Editor UI Integration

The FM Editor (`tools/pc/fm_editor`) fully supports API 19 features:

- **`Does` Dropdown**: Includes `"Cannot be destroyed by battle"` and `"Piercing damage"` alongside standard actions when `"While face up"` is selected.
- **Dedicated `Modifiers` Field**:
  - Positioned directly **on top of the Conditions section** in `EffectDialog`.
  - Appears whenever `"Cannot be destroyed by battle"` or `"Piercing damage"` is selected (and is hidden for `boost` and all other actions).
  - **Battle Immunity Modifiers**:
    - **`Always`** (default): Grants unconditional battle immunity while face up.
    - **`The first time it is attacked`**: Survives 1 attack, destroyed on the 2nd attack.
    - **`Can only be destroyed after two attacks`**: Survives up to 2 attacks, destroyed on the 3rd attack.
    - **`Can only be destroyed after three attacks`**: Survives up to 3 attacks, destroyed on the 4th attack.
    - **`If attacked by monster of Type`**: Dynamically displays **Attacker type** picker.
    - **`If attacked by monster of Attribute`**: Dynamically displays **Attacker attribute** picker.
    - **`If attacked by monster with higher / lower ATK or DEF`**: Dynamically displays **Attacker stat** (`ATK`/`DEF`), comparison operator (`<`, `<=`, `>`, `>=`, `==`), and **Stat threshold** numeric input.
    - **`If attacked by monster with higher/lower level`**: Dynamically displays comparison operator and Level numeric input (1–12).
    - **`If attacked by monster (combined criteria)`**: Dynamically displays all attacker criteria inputs (Type, Attribute, Stat operator/threshold, and Level operator/threshold).
  - **Piercing Damage Modifiers**:
    - **`Always`** (default): Piercing damage applies against any defense monster while conditions are met.
    - **`If it attacks a monster of Type`**: Dynamically displays **Defender type** picker.
    - **`If it attacks a monster of Attribute`**: Dynamically displays **Defender attribute** picker.
    - **`If it attacks a monster with higher / lower ATK or DEF`**: Dynamically displays **Defender stat** (`ATK`/`DEF`), comparison operator (`<`, `<=`, `>`, `>=`, `==`), and **Stat threshold** numeric input.
    - **`If it attacks a monster with higher/lower level`**: Dynamically displays comparison operator and Level numeric input (1–12).
    - **`If it attacks a monster (combined criteria)`**: Dynamically displays all defender criteria inputs (Type, Attribute, Stat operator/threshold, and Level operator/threshold).
  - **`Resets` Dropdown**: Appears when an attack count modifier is chosen, offering `"In a turn"` (default), `"This card copy"`, and `"In a duel"`. Hidden for attacker and defender criteria modifiers.
  - **Clean Separation from Conditions**: Modifiers are serialized directly into `modifier` and corresponding attacker/defender fields on the effect object, completely separated from `conditions`. They do not appear in the conditions treeview or condition dialog.
- **Single Attack Modifier Enforcement**:
  - The editor enforces that each monster can only have at most one battle immunity effect with an attack count modifier across all its effects. Attacker/defender criteria modifiers do not conflict with attack count modifiers.
  - In `EffectDialog.read()`, saving a second attack count modifier is blocked with an error if another effect on the monster already has one.
  - In `problems()` validation, any card exceeding one attack count modifier triggers an immediate diagnostic warning: `"A monster can only have one condition related to a number of attacks."`
- **Horizontal & Vertical Scrolling & Expanded Layouts**:
  - **`EffectsBox`**: In the Cards tab, the effects treeview features expanded columns ([110, 420]) and hint wraplength (480px), paired with bidirectional scrolling.
  - **`EffectDialog`**: Configured with a wider default minimum width (`minsize(680px)`), `resizable(True, False)` for interactive horizontal stretching, 48-char dropdown widths across all action fields, wider description hints (640px wraplength), and an enlarged embedded condition treeview ([160, 460], 620px total) to ensure long condition descriptions remain legible without text clipping.
  - **`ConditionDialog`**: Configured with a wider default minimum width (`minsize(620px)`), `resizable(True, False)` for interactive horizontal stretching, 48-char dropdown widths, wider card field pickers (38-char), and wider condition hint text (580px wraplength).
- **Conditions List**: Shows a dedicated Treeview listing active conditions and details, with horizontal scrolling support, and `Add...`, `Edit...`, and `Remove` buttons.
- **Mode Selector**: Combobox toggles between `"All conditions must be met (AND)"` and `"Any condition can be met (OR)"`.
- **`ConditionDialog`**:
  - A modal dialog providing interactive parameter inputs for true conditions: card search/picker integration (`CardField`), stat threshold comparison with ATK and DEF (0–9999), monster count comparisons (`fewer_monsters`, `more_monsters`), presence toggle (`You control` vs `You do not control`), evaluation scope (`Any monster you control` vs `Another monster you control (exclude self)`), level comparisons (`<`, `<=`, `>`, `>=`, `==` against levels 1–12), type requirements (`At least one monster of this type` vs `Only control this type`), attribute selectors, and field spell selectors (all field spell options: Any field spell, Forest, Wasteland, Mountain, Sogen [Meadow], Umi [Sea], Yami [Dark], with `Is active` vs `Is not active` status). Attack counts are not in this dialog.

---

### 4.1.8 Card Text Engine, Inline Icons, & Text Colors

The text box automatically formats and wraps lines to 20 characters. Inline bytecode tokens can be inserted:

- **`{f8 0B NN}`**: Renders 16x16 attribute, type, or button icons (2 character cells wide).
- **`{f8 0A NN}`**: Switches font palette color (0: White, 1: Yellow, 2: Blue, 3: Green, 4: Grey, 5: Orange, 6: Red).
- **`{g X}`**: Inserts raw glyph index from the VRAM font sheet.

### 4.1.9 Developer Notes & Tag Extraction

Cards support a `"notes"` string for metadata and gameplay scripting:

```json
"notes": "Custom boss card. <burn: 500> <boss-tier: 3>"
```

Tags formatted as `<key: value>` are parsed at runtime using `host->card_tag`.

---

## 4.2 Art Tab (Card Artwork & Visual Pipeline)

### 4.2.1 Specifications, Palettes, and Quantization

- **Full Card Illustration**: 102 × 96 pixels, 255 indexed colors with 1 transparent color.
- **Hand / Field Thumbnail**: 40 × 32 pixels, 63 indexed colors with 1 transparent color.
- The editor automatically imports any standard 24-bit/32-bit PNG, crops to aspect ratio, and performs optimal color quantization.

### 4.2.2 High-Definition Rendering Support

Imported source PNGs are preserved at their native resolution. When the PC engine is configured to internal resolutions (2x, 4x, 8x), textures are rendered directly from high-resolution source images.

### 4.2.3 Title Plate Typography Generator

Card title plates (96 × 14 pixels) are rendered automatically using TrueType fonts, matching the kerning and styling of retail title banners.

### 4.2.4 3D Field Cutout Art (`field_art`)

Allows specifying an independent PNG rendered specifically for the 3D card cutout standing on the duel arena.

---

## 4.3 Fusions & Bulk Fusions Tab

### 4.3.1 Declarative Fusion Rules

Fusions can be added between any two cards:

```json
"fusions": [
  { "with": ["Dark Magician", "Dragon Piper"], "result": "Dark Cavalry" }
]
```

### 4.3.2 Non-Commutative Fusions & Fusion Blocking

- Setting `"result": null` explicitly removes or forbids a fusion.
- Specific card pairings take precedence over broad base-card fusions.

### 4.3.3 Table Clearing (`remove: "all"`)

Setting `{"remove": "all"}` wipes the entire retail fusion table, allowing modders to build custom fusion systems from scratch.

### 4.3.4 The Bulk Fusions Automation Engine

The FM Editor includes an automated bulk fusion generator:

- **CardFilter**: Selects subsets of cards based on Type, Attribute, ATK/DEF ranges, Level, or Fusion Group.
- Automatically generates all permutations or symmetric combinations of fusions between selected card sets.

---

## 4.4 Equips Tab

### 4.4.1 Eligibility Matrices (`add`, `remove`, `replace`)

Defines which cards or monster types an equip card can attach to:

```json
"equips": [
  {
    "card": "Sword of Dark Destruction",
    "add": ["Fiend", "Zombie"],
    "remove": ["Curse of Dragon"],
    "replace": false,
    "bonus": 400
  }
]
```

### 4.4.2 Stat Bonuses, Asymmetric Boosts, & Conditional Boosts

- **`bonus`**: Sets identical ATK and DEF additions (-9999 to 9999).
- **`bonus_attack` / `bonus_defense`**: Sets asymmetric values (e.g. +800 ATK, -200 DEF).
- **`bonus_if`**: Dynamic conditional bonus map keyed by monster type or attribute.

### 4.4.3 Global Default Bonus (`equip_bonus_default`)

Overrides the standard +500 boost across all unlisted equip cards.

---

## 4.5 Rituals Tab

### 4.5.1 Multi-Tribute Rituals (1 to 5 Tributes)

Ritual recipes can demand between 1 and 5 tributes (retail is locked to 3).

### 4.5.2 Tribute Origins: Field, Hand, and Both

Configured via `"tributes_from"`:

- `"field"`: Tributes must be face-up on the field.
- `"hand"`: Tributes are discarded directly from hand.
- `"both"`: Evaluates hand first, then field.

### 4.5.3 Condition-Based Tributes & Fusion Groups

Tributes can be specific card names or condition objects:

```json
"rituals": [
  {
    "card": "Black Luster Ritual",
    "result": "Black Luster Soldier",
    "tributes_from": "both",
    "tributes": [
      { "type": "Warrior", "min_level": 4 },
      { "fusion_group": "Elf" },
      { "defense_gt_attack": true }
    ]
  }
]
```

Supported fusion groups: `AngelWinged`, `Bugrothian`, `Egg`, `Elf`, `FeatherFromBear`, `FeatherFromHarpie`, `FeatherFromMachine`, `Female`, `Jar`, `Koumorian`, `MercuryMagicUser`, `MercurySpellcaster`, `Mirror`, `MusKingian`, `MystElfian`, `Rainbow`, `Sheepian`, `Thronian`, `Turtle`, `UsableBeast`.

---

## 4.6 Duelists Tab & Roster System

### 4.6.1 Roster File Hierarchy & Added Duelists

Duelists beyond retail 39 are organized in directories:

- `duelists/<id>.json`: Definition, base copy, name, AI, rank scoring, unlock rules.
- `decks/<id>.json`: 40-card weighted deck pool or fixed deck list.
- `drops/<id>.json`: Drop tables (POW, BCD, TEC).
- `portraits/<id>.png`: 48 × 48 duel portrait.

### 4.6.2 Free Duel Grid Placement (`slot`) & Collision Rules

- Slots 40 through 127 represent cells on the Free Duel selection screen:
  - `page = slot / 40`
  - `cell = slot % 40` (arranged in 5 columns × 8 rows).
- Explicit `slot` requests are satisfied first. Remaining duelists fill lowest available spaces.

### 4.6.3 40-Card Deck Pools & Fixed Decks (`decks`)

- **Weighted Pools**: Cards assigned weights normalized to 2048. Minimum 14 distinct cards required.
- **Fixed Decks**: `"fixed": true` with card counts summing to exactly 40. Dealt verbatim.

### 4.6.4 Drop Pools: SA-POW, BCD, SA-TEC (`drops`)

Independent probability tables scaled to 2048:

- `pow` (`sa-pow`): S/A rank victory focused on aggressive power.
- `bcd`: Middle-tier rank victory.
- `tec` (`sa-tec`): S/A rank victory focused on technical play (deck exhaustion, traps, fusions).

### 4.6.5 Custom Duelist Portraits & 16-Color Quantization

Portraits are downscaled to 48 × 48 with 64 colors, with original high-resolution assets preserved for HD presentation.

### 4.6.6 Large Character Busts & Emotional Expressions

High-resolution dialog portraits displayed during campaign dialog and Tower cutscenes:

```json
"bust": {
  "normal": "portraits/bust_normal.png",
  "angry": "portraits/bust_angry.png",
  "happy": "portraits/bust_happy.png"
}
```

### 4.6.7 AI Strategy, Search Depth, & Omniscience (`sight`)

- `ai.copy`: Duplicates playstyle of an existing duelist.
- `ai.search`: Search depth and combo evaluation steps.
- `ai.sight`: Boolean. When `true`, grants the AI full visibility of the player's face-down cards.
- `ai.values`: Array of 9 strategic tuning weights governing card prioritization.

### 4.6.8 Rank Score Evaluation Calibration (`ranks`)

Configures the scoring matrices across 10 duel metrics (turns elapsed, damage taken, fusions performed, cards played, etc.).

### 4.6.9 Complex Unlock Conditions (`unlock`)

- `beat`: Duelist required to be defeated.
- `wins`: Win count threshold.
- `story`: Campaign progression flag.
- `card` / `copies`: Required cards in Trunk/Deck.
- `tower`: Tower Mode floor clearance requirement.
- `tower_only`: Restricts opponent exclusively to Tower Mode.

---

## 4.7 UI Tab (Interface, Menus, Frames, & Boards)

### 4.7.1 Title Screen Customization

- Replace background art with 320 × 240 PNGs (supports widescreen extensions).
- Relocate or recolor Game Logo, Copyright notice, and "PUSH START BUTTON" prompt.

### 4.7.2 Interactive Menus & Custom Button Actions

Customize entries in the primary and secondary title menus:

- Built-in targets: `new_game`, `load`, `duel`, `trade`, `options`, `campaign`, `free_duel`, `build_deck`, `library`, `password`, `save`, `tower`, `quit`, `debug_menu`.
- Custom actions: `event` (triggers `MEMORIES_EVENT_MENU`), `notice` (displays text popup), `none`.

### 4.7.3 Duel HUD Layout, Sizing, & Visibility

Reposition and scale all duel interface components:

- Player and Opponent LP panels.
- FIELD elemental indicator box.
- Hand and field cursors.
- Card bar text elements (ATK, DEF, Name, Stars, Type Icon, Spell Category).

### 4.7.4 3D Duel Board Arenas & Terrain Textures

The Duel Board editor provides a 3D interactive viewport to preview and replace floor zone textures and perimeter trim for all 6 arena terrains (Forest, Wasteland, Mountain, Meadow, Sea, Dark).

### 4.7.5 Frame Styles & Full Card Layout Engine

Full graphical control over card borders and elements (`card_layout.frame_styles`):

- Custom high-resolution border images (`image`), card back art (`back_image`), and hand sprites.
- Per-element positioning, scaling, and visibility toggling for Title, Art frame, Stars, Attribute icon, ATK/DEF values, and Labels.

---

## 4.8 Starter Decks Tab

### 4.8.1 Fixed 40-Card Starter Decks

Assigns an exact 40-card deck list dealt to every new campaign save.

### 4.8.2 Probabilistic Starter Pools

Generates initial player decks by rolling from custom card pools:

```json
"starter_pools": [
  { "name": "Key Monster", "draws": 1, "cards": { "Dark Magician": 50, "Blue-eyes White Dragon": 50 } },
  { "name": "Standard Monsters", "draws": 29, "cards": { "Celtic Guardian": 100, "Silver Fang": 100 } },
  { "name": "Magic & Traps", "draws": 10, "cards": { "Raigeki": 10, "Sparks": 90 } }
]
```

The sum of `draws` must equal exactly 40.

---

## 4.9 Campaign Map Tab

### 4.9.1 Binary Location Records Structure

Controls the 16 overworld map nodes (indices 0–9: World Map, indices 10–15: Ancient Town). Each node is defined by a 66-byte record:

- Confirm gate flag and target scene index.
- 3D camera orientation parameters.
- Screen arrow placement coordinates.

### 4.9.2 Camera ViewState & Target Vectors

Defines Camera Pitch, Heading (angle), Distance, and Target X/Z focus coordinates per location.

### 4.9.3 Town Millennium Puzzle Coordinates

Adjusts the 2D pixel coordinates of the Millennium Puzzle location marker in the Town view.

### 4.9.4 Exits, Directional Inputs, Story Flags, & Animations

Each location supports up to 4 directional exits (Up `0x1000`, Right `0x2000`, Down `0x4000`, Left `0x8000`) gated by story progression flags.

---

## 4.10 Values Tab (Core Game Limits & Engine Mechanics)

### 4.10.1 Stat & Life Point Boundaries

- **`stats` / `attack` / `defense`**: Monster stat cap (default: 9,999; maximum: 32,767).
- **`life_points.start`**: Initial LP for player and CPU (default: 8,000; maximum: 32,767).
- **`life_points.max`**: Recovery ceiling (prevents healing beyond starting LP if unset).
- **`life_points.duelists`**: Per-duelist starting LP overrides.
- **`two_player`**: LP settings for 2-Player mode (`start`, `max`, `step`).

### 4.10.2 Magic & Trap Constants

- **`swords_turns`**: Duration of Swords of Revealing Light (default: 3; maximum: 9).
- **`crush_card`**: ATK threshold destroyed by Crush Card Virus (default: 1,500; maximum: 32,767).
- **`spellbinding_circle`**: Stat reduction applied by Spellbinding Circle (default: 500; maximum: 9,999).
- **`shadow_spell`**: Stat reduction applied by Shadow Spell (default: 1,000; maximum: 9,999).

### 4.10.3 Inventory Capacities & Trunk Overflow Conversion

- **`deck_copies`**: Maximum copies of a card permitted in deck construction (default: 3; maximum: 40).
- **`chest`**: Maximum copies of each card stored in the Trunk (default: 250; maximum: 255).
- **`chest_overflow`**: Automatically converts duplicate cards won beyond Trunk capacity into StarChips:
  ```json
  "chest_overflow": { "limit": 3, "starchips": 5 }
  ```

### 4.10.4 StarChip Economy & Drop Locks

- **`starchip_prize`**: StarChips rewarded per victory rank: `S`, `A`, `B`, `C`, `D` (default: 5, 4, 3, 2, 1; maximum: 1,000).
- **`new_game_starchips`**: Starting balance for new game saves (maximum: 99,999,999).
- **`starchips`**: Maximum StarChip wallet capacity (default: 999,999; maximum: 99,999,999).
- **`card_drops`**: Locks the number of cards rewarded per victory (1–99), overriding user cheat settings.

### 4.10.5 Rank Scoring Constants

- **`rank_score.start`**: Initial rank score baseline (default: 50; 50+ awards POW, under 50 awards TEC).
- **`rank_score.exodia`**: Score points added on Exodia victory (default: +40).
- **`rank_score.deck_out`**: Score points added on opponent deck depletion (default: -40).

### 4.10.6 Password Shop Pricing & Overrides

Customizes password activation codes and StarChip pricing:

```json
"passwords": {
  "all": { "password": "card number", "starchips_percent": 50 },
  "Blue-eyes White Dragon": { "password": "00000001", "starchips": 1000 }
}
```

### 4.10.7 Terrain Combat Multipliers

Customizes ATK/DEF bonuses (-9,999 to +9,999) granted to monster types across the 6 terrains.

---

## 4.11 Guardian Stars Tab

### 4.11.1 Expanding Beyond the 10 Retail Stars (Up to 15 Stars)

Guardian Stars are stored in 4-bit fields in card records, allowing up to **15 total stars** (IDs 1–15; 0 indicates no star):

```json
"guardian_stars": {
  "stars": [
    { "id": 11, "name": "Earthquake", "icon": "icons/earth.png", "palette": "own", "beats": ["Mercury"] }
  ]
}
```

### 4.11.2 Custom Matchup Advantages & Mirror Match Rules

- **`matchups`**: Configures directional combat advantage bonuses between specific star pairs.
- **`default_bonus`**: Default advantage bonus (default: +500).
- **`mirror`**: When `true`, automatically assigns reciprocal negative bonus to the defender.

### 4.11.3 Summoning Star Selection Policies

Configured via `"choice"`:

- `"ask"`: Prompts player to choose between primary and secondary star on summon (retail default).
- `"first"`: Automatically assigns primary star.
- `"best"`: Automatically selects star providing optimal matchup against opposing field.

### 4.11.4 Batch Rule Automation ("Set stars by rule...")

Automates assigning guardian stars across hundreds of cards based on Type, Attribute, or ATK thresholds.

---

## 4.12 Packs Tab (Card Booster Packs & In-Game Shop)

### 4.12.1 Booster Pack Packaging & Visuals

Defines purchasable card booster packs integrated into the in-game Shop:

- `image`: Pack wrapper art.
- `cover`: Featured card art overlay.
- `image_style`: `"card"` (traditional rectangular frame) or `"full"` (full-bleed pack wrapper).

### 4.12.2 Economic Models & Multi-Currency Costs

Packs can be purchased using StarChips or card sacrifices:

```json
"cost": {
  "starchips": 250,
  "cards": ["Kuriboh", "Baby Dragon"]
}
```

### 4.12.3 Rarity Tiers, Drop Probabilities, & Weighted Pools

Packs contain structured probability tiers:

```json
"tiers": [
  { "odds": 700000, "label": "Common", "cards": ["Common Pool"] },
  { "odds": 250000, "label": "Rare", "cards": ["Rare Pool"], "color": "Blue" },
  { "odds": 50000, "label": "Secret Rare", "cards": ["Secret Pool"], "color": "Gold", "sound": 12, "reveal": "flip" }
]
```

### 4.12.4 Guaranteed Slots & Pity Mechanics

- **`slots`**: Enforces specific rarity distributions across individual cards in a pack (e.g. Card 5 is guaranteed Rare or better).
- **`pity`**: Increments odds of higher tiers for each pack opened without a top-tier pull.

### 4.12.5 Unlocks, Limited Stock, & Shop Environment Tuning

- `stock`: Maximum packs available for purchase.
- `unlock`: Unlocks availability based on defeated opponents, StarChips spent, or campaign progression.
- `reveal`: Animation style (`"flip"`, `"quick"`, `"list"`).

---

## 4.13 Tower Mode Tab (Gauntlet Dungeons & Visual Novel)

### 4.13.1 Floor-by-Floor Progression (Up to 99 Floors)

Tower Mode allows building multi-stage roguelike gauntlets:

```json
"tower": {
  "name": "Trials of the Shadow Realm",
  "on_lose": "restart",
  "on_leave": "checkpoint",
  "floors": [
    {
      "duelist": "Dark Simon",
      "field": "Dark",
      "deck": "decks/dark_simon.json",
      "life_points": 10000,
      "no_fusions": true,
      "reward": { "starchips": 1000, "card": "Dark Magician" }
    }
  ]
}
```

### 4.13.2 Floor Constraints & Duel Modifiers

- `no_fusions`: Disables fusion mechanics for the battle.
- `no_equips`: Disables equip spell attachments.
- `life_points`: Custom starting HP for the floor opponent.
- `field`: Forced arena terrain.

### 4.13.3 Cutscene Scripting Syntax & Stage Choreography

Tower Mode includes a built-in visual novel dialogue system:

```text
[scene: torch corridor] [left: 1] [right: opponent]
Welcome to the trial.
@right: You shall not pass!
#8: The Pharaoh steps forward.
[scene: scenes/portal.png] [pan: 64 0 30 wait]
```

- `[scene: name]`: Sets background scenery.
- `[left: id] / [right: id]`: Stages character busts on left/right screen edges.
- `@speaker: text`: Formats dialogue assigned to staged character.
- `[pan: x y frames wait]`: Animates camera translation across the scene.

### 4.13.4 Rewards, Checkpoints, & Failure Rules

- `on_lose`: `"restart"` (reset to floor 1) or `"checkpoint"`.
- `reward`: Unlocks duelists, cards, or StarChips upon floor clearance.

---

## 4.14 Mod Info, Diagnostics, & Conflict Matrix

### 4.14.1 Mod Validation & Error Preflight

The FM Editor performs preflight validation before saving:

- Verifies 40-card totals on fixed decks.
- Ensures drop tables scale to 2048.
- Detects circular dependencies in `requires` and `after`.
- Flags missing asset file references.

### 4.14.2 Conflict Detection Engine

The Conflicts tab analyzes all installed mods:

- Detects overlapping retail card edits.
- Identifies conflicting disc sector overrides.
- Flags duplicate function hooks and unresolvable dependencies.

---

## 4.15 Mod Conversion, Disc Import, & Migration Tools

### 4.15.1 Direct Retail Disc Extraction

The editor can extract retail tables, card artwork, and audio directly from a retail ISO (`SLUS_014.11` / `SLES_014.11`) without external extraction utilities.

### 4.15.2 Converting Old `.ygomods` Packages

Legacy mod archives (`.ygomods`) are imported and automatically upgraded into the native `mod.json` folder structure.

### 4.15.3 Card Identity Migration (`MEMORIES_MIGRATE_CARD_IDS`)

When upgrading older mods that used volatile numeric card IDs to stable string identities (`mod-id:entry-id:n`), running the game once with:

```bash
MEMORIES_MIGRATE_CARD_IDS=1
```

converts player deck and inventory save files to persistent string identities.

### 4.15.4 CLI Verification & Headless Validation

The editor can be run headlessly from the command line for automated testing:

```bash
python tools/pc/fm_editor check <mod_folder> --print
```

Verifies manifest syntax, checks for broken references, and outputs normalized JSON.

---

<!-- GOAL_COMPLETE -->
