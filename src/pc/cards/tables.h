#ifndef MEMORIES_PC_TABLES_H
#define MEMORIES_PC_TABLES_H
/* The duel's rule tables as mods change them (notes/gameplay-tables.md).
 *
 * A mod's manifest may carry "fusions", "equips", "rituals", "drops",
 * "decks", "equip_bonus_default", "terrain_bonus", "trap_thresholds",
 * "chest_overflow", "passwords" and "limits":
 * edits to the tables and constants the duel reads from the disc, written with
 * card names, stable identities or ids. They are read once at startup, after
 * the cards (cards.h), in the order the mods load; where two mods set the
 * same thing the later one wins, and drop and deck edits of the same
 * opponent add up rather than replace each other.
 *
 * Nothing here changes the tables the game loaded. The game's own code asks
 * these functions first, where it reads a table, and falls back to what the
 * disc (or a data mod's patch of it) says. The Password screen's table is
 * the exception: its search is a loop over the loaded records, so
 * Main_RunPasswordMenu writes the mods' passwords and prices into them. */

/* Read every applied mod's tables; once, from Cards_Build. */
void Tables_Build(void);

/* Tables_Add without a mod list: one manifest's tables, for the tests. */
struct JsonValue;
void Tables_Add(const char *mod, const struct JsonValue *manifest);
/* The same with the mod's directory, so "drops" and "decks" may name a file
 * of the mod's instead of being written out in the manifest. */
void Tables_AddFrom(const char *mod, const char *directory, const struct JsonValue *manifest);
void Tables_Clear(void);

/* A fusion rule for these two cards (in either order, or for their retail
 * bases): 1 with its result in *result, which is 0 when the rule forbids
 * the fusion; 0, leaving *result alone, when no rule applies. */
int Tables_Fusion(int a, int b, int *result);
/* What a retail recipe makes, unless a mod removed every recipe for it. */
int Tables_FilterFusion(int result);

/* Whether `equip` may equip `monster`: 1 or 0 by a mod's rule, -1 when no
 * rule says, and the disc's table decides. */
int Tables_Equip(int equip, int monster);
/* What `equip` adds to `monster`'s ATK and DEF: a mod's "bonus" or
 * "bonus_if" for it (the latest entry that fits), else the mods'
 * "equip_bonus_default", else `retail`, the disc's +500 (+1000 for
 * Megamorph). */
int Tables_EquipBonus(int equip, int monster, int retail);

/* A ritual's recipe: 1 with the ritual card, its three tributes, its result
 * and a 0 after them in `recipe` (the layout of the game's ritual table),
 * 0 when a mod removed the ritual, -1 when the disc's recipe stands. */
int Tables_Ritual(int ritual, unsigned short recipe[6]);
/* Whether any mod has a "rituals" entry for this card (a card past the
 * disc's without one is its base's ritual). */
int Tables_HasRitual(int ritual);

/* A condition-based ritual tribute. A slot may require a specific card,
 * a monster type, secondary fusion group (Elf/Female), minimum/maximum printed
 * ATK/DEF, a minimum/maximum printed level, and/or DEF greater than ATK.
 * The requirements in one slot are ANDed. Returns 1 when the latest ritual
 * rule has condition-based tributes and fills result, 0 otherwise. */
typedef struct {
    unsigned short card;
    short min_attack, min_defense, max_attack, max_defense;
    signed char type, min_level, max_level;
    unsigned char fusion_group, defense_gt_attack;
} TablesRitualRequirement;
int Tables_RitualRequirements(int ritual, TablesRitualRequirement requirements[3], unsigned short *result);

/* A weighted pool as the running opponent's mods have it: TABLES_POOL_DECK
 * (the cards an opponent's deck is dealt from), or a drop pool (S/A-POW,
 * B/C/D, S/A-TEC, in Duel_SelectCardDrop's order). `retail` is the pool the
 * game loaded (CARD_COUNT weights by id - 1). Returns weights by card id,
 * gCard_nCount + 1 of them adding up to DUEL_DROP_WEIGHT_TOTAL, or NULL when
 * no mod edits this pool and the game reads its own. */
enum { TABLES_POOL_DECK, TABLES_POOL_POW, TABLES_POOL_BCD, TABLES_POOL_TEC, TABLES_POOL_COUNT };
const unsigned short *Tables_Pool(int pool, const unsigned short *retail);
/* The same for a given opponent (0-39), for the tests and tools. */
const unsigned short *Tables_PoolFor(int duelist, int pool, const unsigned short *retail);
/* Scale the weights `chosen` marks (by card id, 1..count) so they add up to
 * `target` exactly: each its share rounded down, the rest to the largest
 * remainders, the lower id first. The others are left as they are. 0 when
 * it runs out of memory, or when nothing chosen weighs anything and the
 * target is not 0. Pools use it. */
int Tables_Scale(unsigned *weights, const unsigned char *chosen, int count, unsigned target);

