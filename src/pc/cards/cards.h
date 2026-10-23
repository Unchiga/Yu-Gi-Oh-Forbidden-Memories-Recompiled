#ifndef MEMORIES_PC_CARDS_H
#define MEMORIES_PC_CARDS_H
#include <stddef.h>
/* More cards than the disc has (notes/more-cards.md).
 *
 * A mod's "cards" adds cards after the 722 retail ones. Each new card is a
 * copy of a retail card, its *base*: it has the base's artwork, 3D model,
 * card text, fusions, equips and effect, and its own ATK, DEF, type,
 * guardian stars, level, attribute and, optionally, name. The disc has none
 * of these cards, so wherever the game goes to the disc or to a table laid
 * out by the disc it asks for the base instead (Cards_BaseId).
 *
 * The tables the game indexes by card id (stats, level and attribute, the
 * name sort key) and the workspaces sized by the number of cards live with
 * the game's own variables (src/pc/game/card_storage.c), so save states
 * carry them. What a save holds of the new cards -- how many the player owns
 * and whether the Library has seen them -- has no room in the memory card's
 * block, so it is kept beside it in the user directory, by duelist code and
 * save sequence (cards.c).
 *
 * An entry with "replace" in place of "copy" changes a retail card itself:
 * its stats in the tables, and its own name, text and artwork here, which
 * the game's lookups of the retail ones (Text_Resolve) then give. */
#include "game/card_constants.h"
struct MonsterEffect;
struct MonsterEffectEx;

/* The number of cards this run has: CARD_COUNT without a card mod. */
extern int gCard_nCount;
/* Per card id: the retail card it is a copy of (itself for retail cards). */
extern unsigned short gCard_awBaseId[];
/* The trunk and the Library's seen marks of the running save, for ids past
 * CARD_COUNT: quantities by id, and one bit per id. */
extern unsigned char gCard_abExtraChest[];
extern unsigned char gCard_abExtraSeen[];
/* Whose they are: the duelist code of the save they belong to. */
extern int gCard_nExtraOwner;
/* The same trunks for the two saves a two-player trade or duel loads, and
 * for the copies of them a trade is made on before it is written. */
extern unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END];
extern unsigned char gCard_abPairPending[2][CARD_TABLE_ID_END];

/* At startup, once the mods are applied: the retail tables from the game's
 * executable, then every applied mod's cards. */
void Cards_Build(void);
const char *Cards_Identity(int id);
int Cards_FindIdentity(const char *identity);
int Cards_ModelId(int id);
int Cards_EffectId(int id);
/* Data-defined effects for a Magic or Trap card.  "card_effects" either
 * supplements its retail/aliased effect or replaces it entirely. */
int Cards_CardEffects(int id, const struct MonsterEffect **effects);
int Cards_CardEffectsEx(int id, const struct MonsterEffectEx **effects_ex);
int Cards_CardEffectsReplace(int id);
/* The trap a card set on the field springs as: a trap card's effect (its
 * own, its base's or the one "effect" names), 0 for a card of another type,
 * which springs as none even where it was a trap on the disc. */
int Cards_TrapId(int id);
/* A card's own attack-trap threshold (inclusive ATK), or fallback. Only
 * active for traps with one of the six retail attack-destruction effects.
 * Copies inherit their base's override; null clears it. */
int Cards_TrapThreshold(int id, int fallback);
/* The type a card has on the disc, which is what its effect is; -1 past the disc. */
int Cards_RetailType(int id);
/* Nonzero for a card a mod's "replace" made another kind of card than its
 * base is on the disc: a monster made a magic, trap, ritual or equip card, or
 * one of those made a monster or another of them. The disc's fusions and
 * equips are of the card it was, so they no longer hold for it (rules.h). */
int Cards_KindChanged(int id);
/* The disc card the opponent's scripts take `id` for when they look a card up
 * by number: its effect (Cards_EffectId) while it is still that card's kind,
 * or an explicit card_effects replacement's "ai_effect". -1 means the CPU
 * must not confuse this card with a retail behavior. */
