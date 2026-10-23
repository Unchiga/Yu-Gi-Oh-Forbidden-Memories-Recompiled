# Extended Monster Effects System (API 19)

This document provides complete technical documentation for the extended monster effects system introduced in **Mod API 19** for **Yu-Gi-Oh! Forbidden Memories Recompiled**, covering both the native runtime engine (`src/pc/cards/monster_effects*.c`) and the FM Editor (`tools/pc/fm_editor/monster_effects*.py`).

---

## Contents

- [Overview](#overview)
- [Effect Triggers (`when`) and Actions (`do`)](#effect-triggers-when-and-actions-do)
- [Cannot Be Destroyed by Battle (`battle_immunity`)](#cannot-be-destroyed-by-battle-battle_immunity)
  - [Lifecycle & Destruction Interception](#lifecycle--destruction-interception)
  - [Battle Immunity Modifiers](#battle-immunity-modifiers)
  - [Attacker Filtering Criteria](#attacker-filtering-criteria)
  - [Single Attack Modifier Rule](#single-attack-modifier-rule)
- [Piercing Battle Damage (`piercing`)](#piercing-battle-damage-piercing)
  - [Combat Mechanics & LP Damage](#combat-mechanics--lp-damage)
  - [2D Battle Presentation: Visual Slash & Damage Popup](#2d-battle-presentation-visual-slash--damage-popup)
  - [Multi-Frame Safety & Lifecycle Scoping](#multi-frame-safety--lifecycle-scoping)
  - [Piercing Damage Modifiers (Defender Criteria)](#piercing-damage-modifiers-defender-criteria)
- [Modular Conditions Engine](#modular-conditions-engine)
  - [Evaluation Pipeline & Re-entrancy Guards](#evaluation-pipeline--re-entrancy-guards)
  - [Condition Catalog & Specification](#condition-catalog--specification)
- [FM Editor Integration](#fm-editor-integration)
  - [Action Pickers & Dynamic Modifiers Block](#action-pickers--dynamic-modifiers-block)
  - [Condition Dialog & Advanced Field Pickers](#condition-dialog--advanced-field-pickers)
  - [Validation & Diagnostic Rules](#validation--diagnostic-rules)
  - [Automated Verification & Tests](#automated-verification--tests)

---

## Overview

The monster effects system allows declarative triggered abilities, static aura effects, and combat modifiers to be attached to cards via the `"monster_effects"` array in `mod.json`.

API 19 introduces:
1. **Battle Immunity (`do: "battle_immunity"`)**: Continuous protection from combat destruction under `when: "face_up"`, with optional attack count limits or attacker filtering criteria.
2. **Piercing Damage (`do: "piercing"`)**: Infliction of difference battle damage against defense-position monsters under `when: "face_up"`, accompanied by authentic 2D battle slash visuals and floating red damage numbers.
3. **Dedicated Modifiers Architecture**: Attack-count caps and combat filtering criteria are modeled as effect-specific modifiers directly on the effect, kept strictly distinct from the general condition list.
4. **Expanded Modular Condition Suite**: A broad collection of field presence checks, player LP comparisons, monster count comparisons, and field spell checks combinable using AND/OR logic.

---

## Effect Triggers (`when`) and Actions (`do`)

### Triggers (`when`)
- **`summon`**: Triggers once immediately upon face-up normal summon or flip face-up by player action.
- **`flip`**: Triggers when attacked while face-down and flipped face-up in combat.
- **`draw`**: Triggers during the owner's draw phase.
- **`combat`**: Triggers pre-battle when engagement is initiated.
- **`destroyed`**: Triggers when removed from the field by battle or card effect.
- **`destroy_opponent`**: Triggers after successfully destroying an opponent's monster in battle.
- **`face_up`**: Continuous static aura active while the card remains face up on the field.

### Actions (`do`)
- **`boost`**: Applies dynamic or static ATK/DEF modifiers to targets.
- **`heal`**: Restores owner Life Points.
- **`damage`**: Inflicts effect damage to opponent Life Points.
- **`destroy`**: Removes qualifying monsters from the field.
- **`magic`**: Triggers an existing spell effect by numeric ID.
- **`battle_immunity`** *(API 19)*: Continuous battle destruction shield.
- **`piercing`** *(API 19)*: Piercing combat damage against defense monsters.

---

## Cannot Be Destroyed by Battle (`battle_immunity`)

Configured under continuous static field state:
```json
{
  "when": "face_up",
  "do": "battle_immunity"
}
```

### Lifecycle & Destruction Interception
- **Scope**: With the default `"always"` modifier the card is not destroyed by battle whether it attacks or is attacked. Every other modifier (attack counts and attacker criteria) applies only while this card is the **defender** receiving an attack (`record == S.trap_battle_record[1]`); attacking a stronger monster, it is destroyed as usual.
- **Standard Battle Damage**: Life Point calculation proceeds normally: if the defender's ATK/DEF is lower than the attacking monster's ATK, the difference is deducted from the player's LP as regular combat damage.
- **Destruction Interception**: When battle destruction would be finalized:
  - 2D duel view: internal register `D_8009B1B0` is transitioned from `-1` (destroy) to `1` (struck, stays on field).
  - 3D battle: the losers in `D_8009B208` / `D_8009B209` are cleared to `-1` before the resume phase (11) removes them on the way back.
- **Non-Battle Removal**: Spells (e.g. *Raigeki*), traps, and monster destruction effects remove the card normally.

### Battle Immunity Modifiers

Modifiers define specific combat conditions under which battle immunity applies. To preserve the 20-byte `MonsterEffect` binary ABI from v0.3.0, modifiers and conditions are stored in a parallel table `MonsterEffectEx` (queried via `Cards_MonsterEffectsEx()` / `Cards_CardEffectsEx()`):

| Modifier ID | Name / Description | Extra Parameters |
| ----------- | ------------------ | ---------------- |
| `"always"` | Always (Default) | _(none)_ |
| `"first_attacked"` | The first time it is attacked | `reset` (`"turn"`, `"copy"`, `"duel"`) |
| `"destroyed_after_2_attacks"` | Can only be destroyed after two attacks | `reset` (`"turn"`, `"copy"`, `"duel"`) |
| `"destroyed_after_3_attacks"` | Can only be destroyed after three attacks | `reset` (`"turn"`, `"copy"`, `"duel"`) |
| `"attacked_by_type"` | If attacked by monster of Type | `attacker_type` (Type name/ID) |
| `"attacked_by_attribute"` | If attacked by monster of Attribute | `attacker_attribute` (Attribute name/ID) |
| `"attacked_by_stat"` | If attacked by monster with ATK/DEF threshold | `attacker_stat`, `attacker_stat_op`, `attacker_amount` |
| `"attacked_by_level"` | If attacked by monster with Level threshold | `attacker_level_op`, `attacker_level` |
| `"attacked_by_monster"` | If attacked by monster (Combined criteria) | All configured attacker fields |

### Attacker Filtering Criteria

Attacker criteria dynamically inspect the attacking monster during damage calculation via `Duel_CalcCardStats()` and `Cards_Level()`:
- **`attacker_stat`**: `"atk"` or `"def"`
- **`attacker_stat_op`**: `"<"`, `"<="`, `">"`, `">="`, `"=="` (default `">"`)
- **`attacker_amount`**: Stat value threshold (0–9999)
- **`attacker_level_op`**: `"<"`, `"<="`, `">"`, `">="`, `"=="` (default `">"`)
- **`attacker_level`**: Level threshold (1–12)

Example:
```json
{
  "when": "face_up",
  "do": "battle_immunity",
  "modifier": "attacked_by_monster",
  "attacker_type": "Dragon",
  "attacker_attribute": "LIGHT",
  "attacker_stat": "atk",
  "attacker_stat_op": ">=",
  "attacker_amount": 2500,
  "attacker_level_op": ">=",
  "attacker_level": 7
}
```

> [!IMPORTANT]
> **Single Attack Modifier Rule**:
> A card may possess at most **one** battle immunity effect containing an attack-count modifier (`first_attacked`, `destroyed_after_2_attacks`, `destroyed_after_3_attacks`). Attacker criteria modifiers do not count toward this limit. The FM Editor enforces this (its dialog and `problems()`); the game itself does not check it, and with two such effects the card is protected while either holds.

---

## Piercing Battle Damage (`piercing`)

Configured under continuous static field state:
```json
{
  "when": "face_up",
  "do": "piercing"
}
```

### Combat Mechanics & LP Damage
- **Attacker Scope**: Evaluated exclusively when this card is the **attacker** initiating combat (`record == S.trap_battle_record[0]`).
- **Target Position**: Triggers when the opposing defender is in **Defense Position** (`D_8009B178[1] & 0x800`).
- **Calculation**: If `attacker_atk > defender_def`:
  - `diff = attacker_atk - defender_def` is inflicted directly to the defending player's Life Points via `Mods_DamageLife()`.
  - Floating battle damage popup is populated via `D_8009B1A4[1] = diff`.
  - Player duel rank statistics are updated (`rank.effective_attacks++`).
  - If `attacker_atk <= defender_def`, standard defense recoil damage applies and piercing does not trigger.

### 2D Battle Presentation: Visual Slash & Damage Popup
During the 2D zoomed-in battle scene:
- Retail duel state 8 allocates an effect request: `req = DuelEffect_AllocateRequest(2)` for the defender (`D_8009B1B9 == 1`) at coordinates `(0xF0, 0x78)`.
- `req->field_12` receives the piercing damage value `diff`.
- `req->field_1A` is configured with the slash power tier (`diff / 1000`: 0 for light, 1 for medium, 2 for heavy).
- The corresponding slash sound effect is triggered: `SD_SEPlayFull(req->field_1A + 0x10)`.
- Effect 2 (`func_80153200`) detects non-zero damage, selects the diagonal slash animation table, assigns red font coloring, and draws the floating damage number and slash polygon sparkles across the defending card.

### Multi-Frame Safety & Lifecycle Scoping
- Damage calculation is strictly scoped to presentation steps 4 through 8 (`battle_step >= 4 && battle_step <= 8`).
- Upon first infliction, `S.piercing_applied = 1` locks subsequent frames in that combat exchange from re-applying damage.
- Multi-frame cleanup states (such as State 11 sprite fade-outs running across 16–32 consecutive frames) are barred from recalculating damage or resetting the guard flag.
- State is cleared cleanly when transitioning out of `PHASE_BATTLE` or initiating a new combat interaction in `MonsterEffects_Battle()`.

### Piercing Damage Modifiers (Defender Criteria)

| Modifier ID | Name / Description | Extra Parameters |
| ----------- | ------------------ | ---------------- |
| `"always"` | Always (Default) | _(none)_ |
| `"attacks_type"` | If it attacks a monster of Type | `defender_type` (Type name/ID) |
| `"attacks_attribute"` | If it attacks a monster of Attribute | `defender_attribute` (Attribute name/ID) |
| `"attacks_stat"` | If it attacks a monster with ATK/DEF threshold | `defender_stat`, `defender_stat_op`, `defender_amount` |
| `"attacks_level"` | If it attacks a monster with Level threshold | `defender_level_op`, `defender_level` |
| `"attacks_monster"` | If it attacks a monster (Combined criteria) | All configured defender fields |

---

## Modular Conditions Engine

Continuous effects under `when: "face_up"` (`boost`, `battle_immunity`, and `piercing`) can specify up to 4 conditions (`MONSTER_CONDITIONS_MAX = 4`):

```json
{
  "when": "face_up",
  "do": "piercing",
  "conditions": [
    {
      "type": "control_type",
      "card_type": "Dragon",
      "mode": "at_least_one"
    },
    {
      "type": "field_spell",
      "field": "mountain",
      "presence": "active"
    }
  ],
  "condition_mode": "all"
}
```

### Evaluation Pipeline & Re-entrancy Guards
- **`condition_mode`**: `"all"` (logical AND) or `"any"` (logical OR).
- **Stat Recursion Guard**: Because conditions evaluate in-combat effective stats using `Duel_CalcCardStats()`, an internal re-entrancy flag `s_evaluating_stats` in `src/pc/cards/monster_effects_duel.c` prevents infinite recursion loops.

### Condition Catalog & Specification

| Condition Type | Parameters | Evaluation Summary |
| -------------- | ---------- | ------------------ |
| **`named_card`** | `card`<br>`whose`: `"all"`, `"own"`, `"opponent"` | True if designated card is face-up in the specified field zone. |
| **`stat_threshold`** | `stat`: `"atk"` or `"def"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`<br>`value`: integer | Compares this monster's current effective stat against `value`. |
| **`lowest_atk`** | _(none)_ | True if this monster has strictly lower ATK than every other face-up monster on the board. Ties disqualify. |
| **`highest_atk`** | _(none)_ | True if this monster has strictly higher ATK than every other face-up monster on the board. Ties disqualify. |
| **`fewer_monsters`** | _(none)_ | True if controller controls strictly fewer monsters than opponent (`own < opp`). Zero monsters handled explicitly; ties disqualify. |
| **`more_monsters`** | _(none)_ | True if controller controls strictly more monsters than opponent (`own > opp`). Zero monsters handled explicitly; ties disqualify. |
| **`control_stat`** | `presence`: `"control"` or `"not_control"`<br>`scope`: `"own"` or `"other"`<br>`stat`: `"atk"` or `"def"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`, `"=="`<br>`value`: integer | True if controller controls (or does not control) a monster meeting the ATK/DEF threshold. `scope: "other"` excludes this card. |
| **`control_level`** | `presence`: `"control"` or `"not_control"`<br>`scope`: `"own"` or `"other"`<br>`op`: `"<"`, `"<="`, `">"`, `">="`, `"=="`<br>`level`: 1–12 | True if controller controls (or does not control) a monster meeting the Level comparison via `Cards_Level()`. `scope: "other"` excludes this card. |
| **`control_type`** | `whose`: `"own"`, `"opponent"`, `"both"`<br>`presence`: `"control"` or `"not_control"`<br>`scope`: `"own"` or `"other"`<br>`card_type`: Type<br>`mode`: `"at_least_one"` or `"only"` | True if designated Type is present (or not present) on the target field. `mode: "only"` requires all monsters to match. |
| **`control_attribute`** | `whose`: `"own"`, `"opponent"`, `"both"`<br>`presence`: `"control"` or `"not_control"`<br>`scope`: `"own"` or `"other"`<br>`attribute`: Attribute<br>`mode`: `"at_least_one"` or `"only"` | True if designated Attribute is present (or not present) on the target field. `mode: "only"` requires all monsters to match. |
| **`field_spell`** | `field`: `"any"`, `"forest"`, `"wasteland"`, `"mountain"`, `"sogen"`, `"umi"`, `"yami"`<br>`presence`: `"active"` or `"not_active"` | True if active duel terrain matches target field spell criteria (`gDuel_bTerrain != 0` for any, or specific ID match). Unknown field spell names emit a `Mods_Note` and are rejected. |
| **`lower_lp`** | _(none)_ | True if controller Life Points are strictly lower than opponent's (`my_lp < opp_lp`). Ties disqualify. |
| **`higher_lp`** | _(none)_ | True if controller Life Points are strictly higher than opponent's (`my_lp > opp_lp`). Ties disqualify. |

---

## FM Editor Integration

The FM Editor (`tools/pc/fm_editor`) provides complete graphical configuration for API 19 features.

### Action Pickers & Dynamic Modifiers Block
- Selecting `"Cannot be destroyed by battle"` or `"Piercing damage"` reveals the dedicated **Modifiers** panel directly above the Conditions section.
- For `"battle_immunity"`, options include unconditional protection, attack-count survival limits (with reset policies), and attacker filtering criteria.
- For `"piercing"`, options include unconditional piercing and defender filtering criteria.
- Attacker and defender filtering dynamically toggle Type, Attribute, Stat operator/threshold, and Level operator/threshold pickers.

### Condition Dialog & Advanced Field Pickers
- **`ConditionDialog`**: Full modal editor featuring presence toggles, evaluation scope pickers (`Any monster you control` vs `Another monster you control`), field spell selectors, and LP comparison modes.
- **Treeview Display**: Formats multi-condition clauses clearly with human-readable descriptions and handles AND/OR combinator switching.

### Validation & Diagnostic Rules
- **Attack Count Limit**: `validate.py` and `problems()` emit an immediate diagnostic warning if any monster defines more than one attack-count modifier across all its effects:
  `"A monster can only have one condition related to a number of attacks."`
- The `EffectDialog` blocks saving a second attack-count modifier if another effect on the card already specifies one.

### Responsive Layout & Bidirectional Scrolling
- **Bidirectional Treeview**: Horizontal and vertical scrollbars prevent truncation of descriptive effect phrases.
- **Expanded Windows**: Default widths expanded (`EffectDialog` at 680px minsize, `ConditionDialog` at 620px minsize) with horizontal window resizing enabled.

- **C Engine Tests**:
  - `tests/pc/cards_stars_test.c`: Validates effect parsing, serialization, and battle immunity stat comparisons.
  - `tests/pc/monster_attacked_conditions_test.c`: Exhaustive test matrix for all attacker/defender modifiers, modular conditions, and tier comparisons.
- **FM Editor Python Tests**:
  - `tools/pc/fm_editor/tests/test_data.py`: Tests manifest serialization, deserialization, and validator problem rules.
  - `tools/pc/fm_editor/tests/test_gui.py`: Tests effect dialog widgets, condition dialog parameters, and single attack modifier enforcement.
  - Discover run: 685 tests passing.
