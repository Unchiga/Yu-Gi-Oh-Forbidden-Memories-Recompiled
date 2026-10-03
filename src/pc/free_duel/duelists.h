#ifndef MEMORIES_PC_FREE_DUEL_DUELISTS_H
#define MEMORIES_PC_FREE_DUEL_DUELISTS_H
/* More duelists than the disc has (notes/more-duelists.md).
 *
 * A mod's "duelists" adds opponents after the retail thirty-nine. Each new
 * duelist is a copy of a retail one, its *base*: it has the base's portrait,
 * deck pool, drop pools and AI, and its own name. The disc has none of these
 * duelists, so wherever the game goes to the disc, or to a table the disc laid
 * out, it asks for the base instead (Duelists_BaseId) -- the same shape
 * cards.h uses for cards past the disc's 722, and for the same reason.
 *
 * What the base gives is then edited with the tables a mod already has
 * (pc/cards/tables.h): "drops" and "decks" name an added duelist as readily
 * as a retail one, so a new opponent with a deck of its own is a "duelists"
 * entry and a "decks" entry for it.
 *
 * An entry may instead "replace" one of the disc's own, and then its name,
 * face, way of playing and unlock go over that duelist rather than making a
 * new one. Where the disc is read never changes -- a replacement is still its
 * own base -- so its deck and drops are edited the way any duelist's are.
 * Two mods replacing the same one are settled as an edited pool is: the last
 * to name it has it.
 *
 * An added entry may ask for a "slot", the id it wants, which is its place on
 * the grid: page slot / 40, cell slot % 40. Slots are handed out once every
 * mod has been read, explicit ones first, so an entry that asked for a place
 * is never beaten to it by one that would have taken anything. Two asking for
 * the same slot are settled by load order the other way from a replacement:
 * the earlier keeps it and the later takes the next free place, since moving
 * the duelist already there would rearrange a roster its own mod laid out. A
 * slot nothing was placed in is no duelist: Duelists_Valid says so and its
 * cell stays empty.
 *
 * Ids are 0 for the Free Duel screen's Deck Build entry and 1 up for
 * opponents, as gDuel_bOpponentID and Tables_DuelistNames number them.
 */

/* The Deck Build entry and the thirty-nine opponents the disc carries. */
#define DUELISTS_RETAIL_COUNT 40

/* Every applied mod's duelists; once, from Cards_Build, before the tables. */
void Duelists_Build(void);
void Duelists_Clear(void);

/* How many this run has: DUELISTS_RETAIL_COUNT without a duelist mod. */
int Duelists_Count(void);
int Duelists_Valid(int duelist);
/* The retail duelist this one is a copy of; itself for a retail duelist. Every
 * disc-shaped read -- the portrait record, the deck and drop block, the AI
 * script -- goes through this. 0 for no duelist. */
int Duelists_BaseId(int duelist);
/* The name the game shows: the duelist's own when it has one, its base's
 * otherwise. Never NULL for a valid id. */
const char *Duelists_Name(int duelist);
/* "mod-id:key", stable across runs and load orders, for what a save keeps of
 * a duelist. NULL for the retail ones, which their id identifies. */
const char *Duelists_Identity(int duelist);
/* The id an identity has this run, or -1. */
int Duelists_Find(const char *identity);
/* The duelist a mod names in a manifest or a filename, or -1: an id, a name
 * from the list, the name the disc gave a duelist a mod has since renamed, or
 * the "mod-id:key" identity of one a mod added. The rule tables name a duelist
 * the same way (cards/tables.h), so both ask here rather than each keeping its
 * own idea of what a name reaches. */
int Duelists_Named(const char *text);

/* --- what a save holds of a duelist ---------------------------------------
 *
 * The first forty win/loss records are the save block's own, at 0x51C, where
 * the game put them; the ones past that are this module's, kept beside the
 * save as the cards' trunk is (cards.h). Both are reached the same way, so a
 * reader does not care which it has.
 */

/* The two halfwords -- wins, then losses -- that `state` (a SaveDataState)
 * holds for `duelist`. Never NULL: an id with nowhere to live gets a slot
 * that reads zero and forgets what is written to it. */
unsigned short *Duelists_RecordSlot(void *state, int duelist);

/* The duelist the Free Duel grid's cell stands for on the page it is showing:
 * page * DUELISTS_RETAIL_COUNT + cell. The grid holds forty cells whatever the
 * roster is, and every index the screen takes from its cursor is a cell, never
 * a duelist, so everything that means a duelist asks here. */
int Duelists_AtCell(int cell);

/* Whether the Free Duel grid shows a duelist, and setting it: the grid's own
 * array for the first forty, this module's past them. */
int Duelists_Available(int duelist);
void Duelists_SetAvailable(int duelist, int shown);