int Cards_AiId(int id);
/* Whether the disc has a 3D model for `id` to stand as: a magic, trap, ritual
 * or equip card a mod made a monster has none unless it borrows one. */
int Cards_HasModel(int id);
/* Whether `id` is one of the five pieces of Exodia, with their rules (one of
 * each in a deck, all five in hand win): a piece a mod replaced is not,
 * unless its entry says "exodia": true. */
int Cards_ExodiaPiece(int id);
/* The disc's six frame colors, in the order of their palette rows: the card
 * view's frame sheet (rows 8-13), the duel hand's (rows 1-6) and the
 * Library's grid. A card's frame color is one of these, or the type's. The
 * disc draws monster gold, magic and equip green, trap pink and ritual blue;
 * purple and orange it never uses. (The names are of the colors: the enum's
 * MONSTER, MAGIC, TRAP and RITUAL are the disc's own labels for gold, green,
 * pink and blue, and a cards mod's "frame" accepts either.) */
enum { CARD_FRAME_MONSTER, CARD_FRAME_MAGIC, CARD_FRAME_TRAP, CARD_FRAME_RITUAL, CARD_FRAME_PURPLE,
       CARD_FRAME_ORANGE, CARD_FRAME_COUNT };
/* What a card is, for choosing its frame (card_layout.h): worked out from its
 * type and its "monster_effects", never written by a mod. Every card is in
 * exactly one class. */
enum { CARD_CLASS_MONSTER, CARD_CLASS_EFFECT_MONSTER, CARD_CLASS_SPELL, CARD_CLASS_EQUIP,
       CARD_CLASS_RITUAL_SPELL, CARD_CLASS_TRAP, CARD_CLASS_COUNT };
int Cards_Class(int id);
/* The class's name as a layout's rules write it ("effect_monster"). */
const char *Cards_ClassName(int cls);
/* Whether a cards mod's entry gave `id` the tag (its "tags": ["god"]): a
 * card is free to carry any, and a layout may give each its own frame
 * ("tag" in card_layout's "frame_for"). A copy has its base's unless it says. */
int Cards_HasTag(int id, const char *tag);
/* The color a mod's "frame" chose for `id`, CARD_FRAME_*, or -1 (left out,
 * or "Type"). */
int Cards_FrameOverride(int id);
/* An optional named frame style selected by a card's "frame_style".  The
 * style itself belongs to the active card_layout mod; this is merely the
 * card-side link, so a cards mod can request a newly defined style without
 * having to overload one of the six palette colors or add a throwaway tag.
 * "" means that the layout's frame_for/default_style rules choose it. */
const char *Cards_FrameStyle(int id);
/* The frame color `id` is drawn in when it is not its type's (-1: its
 * type's, which is what the disc does). With the anime frame on this is the
 * color of the frame style the layout picked (card_layout.h); otherwise a
 * mod's "frame", or orange for a monster with "monster_effects". The card
 * view, the duel's hand and field cards and the Library's grid draw it
 * through that color's palette row. */
int Cards_FrameColor(int id);
int Cards_Fusion(int a, int b, int *result);

/* A card a manifest names: its id, a stable identity ("mod:entry:n"), or a
 * retail card's name ("Blue-Eyes White Dragon"; case, spaces and punctuation
 * do not matter). 0 for null or no value, -1 when it names no card. */
struct JsonValue;
int Cards_Reference(const struct JsonValue *value);
int Cards_Named(const char *text);
/* A type by name ("Winged Beast"), or -1; and the type of a card.  Mods may
 * rename any retail type and declare new monster types in unused encodings
 * 24-255 through their "card_types" section. */
int Cards_TypeNamed(const char *text);
const char *Cards_TypeName(int type);
/* The full PNG path a mod declares for a custom type's marker, or NULL.
 * Retail types deliberately return NULL: their icons remain the disc/HD-pack
 * assets rather than becoming global replacements. */
const char *Cards_TypeIcon(int type);
/* A mod's own name for the type, glyph-encoded (0xFF-terminated) for the
 * card view's text stream (func_80037DA4): a custom type's, or a retail
 * type's a mod renamed. NULL where the disc's own spelling stands.
 * Cards_TypeName's UTF-8 is not the game's own glyph codes. */
