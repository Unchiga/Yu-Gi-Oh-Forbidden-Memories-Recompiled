#ifndef MEMORIES_PC_STARS_H
#define MEMORIES_PC_STARS_H
/* Guardian Stars as mods change them (notes/modding.md, "Guardian Stars").
 *
 * A mod's manifest may carry "guardian_stars": the stars' names and icons,
 * new stars past the disc's ten (up to the card record's 4-bit fields), and
 * the matchup table: for each ORDERED pair (the attacker's star, the
 * defender's star) one signed adjustment, which is exactly what
 * Duel_CalcGuardianStarMatchup returns (the disc's +500, -500 or 0). The
 * duel adds it to the left comparison value, as it adds the disc's.
 *
 *   "guardian_stars": {
 *       "stars": [ {"id": 11, "name": "Fire", "icon": "icons/fire.png",
 *                   "beats": ["Grass"]} ],
 *       "matchups": [ {"attacker": 1, "defender": 2, "bonus": 1000,
 *                      "mirror": true} ],
 *       "default_bonus": 500,
 *       "replace": false,
 *       "choice": "ask"
 *   }
 *
 * Everything left out stays the disc's. Without a mod nothing here decides
 * anything and the game's own arithmetic runs. Read once, the first time
 * anything asks (the cards name stars while they are built), in the order
 * the mods load; where two mods set the same pair the later one wins. */
#include <stddef.h>

#define STARS_RETAIL 10          /* Mars .. Venus */
#define STARS_MAX 15             /* the card record's 4-bit star fields */
#define STARS_NAME_TEXT 0x8317   /* + star: its name in the names bank */

struct JsonValue;

/* Read every applied mod's "guardian_stars"; once. The accessors below
 * call it themselves. */
void Stars_Build(void);
/* One manifest's "guardian_stars", for the tests; and back to the disc's. */
void Stars_Add(const char *mod, const struct JsonValue *manifest);
void Stars_AddFrom(const char *mod, const char *directory, const struct JsonValue *manifest);
void Stars_Clear(void);
/* Set by the optional icon renderer; keeping it a callback lets the rule
 * parser's focused tests remain independent of rendering modules. */
extern void (*Stars_IconsCleared)(void);

/* The disc's rule, as Duel_CalcGuardianStarMatchup works it out for any two
 * 4-bit ids (0 and 11-15 included: the disc gives some of those +/-500). */
int Stars_RetailMatchup(int attacker, int defender);

/* Whether a mod decides the pair (attacker's star, defender's star): 1 with
 * the adjustment in *bonus, 0 when the disc's arithmetic stands. */
int Stars_Matchup(int attacker, int defender, int *bonus);

/* The adjustment for any pair, the disc's where no mod decides it. */
int Stars_Bonus(int attacker, int defender);

/* How far DuelScene_UpdateBattle moves the on-screen modifier each update
 * on its way to `shown` (the pair's absolute adjustment): the disc's 16 up
 * to 512, and so many that it takes no longer than the disc's 32 updates
 * past that. */
int Stars_DisplayStep(int shown);

/* Which star a monster summoned now uses. A card with one star (no second,
 * or the same one twice; no retail card is either) never asks. Otherwise the
 * mods' "choice": "ask" (the disc's: the SELECT A GUARDIAN STAR box),
 * "first" (always the first, no box) or "best" (the one with the better
 * matchups against `enemy`, the `count` stars of the opponent's face-up
 * monsters; the first on a tie or with none). -1 to ask, 0 for the first
 * star, 1 for the second. */
enum { STARS_CHOICE_ASK, STARS_CHOICE_FIRST, STARS_CHOICE_BEST };
int Stars_ChoiceMode(void);
int Stars_SummonChoice(int first, int second, const int *enemy, int count);
/* Whether a card with these stars has only one: no second, or the same. */
int Stars_Single(int first, int second);

