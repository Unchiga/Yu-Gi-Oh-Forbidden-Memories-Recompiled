/* Duelists a mod adds, replaces and places (src/pc/free_duel/duelists.h).
 *
 * Every rule here is a decision made without a screen: which slot an entry
 * gets when two mods want one, which mod has a stock duelist when two replace
 * it, and whether a save meets an unlock. They are exercised against real
 * manifests, through the real JSON reader, so what a mod writes is what is
 * tested.
 *
 * The save is a SaveDataState as duelists.c reads one: the deck at 0, the
 * trunk at +0x50, the first forty records at 0x51C.
 */
#include "pc/free_duel/duelists.h"
#include "pc/mods/json.h"
#include "pc/cards/tables.h"
#include "pc/debug/log.h"
#include "pc/text/glyphs.h"
#include "game/card_constants.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "pc/compat/fs.h"

/* --- what the module leans on -------------------------------------------- */

#define MODS_MAX 4
static JsonDocument *manifests[MODS_MAX];
static const char *ids[MODS_MAX];
static int mod_count;
static int notes;               /* how many complaints the mods got */

int Mods_LoadedCount(void) { return mod_count; }
int Mods_Loaded(int at) { return at; }
int Mods_Active(int mod) { (void)mod; return 1; }
const char *Mods_Id(int mod) { return ids[mod]; }
static const char *directory = ".";   /* the roster folder a case puts up */
const char *Mods_Directory(int mod) { (void)mod; return directory; }
const JsonValue *Mods_Manifest(int mod) { return Json_Root(manifests[mod]); }
void Mods_Note(const char *mod, const char *format, ...) { (void)mod; (void)format; notes++; }
static int (*given_resolver)(const char *);   /* what the list hands the mod API */
void Mods_SetDuelistResolver(int (*resolve)(const char *)) { given_resolver = resolve; }
void Mods_SetOverlapDuelists(int (*duelist)(const char *)) { (void)duelist; }

/* The forty the disc has; only the handful the cases name need a real one. */
const char *const Tables_DuelistNames[TABLES_DUELIST_COUNT] = {
    "Deck Build", "Simon Muran", "Teana", "Jono", "Villager1", "Villager2", "Villager3", "Seto",
    "Heishin", "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi", "Yami Bakura",
    "Pegasus", "Isis", "Kepura", "Martis", "Neku", "Secmeton", "Mai 2nd", "Nitemare2", "Jono 2nd",
    "Teana 2nd", "Seto 2nd", "Heishin 2", "Labyrinth Mage", "Ocean Mage", "Meadow Mage", "Mountain Mage",
    "Desert Mage", "Forest Mage", "Guardian Sebek", "Guardian Neku", "Heishin 2nd", "Seto 3rd",
    "DarkNite", "Nitemare", "Duel Master K",
};

signed char gDuel_aOpponentData[TABLES_DUELIST_COUNT][DUELIST_AI_FIELDS];
unsigned short gFreeDuel_aExtraRecords[DUELIST_TABLE_COUNT][2];
unsigned char gFreeDuel_abExtraAvailable[DUELIST_TABLE_COUNT];
unsigned char gFreeDuel_abGridAvailable[DUELIST_TABLE_COUNT];
int gFreeDuel_nExtraOwner;
/* The running save, which Duelists_Frame reads the duelist code out of. */
unsigned short gDuel_awPlayerDeck[0x600];
/* Which page of forty the grid is showing, for Duelists_AtCell. */
int gFreeDuel_nPage;

/* The tower (pc/tower/tower.h): which floors the save has won, and which
 * duelists those floors let out. */
static int tower_floors_won;    /* floors 1..n won; the whole tower when -1 */
static int tower_frees;         /* the duelist a won floor names, 0 none */
int Tower_ConditionMet(int floor)
{
    if (floor <= 0) return tower_floors_won < 0;
    return tower_floors_won < 0 || floor <= tower_floors_won;
}
int Tower_FreesDuelist(int duelist) { return duelist == tower_frees; }
static int tower_opens;         /* the duelist a won floor unlocks outright, 0 none */
int Tower_OpensDuelist(int duelist) { return duelist == tower_opens; }