const unsigned char *Cards_TypeNameText(int type);
int Cards_IsMonsterType(int type);
int Cards_Type(int id);
/* An attribute by name (the six retail ones or one a mod's "attributes"
 * declares), its id (0-5 or 8-255), or -1; and the attribute of a card. */
int Cards_AttributeNamed(const char *text);
int Cards_Attribute(int id);
/* A monster's printed level (the number of stars), or -1 for no card. */
int Cards_Level(int id);
/* Forbidden Memories' secondary fusion groups used by general fusion rules.
 * These are properties of the retail base card, not primary monster types. */
enum {
    CARD_FUSION_GROUP_NONE,
    CARD_FUSION_GROUP_ANGEL_WINGED,
    CARD_FUSION_GROUP_BUGROTHIAN,
    CARD_FUSION_GROUP_EGG,
    CARD_FUSION_GROUP_ELF,
    CARD_FUSION_GROUP_FEATHER_FROM_BEAR,
    CARD_FUSION_GROUP_FEATHER_FROM_HARPIE,
    CARD_FUSION_GROUP_FEATHER_FROM_MACHINE,
    CARD_FUSION_GROUP_FEMALE,
    CARD_FUSION_GROUP_JAR,
    CARD_FUSION_GROUP_KOUMORIAN,
    CARD_FUSION_GROUP_MERCURY_MAGIC_USER,
    CARD_FUSION_GROUP_MERCURY_SPELLCASTER,
    CARD_FUSION_GROUP_MIRROR,
    CARD_FUSION_GROUP_MUS_KINGIAN,
    CARD_FUSION_GROUP_MYST_ELFIAN,
    CARD_FUSION_GROUP_RAINBOW,
    CARD_FUSION_GROUP_SHEEPIAN,
    CARD_FUSION_GROUP_THRONIAN,
    CARD_FUSION_GROUP_TURTLE,
    CARD_FUSION_GROUP_USABLE_BEAST
};
int Cards_FusionGroupNamed(const char *text);
int Cards_InFusionGroup(int id, int group);

/* The retail card `id` is a copy of, or `id` itself; 0 for no card. */
int Cards_BaseId(int id);
/* Whether `id` names a card this run has. */
int Cards_Valid(int id);

/* The byte that holds how many of `id` the save at `state` (a SaveDataState:
 * the deck, then the trunk at +0x50) has in its trunk. For the running save
 * and the two a two-player screen loads, ids past CARD_COUNT have a trunk of
 * their own here; for any other copy of a save there is none, and the byte
 * returned for them reads zero and forgets what is written to it. */
unsigned char *Cards_ChestSlot(void *state, int id);

/* The Library's "seen" mark for `id` (campaign flag 0x120 + id for retail
 * cards, this module's bits past them). */
int Cards_Seen(int id);
void Cards_MarkSeen(int id);
/* View > Library: show every card (SET_LIBRARY_ALL_CARDS): nonzero when the
 * Library, as it opens, should lay `id` out as a seen card although it has
 * never been seen. The Library shows it with its art, name, stats and text,
 * but does not count it in its "seen/total" heading, and no seen mark, trunk
 * byte or other save byte is written for it (func_8002BFCC). */
int Cards_LibraryPlaceholder(int id);

/* A card's own name, in the game's glyph codes and ending in 0xFF, or NULL
 * when it has the name of its base (for a retail card: its disc name). */
const unsigned char *Cards_NameText(int id);
/* A card's name as the game shows it now (a mod's, a translation's, or the
 * retail one) in the game's glyph codes, ending in 0xFF; NULL for no card. */
const unsigned char *Cards_NameCodes(int id);
/* The same in UTF-8. 0 for no such card. */
int Cards_NameUtf8(int id, char *out, size_t size);

/* A card's own text (glyph codes, 0xFE between lines, 0xFF at the end), or
 * NULL when it has its base's. */
const unsigned char *Cards_DescriptionText(int id);

/* A card's notes (card_notes.h): every "notes" its entries gave, in load
 * order, a line between two; NULL for none. The game plays by none of it:
 * it is the modder's, and a code mod's through the host's card_notes and
 * card_tag (API 7). */