/* No star. A card's "stars" may say none (0, null, "none" or "(none)"):
 * [none, none] is a monster with no star at all, [none, X] is read as
 * [X, none], a monster with the one star X. No card of the disc is either.
 * Once a mod has made a monster with no star (cards.c calls
 * Stars_NoteNoStar), star 0 meets every star at 0 both ways, where the
 * disc's arithmetic gives 0 against Mars +500 and against Pluto -500; the
 * lists and the field bar draw no icon and no name for it, and the card
 * view has the magic cards' layout (Stars_NoStarCard). Without such a card
 * nothing here changes anything. */
/* A star as a card's "stars" gives it: its number (0 is none, up to 15 and
 * past it, which the caller says is too many), a name Stars_Find knows, or
 * none; -1 for anything else. */
int Stars_Value(const struct JsonValue *value);
/* [none, X] to [X, none]: 1 when it swapped. */
int Stars_Normalize(int *first, int *second);
void Stars_NoteNoStar(void);
int Stars_NoStarUsed(void);
/* The same two for card `card_id` summoned now by the side whose turn it
 * is, against the duel's records (stars_duel.c). */
int Stars_PickForCard(int card_id);
int Stars_CardSingle(int card_id);
/* Whether card `card_id` is a monster a mod gave no star (0 until a mod has
 * made one, Stars_NoStarUsed): the card views lay it out as a magic card's,
 * with no GUARDIAN STAR heading over nothing (stars_duel.c). */
int Stars_NoStarCard(int card_id);

/* How many stars there are: 10, or the highest a mod declares. */
int Stars_Count(void);
/* Whether a mod declares `star` (a new one, or a disc star it renames). */
int Stars_Declared(int star);

/* A star by its number ("11", 11), the disc's English name ("Mars"), or
 * any name a mod gives it; -1 for none. */
int Stars_Find(const char *name);

/* The mod's name for `star` in the names bank's codes (ending in 0xFF), in
 * the language the game runs in, or NULL: Text_Resolve's for
 * STARS_NAME_TEXT + star. */
const unsigned char *Stars_NameText(int star);
/* Whether a mod gives `star` a "name" (Stars_NameText is otherwise "Star N"
 * for a new star, which a translation's name for it stands over). */
int Stars_Named(int star);
/* The name in UTF-8 for logs and host UI: the mod's, else the disc's
 * English, else "Star N". */
const char *Stars_Name(int star);

/* The mod's icon for `star`, a PNG's full path, or NULL; and whether it
 * keeps the PNG's own colors ("palette": "own") rather than taking the
 * disc's stars' ("game", the default). */
const char *Stars_Icon(int star);
int Stars_IconOwnPalette(int star);

/* Icons (star_icons.c). func_80037DA4 marks the icon it is about to add
 * with its star, DuelEffect_AppendEntry takes the mark into the entry's
 * code (retail leaves an icon's code unused) as STARS_ICON_CODE | star, or
 * 0 where the disc's icon stands; func_80035E20 then asks Stars_IconCell
 * where the icon is. Only stars past 10 and stars a mod gives an icon are
 * marked. */
#define STARS_ICON_CODE 0xE500u   /* no Shift-JIS character: E5 then 00-0F */
void Stars_MarkIcon(int star);
unsigned Stars_TakeIconMark(void);
/* For a marked entry's code: the sprite's texture page (with the bank's
 * bits), u, v and palette. 0 for any other code. */
int Stars_IconCell(unsigned code, int *tpage, int *u, int *v, int *clut_x, int *clut_y);
/* The same for the battle's star effect (duel effect 0xE, retail MIPS),
 * with the palette as a primitive's clut word. 0 where the disc's stands. */
int Stars_EffectCell(int star, int *tpage, int *u, int *v, int *clut);

/* After the cards and the rule tables: notes about stars no card has, cards
 * with stars no mod declares, stars with no matchup and adjustments past
 * the stat cap. */
void Stars_Check(void);

#endif