/* An opponent's fixed deck ("decks": {"fixed": true, card: copies}): 1 with
 * its 40 cards, in id order, in `cards`, which the duel then shuffles; 0 when
 * no mod fixes it. A fixed deck wins over weighted edits of the same deck,
 * and the latest fixed deck over earlier ones. */
#define TABLES_DECK_SIZE 40
int Tables_FixedDeck(int duelist, unsigned short cards[TABLES_DECK_SIZE]);

/* Duel_AwardCard's questions before it adds a card to the chest, which holds
 * `quantity` of it. With a mod's "chest_overflow", the chest keeps at most
 * Tables_ChestLimit copies (CARD_CHEST_QUANTITY_MAX, 250, without one), and a
 * card it has no room for adds the mod's starchips to the save's
 * `starchips`, up to 999999. Tables_ChestOverflow returns the starchips
 * added, 0 when the card fits or no mod says. Tables_ChestFull is 1 when a
 * mod's "chest_overflow" is read and `quantity` is at its limit: the chest
 * then keeps what it holds, and the password shop sells no copy. Without
 * one it is always 0, and the chest is the disc's. */
int Tables_ChestLimit(void);
/* The most copies of a card the chest may hold at all: the disc's 250, or a
 * mod's limit past it (up to 255, a byte a card). Returning a card from the
 * deck and a trade stop there. */
int Tables_ChestRoom(void);
int Tables_ChestFull(unsigned quantity);
int Tables_ChestOverflow(unsigned quantity, unsigned *starchips);

/* --- limits (notes/gameplay-tables.md, "Limits") ---------------------------
 *
 * A mod's "limits" raise or lower the numbers the game caps: a monster's ATK
 * and DEF, the life points a duel starts with and may heal up to, the
 * two-player setup's LP choice, the starchips the save holds, the copies of a
 * card the chest keeps and a Free Duel record. Each answer is the retail
 * constant when no mod sets it, so without one nothing changes.
 *
 * The values live in 16-bit fields (DuelCardRecord, AiActiveCard,
 * DuelSideState), so 32767 is as far as ATK, DEF and LP go; the chest is a
 * byte a card (255) and the Free Duel records are halfwords. A value past
 * that is noted in the Mods window and held at the largest the game keeps. */
#define TABLES_LIMIT_STAT_MAX 32767        /* s16 ATK/DEF */
#define TABLES_LIMIT_LIFE_POINTS_MAX 32767 /* s16 LP */
#define TABLES_LIMIT_CHEST_MAX 255         /* a byte a card in the chest */
#define TABLES_LIMIT_RECORD_MAX 32767      /* s16 wins and losses */
#define TABLES_LIMIT_TWO_PLAYER_RECORD_MAX 65535 /* u16 wins and losses on a 2P save */
#define TABLES_LIMIT_STARCHIPS_MAX 99999999L

/* The most ATK (`defense` 0) or DEF (1) a monster has after its bonuses:
 * CARD_STAT_MAX, 9999, unless a mod's "attack", "defense" or "stats" say. */
int Tables_StatCap(int defense);
/* The higher of the two, for a scan that ranks by either. */
int Tables_StatCapEither(void);
/* The life points a side (0 the player, 1 the opponent) starts a duel
 * against `duelist` (gDuel_bOpponentID; negative for a two-player duel)
 * with: the mod's entry for that duelist, else its "player" or
 * "opponent", else its "start", else `retail`. */
int Tables_StartingLifePoints(int side, int duelist, int retail);
/* How far healing may take a side that started with `start`: the mod's
 * "max", else `start` (the disc's rule: no healing past the start). */
int Tables_MaxLifePoints(int start);
/* The two-player setup's LP choice: TABLES_TWO_PLAYER_START (8000),
 * TABLES_TWO_PLAYER_MAX (8000) and TABLES_TWO_PLAYER_STEP (500), each the
 * mod's "two_player" entry or `retail`. */
enum { TABLES_TWO_PLAYER_START, TABLES_TWO_PLAYER_MAX, TABLES_TWO_PLAYER_STEP };
int Tables_TwoPlayerLifePoints(int which, int retail);
/* The most starchips the save holds: SAVE_DATA_STARCHIP_MAX (999999) or
 * the mod's "starchips". */
unsigned Tables_StarchipCap(void);
/* The most wins or losses a Free Duel record counts: FREE_DUEL_RECORD_MAX
 * (999) or the mod's "free_duel_record". */
int Tables_FreeDuelRecordCap(void);
/* The most wins or losses a two-player duel adds up on a memory card's
 * save: 9999 or the mod's "two_player_record". */
int Tables_TwoPlayerRecordCap(void);
/* A limit by the name the mod API asks with (modapi.h, API 8): "attack",
 * "defense", "life_points" (the start against the CPU), "life_points_max"
 * (0 while healing stops at the start), "two_player_start",
 * "two_player_max", "two_player_step", "starchips", "chest",
 * "free_duel_record", "two_player_record". -1 for a name it does not know. */