const char *Cards_Notes(int id);
/* The value of tag `key` in the card's notes, as CardNotes_Tag; -1 for none. */
int Cards_NoteTag(int id, const char *key, char *out, size_t size);

/* A card's password, eight BCD digits as the disc's password table has
 * them (0x89631139 is 89631139), or CARD_PASSWORD_NONE for a card no
 * password gives (the disc's value for them). */
#define CARD_PASSWORD_NONE 0xFFFFFFFEu
/* The password a mod's entry gave the card ("password"): 1 and *password
 * if it gave one, else 0 (passwords.h has the disc's). */
int Cards_OwnPassword(int id, unsigned *password);

/* A card's own artwork over its base's, as the game loads it: the art record
 * func_80029164 read (the picture, the title plate, the thumbnail), and the
 * 0x580-byte thumbnail block the duel copies for the hand and field. */
void Cards_PatchArtRecord(int id, unsigned char *record);
/* The Password screen's card packs (pack_shop.h) draw a pack on the big
 * card: the next art record loaded for `id` takes `record`'s picture,
 * palette and plate (the bytes before the thumbnail), or, with `record`
 * NULL, `plate` (CARD_TITLE_BYTES) over the card's own plate. Once. Whether
 * the last record loaded was so changed, asked once (HD text then leaves
 * its title alone). An id of 0 disarms it. */
void Cards_OverrideArt(int id, const unsigned char *record, const unsigned char *plate);
int Cards_ArtOverridden(void);
void Cards_PatchThumbnail(int id, unsigned char *block);

/* "field_art" (notes/more-cards.md): a card's picture for its cutout on the
 * duel field alone (mods/3d-monsters/field_art.c), art.h's CARD_ART_PIXELS +
 * CARD_ART_CLUT layout, or NULL when the card (and its base) have none, in
 * which case the cutout uses the card's own art as Cards_PatchArtRecord
 * would. Never applied by Cards_PatchArtRecord itself, so nowhere else the
 * card's art shows is affected. */
const unsigned char *Cards_FieldArtRecord(int id);

/* The game's text for string `id`, found at `text` (a translation's or the
 * disc's), or the port's own version of it where the string counts the
 * disc's 722 cards (the Library's "<seen/722>", string F8). */
const unsigned char *Cards_Text(int id, const unsigned char *text);

/* A card the game rolled from a disc table of retail cards (a duel reward,
 * an opponent's deck): `id` or one of the copies of it that asked to take
 * its place there. `use` is CARDS_USE_DROP or CARDS_USE_OPPONENT. Draws
 * from the game's own random numbers only when there is a choice, so
 * without such copies the game's sequence is untouched. */
#define CARDS_USE_DROP 0
#define CARDS_USE_OPPONENT 1
int Cards_PickVariant(int id, int use);

/* The running save was just loaded (`state` is its SaveDataState), or was
 * just written with this sequence number: read or write what it holds of the
 * new cards, under the token Cards_SetSlotTokens gave last. */
void Cards_SaveLoaded(const void *state);
void Cards_SaveWritten(const void *state, unsigned sequence);
/* The save slot tokens (save_slots.h): the save being played, and every
 * token a slot holds now, whose sections are kept; and the two saves of a
 * two-player screen. */
void Cards_SetSlotTokens(unsigned playing, const unsigned *live, int count);
void Cards_SetPairTokens(unsigned first, unsigned second);
/* A two-player screen loaded both saves (0x801D1200 and 0x1000 on). */
void Cards_PairLoaded(void);
/* A trade: its copies of the two saves (+0x680) start as the saves are, and
 * once both memory cards took them the saves are the copies, and what they
 * hold of the new cards is written beside them. */
void Cards_PairBackup(void);
void Cards_PairCommit(void);

/* The Library draws its grid: the panels behind the sections the retail
 * art does not number (library_panels.c). */
void Cards_DrawLibraryPanels(void);

/* Once a frame: a save the game started fresh (NEW GAME) owns none of the
 * new cards the previous one had. */
void Cards_Frame(void);

#endif