static int story_flag;          /* the one flag the cases test */
static int cards_named;         /* what Cards_Named answers */
int Campaign_TestStoryFlag(int flag) { return flag == story_flag; }
int Cards_Named(const char *text) { (void)text; return cards_named; }
unsigned char *Cards_ChestSlot(void *state, int id) { return (unsigned char *)state + 0x50 + id; }

/* A portrait is not made here: the cases are about which duelist is which. */
int CardArt_PortraitFromImage(const char *path, unsigned char *record, char *why, size_t size)
{
    (void)path; (void)record; (void)why; (void)size;
    return 0;
}
/* A bust is not made here (busts.c has its own cases): each "bust" read is
 * the next number, so a case sees which entry kept which. */
static int busts_read;
int Busts_Read(const char *mod, const char *directory, const JsonValue *value, const char *where)
{
    (void)mod; (void)directory; (void)where;
    return Json_TypeOf(value) == JSON_OBJECT || Json_TypeOf(value) == JSON_STRING ? busts_read++ : -1;
}
void Busts_Make(void) {}
void Busts_Clear(void) { busts_read = 0; }
/* The rules a duelist's file gave, by duelist and rule. */
static short set_rows[DUELIST_TABLE_COUNT][TABLES_RANK_RULE_COUNT][TABLES_RANK_STEPS * 2];
static int set_count;
void Tables_SetRank(int duelist, int rule, const short *row)
{
    assert(duelist >= 0 && duelist < DUELIST_TABLE_COUNT);
    assert(rule >= 0 && rule < TABLES_RANK_RULE_COUNT);
    memcpy(set_rows[duelist][rule], row, sizeof set_rows[0][0]);
    set_count++;
}
int Tables_RankNamed(const char *name)
{
    static const char *const rules[TABLES_RANK_RULE_COUNT] = {
        "turns", "effective attacks", "defensive wins", "face-down plays", "pure magic",
        "traps triggered", "cards used", "remaining lp", "initiate fusion", "equip magic",
    };
    int rule;
    for (rule = 0; name && rule < TABLES_RANK_RULE_COUNT; rule++) {
        if (!strcmp(name, rules[rule])) return rule;
    }
    return -1;
}

int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw, int *ch, int *width, int *height)
{
    (void)path; (void)w; (void)h; (void)x; (void)y; (void)cw; (void)ch; (void)width; (void)height;
    return 0;
}
int TexturePack_AddMade(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                        const char *file, int x, int y, int w, int h)
{
    (void)pixels; (void)words; (void)rows; (void)bpp; (void)clut; (void)clut_entries; (void)file;
    (void)x; (void)y; (void)w; (void)h;
    return 1;
}
int TexturePack_AddMadeSeeThrough(const void *pixels, int words, int rows, int bpp, const void *clut,
                                  int clut_entries, const char *file, int x, int y, int w, int h)
{
    (void)pixels; (void)words; (void)rows; (void)bpp; (void)clut; (void)clut_entries; (void)file;
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}
int Glyphs_Code(uint32_t character) { return (int)character; }
uint32_t Glyphs_NextCharacter(const char **at) { return (unsigned char)*(*at)++; }
int Paths_Contained(const char *path) { return path && strncmp(path, "..", 2) != 0; }
/* Off by default, so a case that is not about the sidecar never writes one;
 * the round-trip case turns it on. */
static int sidecars;
int Paths_User(char *out, size_t size, const char *relative)
{
    if (!sidecars) return 1;
    return snprintf(out, size, "duelists_test_user/%s", relative) >= (int)size;
}
const char *Paths_UserDir(void) { return NULL; }  /* no roster folder in the cases */
int Paths_MakeDirs(const char *path) { mkdir(path, 0777); return 0; }
void Paths_WriteBegin(void) {}
const char *Paths_WriteError(char *out, size_t size, const char *path)
{ snprintf(out, size, "%s: failed.", path); return out; }
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }

/* --- the cases ----------------------------------------------------------- */

static unsigned char save[0x600];

/* A roster folder of the cases' own, beside wherever the test is run. */
#define ROSTER "duelists_test_roster"