/* --- when an added duelist appears ----------------------------------------
 *
 * The disc's own thirty-nine are unlocked by campaign story flags, which an
 * added duelist has none of. A mod may give one an "unlock" instead: an
 * object of conditions the running save must meet, all of them, before the
 * grid shows it. A duelist with no "unlock" is shown from the start, which is
 * what every added duelist did before there were conditions.
 *
 *   "unlock": {
 *     "beat":   a duelist to have beaten -- an id, a name, or an identity
 *     "wins":   how many wins: against "beat" if there is one (1 by
 *               default), else in all against everybody
 *     "story":  a campaign story flag, as the disc's own unlocks use
 *     "card":   a card the trunk or deck must hold -- an id, a name, or an
 *               identity
 *     "copies": how many of "card" (1 by default)
 *   }
 *
 * Nothing is stored: a condition is read from the save's own records, trunk
 * and flags every time the screen opens, so it follows the save that is
 * loaded and needs no migrating. `state` is a SaveDataState, as
 * Duelists_RecordSlot takes.
 *
 * A stock duelist a mod replaced may carry conditions too, and they stand in
 * place of its campaign flag rather than beside it: Duelists_HasUnlock says
 * whether one did, so the screen knows to leave that flag alone.
 */
int Duelists_Unlocked(const void *state, int duelist);
int Duelists_HasUnlock(int duelist);
/* The same conditions given one by one, for whatever else unlocks by them
 * (the card packs' "unlock", pc/cards/packs.h): "" (or NULL) for no duelist
 * or card, 0 wins and copies for the defaults, -1 for no story flag. */
int Duelists_ConditionsMet(const void *state, const char *beat, int wins, int story, const char *card, int copies);

/* The records of the added duelists, beside the memory card's block: read
 * when a save is loaded and written when one is written, by the duelist code
 * and save sequence that save carries, as the cards' trunk is (cards.h). A
 * save made while no duelist mod was applied has no section of its own; it
 * has what the newest earlier one held. */
void Duelists_SaveLoaded(const void *state);
void Duelists_SaveWritten(const void *state, unsigned sequence);
/* Adds the added duelists' wins and losses a save holds to `wins` and
 * `losses`, as Duelists_SaveLoaded would read them, for the save slot menu
 * (SaveSlots_SetRecordReader). */
void Duelists_SavedRecord(const unsigned char *state, int *wins, int *losses);
/* Once a frame, beside Cards_Frame: NEW GAME writes a new duelist code into
 * the running save without loading one, so the records read for the save
 * before it would otherwise stand in the new game. */
void Duelists_Frame(void);

/* A duelist's own face for the Free Duel grid -- a whole portrait record,
 * image then palette -- or NULL for its base's.
 *
 * A mod gives one by putting a PNG at "portraits/<id>.png" beside its
 * manifest, named for the entry's own "id"; "portrait" names a path instead
 * for anything that does not suit. The one file serves both pictures: it is
 * reduced to the 48x48 of 64 colours the console's slot holds, and kept whole
 * for the scaled picture, which draws it at its own size through the texture
 * pack (TexturePack_AddMade), which knows the upload by the record's bytes.
 * So a portrait is as sharp as the file is, and nothing more is asked of the
 * mod. */
const unsigned char *Duelists_Portrait(int duelist);
/* The nine AI parameters a duelist plays by: its own when a mod gave it any,
 * its base's otherwise. Never NULL for a valid id, so a caller can index it
 * straight. Byte 0 is how deep into its deck the AI may look for a card to
 * play (5 for the early villagers, 20 for the late bosses); the script reads
 * the rest by index, byte 1 multiplied by 100. */
#define DUELIST_AI_FIELDS 9
const signed char *Duelists_AiRow(int duelist);

/* Whether the opponent's searches may read a face-down card.
 *
 * The disc decides this in the AI script's own bytecode, which tests the
 * opponent id against six of them -- Heishin, Pegasus, Heishin 2nd, Seto 3rd,
 * DarkNite and Nitemare -- and hands the searches a register saying whether to
 * pass face-down cards over. A duelist a mod added inherits its base's, since
 * the script is handed the base's id.
 *
 * "sight" in a duelist's "ai" says so outright instead. `script` is what the
 * bytecode asked for and what comes back when the duelist says nothing: 0 to
 * read face-down cards, non-zero to pass them over, which is the sense every
 * one of the searches uses. */
int Duelists_HidesFaceDown(int duelist, int script);

/* --- a duelist's name as a string id --------------------------------------
 *
 * The disc names its duelists by string 0x8328 + id, which is what the Free
 * Duel screen and the duel both ask for. That range stops at duelist 39: the
 * strings after it are the campaign's locations, so an added duelist named
 * that way would rename Metropolis. An added one is named through a private
 * range of this module's instead, which nothing on the disc answers.
 *
 * Both screens ask through this, so neither has to know which kind it has.
 */
int Duelists_NameTextId(int duelist);
/* The game's text for string `id`, found at `text`, or a duelist's own name
 * where `id` is one of the two ranges above. The shape Cards_Text has, and
 * hung off the same calls. */
const unsigned char *Duelists_Text(int id, const unsigned char *text);

#endif
