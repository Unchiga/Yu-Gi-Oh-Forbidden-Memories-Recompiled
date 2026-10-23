#ifndef MEMORIES_MOD_TYPES_H
#define MEMORIES_MOD_TYPES_H
#define MEMORIES_MOD_API 21
/* API 15: card_types names the existing type ids and declares monster ids
 * 24-255, with optional per-custom-type icons. It has no host entry: cards
 * and gameplay tables resolve its names. */
/* API 16: "attributes" renames the retail six and declares monster
 * attributes 8-255, with optional icons; no host entry either. */
/* API 11 adds no host entry or event: it marks the mod.json features a game
 * of API 10 would leave out (notes/modding.md, "Which game a mod needs"), so
 * a mod that uses them says "min_api": 11 and an older game refuses it.
 * API 13 does the same for "limits": {"card_drops": n}, a lock on the cards
 * a won duel deals (notes/card-drops.md); API 14 for the tower: a mod's
 * "tower", a duelist's "tower_only" and "unlock" "tower", a menu item's
 * action "tower"; API 17 for a duelist's "bust" and "replace_bust" and a
 * tower story's "expression" by name; API 18 for the card frames' keys
 * (card_layout's default_styles, a frame style's hand, images, library_icon
 * and own parts, atk_label/dfd_label/type_label, a part's hide or color, and
 * a card's frame_style). API 20 adds the named field registry ("fields")
 * and its optional field pictures. API 21 adds battle models a mod brings
 * ("models", read from the mod's own files) and a card's "model" naming
 * one. */
/* API 3: before hooks may alter arguments/result, or set handled to replace
 * the operation (including cancellation). Highest priority runs first;
 * equal priorities follow registration/load order. After hooks observe the
 * operation result. Input runs from VBlank (possibly interrupt context):
 * no allocation, file I/O or registration there. Keep all callbacks short. */
enum { MEMORIES_BEFORE, MEMORIES_AFTER };
enum {
    MEMORIES_EVENT_INPUT, MEMORIES_EVENT_DAMAGE, MEMORIES_EVENT_REWARD,
    MEMORIES_EVENT_FUSION, MEMORIES_EVENT_EFFECT, MEMORIES_EVENT_AI,
    MEMORIES_EVENT_SCENE, MEMORIES_EVENT_SAVE, MEMORIES_EVENT_LOAD,
    MEMORIES_EVENT_SETTINGS, MEMORIES_EVENT_EQUIP,
    /* API 4, after only: the save slot menu saved the running game (a slot,
     * from 0; b the slot's token, save_slots.h; c the save's sequence), or
     * loaded it (the same). What a mod keeps per save goes in its own file
     * named after the token (open_data), so each slot has its own. */
    MEMORIES_EVENT_SLOT_SAVE, MEMORIES_EVENT_SLOT_LOAD,
    /* API 6: end-of-duel StarChip prize about to be added to the save
     * (Mods_AwardStarchips). a is the configured prize (limits.starchip_prize,
     * otherwise rank tier + 1); edit it
     * to change the award, or handle to skip adding. After observes result
     * as the amount actually credited, capped by limits.starchips. The
     * results display shows the configured prize (one icon and xN past 8);
     * changing a here does not change that already displayed prize. */
    MEMORIES_EVENT_STARCHIP,
    /* API 9: an item of the title's menus chosen (notes/modding.md, "The
     * title's menus"): a the item (0-10 the game's entries, as
     * MainMenuSelection; 11 on the mods' buttons, host->menu_item names
     * it), b the menu (0 the first, 1 the second), c the item's "value".
     * Before: set handled to do something else instead, and result to a
     * choice (MainMenuSelection, or a number of the mod's own that its
     * MEMORIES_EVENT_SCENE takes) to slide the menu out and leave the title
     * with it; result stays -1 to stay. After observes. An item whose
     * action is "event" does nothing but this. */
    MEMORIES_EVENT_MENU,
    /* API 10: a monster on the field (notes/more-cards.md, "Monster
     * effects"): a the card, b its duel record (0-29: b / 15 the side, 0 the
     * player; b % 15 is 5-9 for the monster zones), c what happened:
     * MEMORIES_MONSTER_SUMMON ... MEMORIES_MONSTER_DESTROY_OPPONENT. Every monster,
     * with "monster_effects" of its own or not. Before: set handled to skip
     * the card's own effects for this; a mod may start a card effect of its
     * own (DuelEffect_StartCardEffect), which runs before the next one.
     * After observes. SUMMON is a face-up summon only (a face-down play is
     * none). FLIP is a face-down monster attacked (no trap sprang), after
     * the battle, as Yu-Gi-Oh! resolves it after the damage: whether or
     * not the battle destroyed it, before the DESTROYED of what it did.
     * Turned face up any other way is no FLIP. COMBAT comes as the battle begins, when no
     * trap sprang, for the attacker and then the monster it attacks.
     * DESTROY_OPPONENT: it won a battle that destroyed the other monster,
     * after that one's DESTROYED. */
    MEMORIES_EVENT_MONSTER,
    MEMORIES_EVENT_COUNT
};
enum {
    MEMORIES_MONSTER_SUMMON, MEMORIES_MONSTER_FLIP, MEMORIES_MONSTER_DRAW, MEMORIES_MONSTER_COMBAT,
    MEMORIES_MONSTER_DESTROYED, MEMORIES_MONSTER_DESTROY_OPPONENT
};
typedef struct {
    unsigned type, phase;
    int a, b, c, result, handled;
} MemoriesModEvent;
typedef void (*MemoriesModCallback)(MemoriesModEvent *event);

#endif