static void roster_put(const char *name, const char *text)
{
    char path[256];
    FILE *file;
    mkdir(ROSTER, 0777);
    mkdir(ROSTER "/duelists", 0777);
    snprintf(path, sizeof path, "%s/duelists/%s.json", ROSTER, name);
    file = fopen(path, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
}

/* A file that is not a duelist, as the save's own sidecars sit beside these
 * in the player's folder. */
static void roster_litter(const char *name)
{
    char path[256];
    FILE *file;
    snprintf(path, sizeof path, "%s/duelists/%s", ROSTER, name);
    file = fopen(path, "w");
    assert(file);
    fputs("not a duelist", file);
    fclose(file);
}

static void roster_drop(const char *const *names, int count)
{
    char path[256];
    int i;
    for (i = 0; i < count; i++) {
        snprintf(path, sizeof path, "%s/duelists/%s.json", ROSTER, names[i]);
        remove(path);
    }
    rmdir(ROSTER "/duelists");
    rmdir(ROSTER);
}

static void build(const char *a, const char *b)
{
    char error[256];
    int i;
    for (i = 0; i < mod_count; i++) Json_Free(manifests[i]);
    mod_count = 0;
    notes = 0;
    Duelists_Clear();
    memset(gFreeDuel_abGridAvailable, 1, sizeof gFreeDuel_abGridAvailable);
    if (a) {
        ids[mod_count] = "a";
        manifests[mod_count] = Json_Parse(a, error, sizeof error);
        assert(manifests[mod_count]);
        mod_count++;
    }
    if (b) {
        ids[mod_count] = "b";
        manifests[mod_count] = Json_Parse(b, error, sizeof error);
        assert(manifests[mod_count]);
        mod_count++;
    }
    Duelists_Build();
}

/* The wins the save records against a duelist. */
static void wins(int duelist, int count)
{
    Duelists_RecordSlot(save, duelist)[0] = (unsigned short)count;
}

int main(void)
{
    int i;

    /* Placed in order when nobody asks for anywhere. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"name\":\"Ex\"},"
          "{\"id\":\"y\",\"copy\":\"Teana\",\"name\":\"Why\"}]}", NULL);
    assert(Duelists_Count() == 42);
    assert(!strcmp(Duelists_Name(40), "Ex") && !strcmp(Duelists_Name(41), "Why"));
    assert(Duelists_BaseId(40) == 8 && Duelists_BaseId(41) == 2);

    /* A slot is the place on the grid, and the gap before it is nobody: the
     * list reaches that far but the empty ids are not duelists. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"slot\":45}]}", NULL);
    assert(Duelists_Count() == 46);
    assert(Duelists_Valid(45) && Duelists_BaseId(45) == 8);
    for (i = 40; i < 45; i++) assert(!Duelists_Valid(i) && !Duelists_Unlocked(save, i));

    /* An entry that asked for a place is not beaten to it by one that would
     * have taken anything, whichever is written first. */
    build("{\"duelists\":[{\"id\":\"any\",\"copy\":\"Teana\"},{\"id\":\"at41\",\"copy\":\"Heishin\",\"slot\":41}]}", NULL);
    assert(!strcmp(Duelists_Identity(40), "a:any") && !strcmp(Duelists_Identity(41), "a:at41"));

    /* Two mods wanting one slot: the earlier keeps it, the later takes the
     * next free place, and is told. */
    build("{\"duelists\":[{\"id\":\"first\",\"copy\":\"Heishin\",\"slot\":44}]}",
          "{\"duelists\":[{\"id\":\"second\",\"copy\":\"Teana\",\"slot\":44}]}");
    assert(!strcmp(Duelists_Identity(44), "a:first"));
    assert(!strcmp(Duelists_Identity(40), "b:second"));
    assert(notes == 1);

    /* A replacement is an override and goes the other way: the last mod to
     * name a stock duelist has it, and it stays its own base. */
    build("{\"duelists\":[{\"id\":\"one\",\"replace\":\"Heishin\",\"name\":\"First\"}]}",
          "{\"duelists\":[{\"id\":\"two\",\"replace\":\"Heishin\",\"name\":\"Second\"}]}");
    assert(!strcmp(Duelists_Name(8), "Second"));
    assert(Duelists_BaseId(8) == 8 && Duelists_Count() == 40);
    assert(notes == 1);

    /* A replacement takes no slot, and its name answers to the disc's too, so
     * another mod's "decks" keeps naming it. */
    build("{\"duelists\":[{\"replace\":\"Heishin\",\"name\":\"Heishin the Elder\",\"slot\":50}]}", NULL);
    assert(Duelists_Count() == 40 && !strcmp(Duelists_Name(8), "Heishin the Elder"));
    assert(notes == 1); /* slot on a replacement is left out, and said so */

    /* Its own way of playing, over the row it names -- settled after every
     * mod is read, so it may name a duelist a later one adds. */
    memset(gDuel_aOpponentData, 0, sizeof gDuel_aOpponentData);
    gDuel_aOpponentData[38][0] = 20;
    build("{\"duelists\":[{\"id\":\"early\",\"copy\":\"Teana\",\"ai\":{\"copy\":\"Late\"}}]}",
          "{\"duelists\":[{\"id\":\"late\",\"copy\":\"Nitemare\",\"name\":\"Late\"}]}");
    assert(Duelists_AiRow(40)[0] == 20);

    /* No conditions is shown from the start. */
    memset(save, 0, sizeof save);
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\"}]}", NULL);
    assert(Duelists_Unlocked(save, 40) && !Duelists_HasUnlock(40));

    /* Beaten once by default, and the count when one is given. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"beat\":\"Simon Muran\"}},"
          "{\"id\":\"y\",\"copy\":\"Teana\",\"unlock\":{\"beat\":\"Simon Muran\",\"wins\":3}}]}", NULL);
    memset(save, 0, sizeof save);
    assert(!Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    wins(1, 1);
    assert(Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    wins(1, 3);
    assert(Duelists_Unlocked(save, 41));

    /* Wins on their own are every duelist's together. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"wins\":5}}]}", NULL);
    memset(save, 0, sizeof save);
    wins(1, 2); wins(2, 2);
    assert(!Duelists_Unlocked(save, 40));
    wins(3, 1);
    assert(Duelists_Unlocked(save, 40));

    /* A campaign flag on its own, which is what a replacement stands on in
     * place of the flag the screen would have tested. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"story\":12}}]}", NULL);
    memset(save, 0, sizeof save);
    story_flag = 0;
    assert(!Duelists_Unlocked(save, 40));
    story_flag = 12;
    assert(Duelists_Unlocked(save, 40));

    /* Every condition must hold. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\","
          "\"unlock\":{\"beat\":\"Simon Muran\",\"story\":12,\"card\":\"Blue-Eyes White Dragon\"}}]}", NULL);
    memset(save, 0, sizeof save);
    cards_named = 122;
    story_flag = 0;
    wins(1, 1);
    assert(!Duelists_Unlocked(save, 40));      /* no flag, no card */
    story_flag = 12;
    assert(!Duelists_Unlocked(save, 40));      /* still no card */
    save[0x50 + 122] = 1;
    assert(Duelists_Unlocked(save, 40));
    wins(1, 0);
    assert(!Duelists_Unlocked(save, 40));      /* and the win is needed too */

    /* A card in the deck counts as one held. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"card\":\"x\",\"copies\":2}}]}", NULL);
    memset(save, 0, sizeof save);
    cards_named = 122;
    save[0x50 + 122] = 1;
    assert(!Duelists_Unlocked(save, 40));
    ((unsigned short *)save)[0] = 122;
    assert(Duelists_Unlocked(save, 40));

    /* A condition naming something that is not here leaves it locked, so a
     * roster never opens up because a mod was turned off. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"beat\":\"Nobody At All\"}}]}", NULL);
    memset(save, 0, sizeof save);
    for (i = 1; i < 40; i++) wins(i, 99);
    assert(!Duelists_Unlocked(save, 40));

    /* A stock duelist a mod replaced may carry conditions, and says so, which
     * is how the screen knows to leave its campaign flag alone. */
    build("{\"duelists\":[{\"replace\":\"Nitemare\",\"unlock\":{\"beat\":\"Heishin\"}}]}", NULL);
    memset(save, 0, sizeof save);
    assert(Duelists_HasUnlock(38) && !Duelists_Unlocked(save, 38));
    wins(8, 1);
    assert(Duelists_Unlocked(save, 38));
    assert(!Duelists_HasUnlock(37) && Duelists_Unlocked(save, 37));

    /* The tower's condition: a floor won, or the whole tower with true. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"tower\":2}},"
          "{\"id\":\"y\",\"copy\":\"Teana\",\"unlock\":{\"tower\":true}}]}", NULL);
    memset(save, 0, sizeof save);
    tower_floors_won = 0;
    assert(Duelists_HasUnlock(40) && !Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    tower_floors_won = 2;
    assert(Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    tower_floors_won = -1;
    assert(Duelists_Unlocked(save, 41));
    assert(notes == 0);
    /* A duelist of the tower's alone: off the grid until a won floor names
     * it, whatever else its unlock says, which must hold too. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"tower_only\":true},"
          "{\"id\":\"y\",\"copy\":\"Teana\",\"tower_only\":true,\"unlock\":{\"beat\":\"Simon Muran\"}}]}",
          NULL);
    memset(save, 0, sizeof save);
    tower_frees = 0;
    tower_floors_won = 0;
    assert(Duelists_HasUnlock(40) && !Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    tower_frees = 40;
    assert(Duelists_Unlocked(save, 40) && !Duelists_Unlocked(save, 41));
    tower_frees = 41;
    assert(!Duelists_Unlocked(save, 41));      /* freed, but Simon not beaten */
    wins(1, 1);
    assert(Duelists_Unlocked(save, 41));
    tower_frees = 0;
    /* An added duelist with conditions of its own, not the tower's alone: a
     * floor that unlocks it outright opens it whatever they say. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"beat\":\"Seto\",\"wins\":3}}]}", NULL);
    memset(save, 0, sizeof save);
    assert(!Duelists_Unlocked(save, 40));
    tower_opens = 40;
    assert(Duelists_Unlocked(save, 40));
    tower_opens = 0;
    /* Written wrong: "tower" a word, tower_only in quotes, and on a stock
     * duelist, which keeps its campaign; each noted and left out. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"unlock\":{\"tower\":\"top\"}},"
          "{\"id\":\"y\",\"copy\":\"Teana\",\"tower_only\":\"true\"},"
          "{\"replace\":\"Nitemare\",\"tower_only\":true}]}", NULL);
    assert(notes == 4);   /* the tower word, then the unlock with no condition left, the quotes, the stock one */
    assert(!Duelists_HasUnlock(40) && !Duelists_HasUnlock(41) && !Duelists_HasUnlock(38));
    memset(save, 0, sizeof save);
    assert(Duelists_Unlocked(save, 40) && Duelists_Unlocked(save, 41));

    /* What a mod got wrong is left out, not guessed at. */
    build("{\"duelists\":[{\"id\":\"a\",\"copy\":\"Nobody\"},{\"id\":\"b\",\"copy\":\"Heishin\",\"slot\":3},"
          "{\"id\":\"c\",\"copy\":\"Heishin\",\"replace\":\"Nobody\"},"
          "{\"id\":\"d\",\"copy\":\"Heishin\",\"unlock\":7}]}", NULL);
    assert(notes == 4);
    assert(Duelists_Count() == 42);           /* b and d are added, at 40 and 41 */
    assert(!Duelists_HasUnlock(41));          /* the unlock that was not an object */

    /* A folder of duelists, one to a file, the file's own name being the id.
     * They are read in the order the names sort and not the order the
     * filesystem gives them, so the places they take are the same
     * everywhere. */
    {
        static const char *const files[] = {"zeta", "alpha", "middle"};
        static const char *const copies[] = {"Heishin", "Teana", "Nitemare"};
        char text[128];
        for (i = 0; i < 3; i++) {
            snprintf(text, sizeof text, "{\"copy\":\"%s\",\"name\":\"%s\"}", copies[i], files[i]);
            roster_put(files[i], text);
        }
        /* The win and loss sidecars live in this folder too, named by the
         * save and ending .txt. They are not duelists. */
        roster_litter("0000ABCD.txt");
        roster_litter("README");
        directory = ROSTER;
        build("{}", NULL);
        directory = ".";
        assert(Duelists_Count() == 43);
        /* alpha, middle, zeta -- sorted, not as the directory listed them. */
        assert(!strcmp(Duelists_Name(40), "alpha") && !strcmp(Duelists_Name(41), "middle"));
        assert(!strcmp(Duelists_Name(42), "zeta"));
        /* The file names them, so each is identified by its own. */
        assert(!strcmp(Duelists_Identity(40), "a:alpha"));
        assert(Duelists_BaseId(41) == 38 && Duelists_BaseId(42) == 8);
        assert(!notes);

        /* An entry in a folder may say everything a manifest's would. */
        roster_put("alpha", "{\"copy\":\"Teana\",\"slot\":44,\"unlock\":{\"beat\":\"Heishin\"}}");
        directory = ROSTER;
        build("{}", NULL);
        directory = ".";
        assert(Duelists_Valid(44) && !strcmp(Duelists_Identity(44), "a:alpha"));
        memset(save, 0, sizeof save);
        assert(!Duelists_Unlocked(save, 44));
        wins(8, 1);
        assert(Duelists_Unlocked(save, 44));
        {
            char path[256];
            snprintf(path, sizeof path, "%s/duelists/0000ABCD.txt", ROSTER);
            remove(path);
            snprintf(path, sizeof path, "%s/duelists/README", ROSTER);
            remove(path);
        }
        roster_drop(files, 3);
    }

    /* Whether its searches read a face-down card. The disc settles this in
     * the script's bytecode from the id, so a duelist that says nothing gets
     * back whatever the script asked for; one that says overrides it. The
     * sense is the searches': 0 reads them, non-zero passes them over. */
    build("{\"duelists\":[{\"id\":\"sees\",\"copy\":\"Teana\",\"ai\":{\"sight\":true}},"
          "{\"id\":\"blind\",\"copy\":\"Heishin\",\"ai\":{\"sight\":false}},"
          "{\"id\":\"quiet\",\"copy\":\"Heishin\",\"ai\":{\"search\":9}}]}", NULL);
    assert(!Duelists_HidesFaceDown(40, 1) && !Duelists_HidesFaceDown(40, 0));
    assert(Duelists_HidesFaceDown(41, 0) && Duelists_HidesFaceDown(41, 1));
    /* Saying nothing leaves the script's answer alone, either way. */
    assert(Duelists_HidesFaceDown(42, 1) && !Duelists_HidesFaceDown(42, 0));
    /* And a duelist the disc has, which no mod touched. */
    assert(Duelists_HidesFaceDown(8, 1) && !Duelists_HidesFaceDown(8, 0));
    /* A replacement may say for one of the disc's own. */
    build("{\"duelists\":[{\"id\":\"x\",\"replace\":\"Nitemare\",\"ai\":{\"sight\":false}}]}", NULL);
    assert(Duelists_HidesFaceDown(38, 0) && Duelists_HidesFaceDown(38, 1));
    /* The array form is the numbers alone and cannot say. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"ai\":[20,20,10,3,2]}]}", NULL);
    assert(Duelists_HidesFaceDown(40, 1) && !Duelists_HidesFaceDown(40, 0));

    /* "search" is the row's byte 0, which Ai_GetHandSize reads: 5 to 20,
     * the most the AI's fusion search keeps flags for; past either end it is
     * held there, and said so. */
    notes = 0;
    build("{\"duelists\":[{\"id\":\"x\",\"replace\":\"Simon Muran\",\"ai\":{\"search\":20}},"
          "{\"id\":\"y\",\"copy\":\"Heishin\",\"ai\":{\"search\":40}},"
          "{\"id\":\"z\",\"copy\":\"Heishin\",\"ai\":[0, 20]}]}", NULL);
    assert(Duelists_AiRow(1)[0] == 20 && Duelists_AiRow(40)[0] == 20 && Duelists_AiRow(41)[0] == 5);
    assert(notes == 2);

    /* How a duel against it is scored is the duelist's own, and reaches a
     * stock one through a replacement that changes nothing else. A rule with
     * fewer than five pairs is filled out with the last, and the last
     * threshold always ends the walk. */
    set_count = 0;
    build("{\"duelists\":[{\"id\":\"x\",\"replace\":\"Nitemare\","
          "\"ranks\":{\"turns\":[[3,12],[6,4],[12,0],[20,-20],[32767,-40]],"
          "\"cards used\":[[9,15]]}}]}", NULL);
    assert(set_count == 2);
    assert(set_rows[38][0][0] == 3 && set_rows[38][0][1] == 12);
    assert(set_rows[38][0][8] == 32767 && set_rows[38][0][9] == -40);
    /* "cards used" gave one pair; the rest are that pair. */
    assert(set_rows[38][6][0] == 9 && set_rows[38][6][1] == 15);
    assert(set_rows[38][6][8] == 9 && set_rows[38][6][9] == 15);
    /* A rule nobody has heard of is said so and left out. */
    set_count = 0;
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"ranks\":{\"vibes\":[[1,1]]}}]}", NULL);
    assert(!set_count && notes == 1);

    /* The list hands the mod API a resolver, so a code mod can turn an
     * identity into the id it has this run (modapi.h: duelist_id). */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"slot\":44}]}", NULL);
    assert(given_resolver == Duelists_Find);
    assert(given_resolver("a:x") == 44);
    assert(given_resolver("a:nobody") < 0);   /* the host reports 0 for this */

    /* Two entries cannot share an identity: the second is left out. */
    build("{\"duelists\":[{\"id\":\"same\",\"copy\":\"Heishin\"},{\"id\":\"same\",\"copy\":\"Teana\"}]}", NULL);
    assert(Duelists_Count() == 41 && notes == 1);

    /* The string that names a duelist. A stock one, replaced or not, keeps the
     * disc's id; an added one has a private id, because 0x8328 + 40 onwards
     * are the campaign's location names. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"name\":\"Ex\"},"
          "{\"id\":\"o\",\"replace\":\"Teana\",\"name\":\"Oh\"}]}", NULL);
    assert(Duelists_NameTextId(8) == 0x8328 + 8);
    assert(Duelists_NameTextId(2) == 0x8328 + 2);
    assert(Duelists_NameTextId(40) == 0xFF00);
    assert(Duelists_NameTextId(41) == 0xFF01);
    {
        static const unsigned char stock[] = {'A', 0xFF};
        /* The replacement answers on the stock id; the added one does not. */
        assert(Duelists_Text(Duelists_NameTextId(2), stock) != stock);
        assert(Duelists_Text(Duelists_NameTextId(40), stock) != stock);
        /* A location name is left alone: 0x8350 is Metropolis, not duelist 40. */
        assert(Duelists_Text(0x8328 + DUELISTS_RETAIL_COUNT, stock) == stock);
        assert(Duelists_Text(0x8328 + DUELISTS_RETAIL_COUNT + 7, stock) == stock);
        /* Nor is a composed line (page_box.h, passwords.h, drops.h). */
        assert(Duelists_Text(0xFFFD, stock) == stock);
        assert(Duelists_Text(0xFFFF, stock) == stock);
        /* An added duelist with no name of its own is still named: its
         * base's, since no string on the disc answers its private id. */
        build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\"}]}", NULL);
        assert(!strcmp(Duelists_Name(40), "Heishin"));
        assert(Duelists_Text(Duelists_NameTextId(40), stock) != stock);
    }

    /* The name a mod may use for a duelist, which the rule tables ask for too
     * (Duelists_Named). A stock duelist a mod renamed still answers to the
     * name the disc gave it, so another mod's "decks" keeps naming it; an
     * empty slot is nobody and answers to nothing. */
    build("{\"duelists\":[{\"id\":\"o\",\"replace\":\"Simon Muran\",\"name\":\"Dark Simon\"},"
          "{\"id\":\"x\",\"copy\":\"Heishin\",\"name\":\"Ex\",\"slot\":42}]}", NULL);
    assert(Duelists_Named("Dark Simon") == 1);      /* the name it has now */
    assert(Duelists_Named("Simon Muran") == 1);     /* and the disc's, still */
    assert(Duelists_Named("a:o") == 1);             /* and the identity */
    assert(Duelists_Named("Ex") == 42 && Duelists_Named("42") == 42);
    assert(Duelists_Named("40") < 0);               /* the gap before slot 42 */
    assert(Duelists_Named("Deck Build") == 0);      /* but not through a gap */
    assert(Duelists_Named("nobody at all") < 0);

    /* The records beside the save, round-tripped by identity. An identity may
     * hold spaces -- duelists/Dark Simon.json is "a:Dark Simon" -- so it is
     * written last on the line and read whole. */
    build("{\"duelists\":[{\"id\":\"Dark Simon\",\"copy\":\"Heishin\"},"
          "{\"id\":\"plain\",\"copy\":\"Teana\"}]}", NULL);
    sidecars = 1;
    mkdir("duelists_test_user", 0777);
    memset(save, 0, sizeof save);
    memcpy(save + 0x334, "\x01\x00\x00\x00", 4);       /* the duelist code */
    memcpy(save + 0x404, "\x07\x00\x00\x00", 4);       /* the save sequence */
    assert(!strcmp(Duelists_Identity(40), "a:Dark Simon"));
    Duelists_RecordSlot(save, 40)[0] = 9;
    Duelists_RecordSlot(save, 40)[1] = 4;
    Duelists_RecordSlot(save, 41)[0] = 2;
    Duelists_SaveWritten(save, 7);
    Duelists_RecordSlot(save, 40)[0] = 0;
    Duelists_RecordSlot(save, 40)[1] = 0;
    Duelists_RecordSlot(save, 41)[0] = 0;
    Duelists_SaveLoaded(save);
    assert(Duelists_RecordSlot(save, 40)[0] == 9);      /* the spaced identity */
    assert(Duelists_RecordSlot(save, 40)[1] == 4);
    assert(Duelists_RecordSlot(save, 41)[0] == 2);
    remove("duelists_test_user/duelists/00000001.txt");
    rmdir("duelists_test_user/duelists");
    rmdir("duelists_test_user");
    sidecars = 0;

    /* NEW GAME writes a new duelist code into the running save without
     * loading one: the records of the save before it must not stand. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\"}]}", NULL);
    memset(gDuel_awPlayerDeck, 0, sizeof gDuel_awPlayerDeck);
    gFreeDuel_nExtraOwner = 0;
    Duelists_RecordSlot(save, 40)[0] = 3;
    assert(Duelists_RecordSlot(save, 40)[0] == 3);
    memcpy((unsigned char *)gDuel_awPlayerDeck + 0x334, "\xEF\xBE\xAD\xDE", 4);
    Duelists_Frame();
    assert(Duelists_RecordSlot(save, 40)[0] == 0);
    Duelists_RecordSlot(save, 40)[1] = 5;
    Duelists_Frame();                    /* the same save: nothing is cleared */
    assert(Duelists_RecordSlot(save, 40)[1] == 5);

    /* A bust (busts.h): an added duelist's is its own; a disc duelist's is
     * the campaign's only with "replace_bust" on a "replace" entry, which
     * leaves its name, face and play the disc's. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"bust\":\"busts/x.png\"},"
          "{\"replace\":\"Seto\",\"bust\":{\"expressions\":[]}},"
          "{\"replace\":\"Teana\",\"bust\":\"busts/t.png\",\"replace_bust\":true}]}", NULL);
    assert(Duelists_Bust(40) == 0 && !Duelists_ReplacesBust(40));
    assert(Duelists_Bust(7) == 1 && !Duelists_ReplacesBust(7));
    assert(Duelists_Bust(2) == 2 && Duelists_ReplacesBust(2));
    assert(!strcmp(Duelists_Name(2), "Teana") && !Duelists_Portrait(2) && notes == 0);
    assert(Duelists_Bust(1) == -1 && !Duelists_ReplacesBust(1));
    /* "replace_bust" on an added duelist, without a bust, or not a bool:
     * each said, none taking hold. */
    build("{\"duelists\":[{\"id\":\"x\",\"copy\":\"Heishin\",\"bust\":\"busts/x.png\",\"replace_bust\":true},"
          "{\"replace\":\"Seto\",\"replace_bust\":true},"
          "{\"replace\":\"Teana\",\"bust\":\"busts/t.png\",\"replace_bust\":\"yes\"}]}", NULL);
    assert(notes == 3);
    assert(!Duelists_ReplacesBust(40) && !Duelists_ReplacesBust(7) && !Duelists_ReplacesBust(2));
    assert(Duelists_Bust(40) == 0 && Duelists_Bust(7) == -1 && Duelists_Bust(2) == 1);

    Duelists_Clear();
    puts("duelists: places, replacements, borrowed rows, unlock conditions and busts passed");
    return 0;
}