long Tables_Limit(const char *name);

/* A mod's "terrain_bonus" for a monster of `type` (0-19) on `terrain`
 * (gDuel_bTerrain: 1 Forest to 6 Yami): 1 with the points, signed, in
 * *bonus; 0 when no mod lists the pair (nor replaced the table), and the
 * disc's table decides. */
int Tables_TerrainBonus(int terrain, int type, int *bonus);

/* The attack, in points, at or under which attack trap `trap` springs (0
 * House of Adhesive Tape to 5 Widespread Ruin): a mod's "trap_thresholds",
 * else `retail`, the disc's. */
int Tables_TrapThreshold(int trap, int retail);

/* The Password screen's record for card `id` (any loaded card) as the mods'
 * "passwords" have it: `price` and `password` come in as the disc's table
 * has them (starchips, and eight digits a nibble each or
 * CARD_PASSWORD_NONE) and are changed in place. 1 when a mod changed
 * either. The shared password/price policy applies these rules over raw
 * disc values (or an added card's defaults), for both the shop and viewer. */
int Tables_PasswordShop(int id, unsigned *price, unsigned *password);
/* Two cards with one password in the Password screen's table as loaded
 * (`passwords[id]`, 1 through gCard_nCount, a `data` patch and the mods' "passwords" in):
 * the screen gives the lower card number, so the other cannot be had.
 * Each such card is noted in the Mods window beside the mod that set its
 * password (else the one that set the winner's), or only logged when no
 * "passwords" entry made it. Returns how many cards were shadowed. */
int Tables_CheckPasswords(const unsigned *passwords);

/* --- the rank score (notes/more-duelists.md) -------------------------------
 *
 * How well a duel was played is scored from ten rules, each a row of five
 * threshold/change pairs: the first threshold strictly above the measured
 * value gives the change, and the changes move a score that starts at 50 and
 * settles the S to D letter -- which in turn picks the drop pool. The row
 * comes from the duelist's own block on the disc, so it is per duelist, but
 * every one of the forty carries the same forty copies. "ranks" is what makes
 * them differ.
 *
 * A row is TABLES_RANK_STEPS pairs, threshold then change, as the game's
 * DuelRankScoreChangeEntry lays them out. NULL comes back when no mod edits
 * this rule and the game reads its own.
 */
#define TABLES_RANK_RULE_COUNT 10
#define TABLES_RANK_STEPS 5
const short *Tables_Rank(int rule);
/* The same for a given opponent, for the tests and tools. */
const short *Tables_RankFor(int duelist, int rule);
/* A duelist's own rule, from its file (pc/free_duel/duelists.h): `row` is
 * TABLES_RANK_STEPS pairs, threshold then change. The last threshold ends the
 * walk however the value compares, as the disc's 32767 does. */
void Tables_SetRank(int duelist, int rule, const short *row);
/* The rule a name means, in the game's order (duel_rank.h), or -1. */
int Tables_RankNamed(const char *name);

/* The opponent names a manifest may use, by duelist id; "all" means every
 * one of them. */
#define TABLES_DUELIST_COUNT 40
extern const char *const Tables_DuelistNames[TABLES_DUELIST_COUNT];
/* The English name the duel shows for an opponent in place of COM
 * (hd_text.h): the whole name up to 11 letters, else the part that tells
 * them apart (High Mage Anubisius: H.M. Anubisius). NULL for no opponent
 * (a 2P duel). A translation's is Text_OpponentName's (text.h). */
const char *Tables_DuelistShortName(int duelist);
/* Whether a mod names the duelist: one it added, or one it replaced and gave
 * a "name" (notes/more-duelists.md). Such a name is the mod's, shortened as a
 * translation's is, and a translation does not change it. */
int Tables_DuelistRenamed(int duelist);
/* The most letters (spaces and full stops too) a name in place of COM
 * has: H.M. Anubisius, the longest English one, still fits the box. */
#define TABLES_SHORT_NAME_LIMIT 14
/* A translated full name made a name in place of COM, as the English ones
 * are: `name` (Latin-1) up to its first character that is not a letter, a
 * space or a full stop (Jono 2º Duelo: Jono), then whole if it has at most
 * TABLES_SHORT_NAME_LIMIT; else, when its words all start with a capital,
 * the first ones as initials (Sumo Mago Martis: S.M. Martis), else its last
 * word (Mago da Montanha: Montanha); else its first letters. Written to
 * `out` (TABLES_SHORT_NAME_LIMIT + 1 bytes); 0 when nothing is left. */
int Tables_ShortenName(const char *name, char *out);
/* The opponent of the duel under way (gDuel_bOpponentID): 1-39, negative
 * in a 2P duel. */
int Tables_OpponentId(void);

#endif
