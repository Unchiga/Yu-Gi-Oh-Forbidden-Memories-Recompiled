/* View > Free Duel progress's count (src/pc/cards/free_duel_progress.c)
 * over a made-up WA_MRG and the real mod tables (tables.c): the union of
 * the three drop pools, the mods' edits, and the deck and trunk held. */
#include "../../src/pc/cards/tables.c"
#include "../../src/pc/cards/free_duel_progress.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

int gCard_nCount = 800;
signed char gDuel_bOpponentID;
unsigned short gDuel_awPlayerDeck[0x1000];
static unsigned char extra_chest[1024];

/* Cards 1-722 are the disc's, 723-800 made by mods (tables_test.c's list). */
int Cards_Valid(int id) { return id >= 1 && id <= gCard_nCount; }
int Cards_BaseId(int id) { return Cards_Valid(id) ? (id > CARD_COUNT ? id - CARD_COUNT : id) : 0; }
int Cards_Type(int id) { (void)id; return 3; }
int Cards_EffectId(int id) { return Cards_BaseId(id); }
int Cards_RetailType(int id) { (void)id; return -1; }
int Cards_TypeNamed(const char *text) { (void)text; return -1; }
int Cards_IsMonsterType(int type) { return type >= 0 && (type < CARD_TYPE_MAGIC || type > CARD_TYPE_EQUIP); }
int Cards_FusionGroupNamed(const char *text) { (void)text; return 0; }
int Cards_Attribute(int id) { (void)id; return 0; }
int Cards_AttributeNamed(const char *text) { (void)text; return -1; }
int Cards_Named(const char *text)
{
    if (strspn(text, "0123456789") == strlen(text)) return Cards_Valid(atoi(text)) ? atoi(text) : -1;
    return -1;
}
int Cards_Reference(const JsonValue *value)
{
    if (!value || Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) return Cards_Valid((int)Json_Number(value, 0)) ? (int)Json_Number(value, 0) : -1;
    return Cards_Named(Json_String(value, ""));
}
/* The trunk after the deck, as in the save (SAVE_CHEST); mod cards apart. */
unsigned char *Cards_ChestSlot(void *state, int id)
{
    static unsigned char nowhere;
    if (id >= 1 && id <= CARD_COUNT) return (unsigned char *)state + 0x50 + id - 1;
    nowhere = 0;
    return Cards_Valid(id) ? &extra_chest[id] : &nowhere;
}
int Mods_EntryUsed(const char *id, const JsonValue *entry, const char *where) { (void)id; (void)entry; (void)where; return 1; }
long Mods_EachFusion(const char *id, int (*each)(const JsonValue *, long, void *), void *context, char *error,
                     size_t error_size)
{
    (void)id; (void)each; (void)context; (void)error; (void)error_size;
    return 0;
}
void Mods_RefuseApplying(const char *id, const char *why) { (void)id; (void)why; }
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "note %s: ", id);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}
int Log_Wanted(LogChannel channel) { (void)channel; return 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel; (void)format; }
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { return mod >= 0; }
const char *Mods_Id(int mod) { (void)mod; return ""; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }

/* The disc: WA_MRG from sector 1000, its drop blocks where the retail file
 * has them. */
#define FAKE_LBA 1000
static unsigned char blocks[DUELISTS][DROPS_SECTORS * 2048];
static int disc_ok = 1, sectors_read;
int Memories_DiscFileInfo(const char *path, int *lba, unsigned *size)
{
    if (!disc_ok || strcmp(path, WA_PATH)) return -1;
    *lba = FAKE_LBA;
    *size = 0x1000000;
    return 0;
}
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    int at = lba - FAKE_LBA - DROPS_SECTOR;
    assert(sectors == DROPS_SECTORS && at >= 0 && at % DROPS_SECTORS == 0 && at / DROPS_SECTORS < DUELISTS);
    memcpy(out, blocks[at / DROPS_SECTORS], sizeof(blocks[0]));
    sectors_read += sectors;
    return sectors;
}
/* Row 0 is the deck pool; 1-3 the drop pools. */
static void weight(int duelist, int row, int id, int value)
{
    unsigned char *at = blocks[duelist - 1] + row * DROP_ROW + (id - 1) * 2;
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
}

static JsonDocument *documents[8];
static int document_count;
static void add(const char *mod, const char *text)
{
    char error[128];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    assert(document);
    documents[document_count++] = document;
    Tables_Add(mod, Json_Root(document));
}

static void expect(int duelist, int owned, int obtainable)
{
    int have = -1, can = -1;
    assert(FreeDuelProgress_Count(duelist, &have, &can));
    if (have != owned || can != obtainable)
        fprintf(stderr, "duelist %d: %d/%d, expected %d/%d\n", duelist, have, can, owned, obtainable);
    assert(have == owned && can == obtainable);
}

int main(void)
{
    int have, can;
    /* Simon Muran (1): POW 5 and 6, B/C/D 6, TEC 7; card 8 at weight 0;
     * card 9 only in his deck pool, which he does not give away. */
    weight(1, 1, 5, 1024);
    weight(1, 1, 6, 1024);
    weight(1, 2, 6, 2048);
    weight(1, 3, 7, 2048);
    weight(1, 3, 8, 0);
    weight(1, 0, 9, 2048);
    /* Teana (2): 100-199 at 20 each, in all three. */
    for (have = 100; have < 200; have++) {
        weight(2, 1, have, 20);
        weight(2, 2, have, 20);
        weight(2, 3, have, 20);
    }
    /* Duel Master K (39), the last block: card 722. */
    weight(39, 1, 722, 2048);

    /* Nothing held yet. */
    expect(1, 0, 3);
    expect(2, 0, 100);
    expect(39, 0, 1);
    assert(sectors_read == DUELISTS * DROPS_SECTORS);
    /* Read once. */
    expect(1, 0, 3);
    assert(sectors_read == DUELISTS * DROPS_SECTORS);
    /* No opponent 0 (Build Deck) or 40. */
    assert(!FreeDuelProgress_Count(0, &have, &can) && !FreeDuelProgress_Count(40, &have, &can));

    /* Two in the trunk, one in the deck, one in both: each card once. */
    *Cards_ChestSlot(gDuel_awPlayerDeck, 5) = 2;
    gDuel_awPlayerDeck[3] = 7;
    gDuel_awPlayerDeck[4] = 7;
    gDuel_awPlayerDeck[10] = 150;
    *Cards_ChestSlot(gDuel_awPlayerDeck, 150) = 1;
    *Cards_ChestSlot(gDuel_awPlayerDeck, 9) = 3; /* not obtainable from him */
    expect(1, 2, 3);
    expect(2, 1, 100);

    /* A mod adds a retail card and a card of its own to his S/A-POW, and
     * takes card 7 out of his S/A-TEC (the pool the drop roll would use). */
    add("drops", "{\"drops\": {\"Simon Muran\": {\"pow\": {\"10\": 40, \"723\": 20}, \"tec\": {\"7\": 0, \"11\": 10}}}}");
    FreeDuelProgress_Reset();
    expect(1, 1, 5); /* 5, 6, 10, 723, 11 */
    extra_chest[723] = 1;
    expect(1, 2, 5);
    expect(2, 1, 100); /* the others as they were */
    /* An edit for everyone reaches every opponent. */
    add("all", "{\"drops\": {\"all\": {\"bcd\": {\"12\": 8}}}}");
    FreeDuelProgress_Reset();
    expect(1, 2, 6);
    expect(2, 1, 101);
    expect(39, 0, 2);

    /* Fewer cards (a mod left out): worked out again, 723 no longer a card. */
    gCard_nCount = CARD_COUNT;
    expect(1, 1, 5);
    gCard_nCount = 800;

    /* A disc without the file: nothing to show. */
    FreeDuelProgress_Reset();
    disc_ok = 0;
    assert(!FreeDuelProgress_Count(1, &have, &can));
    disc_ok = 1;
    assert(!FreeDuelProgress_Count(1, &have, &can)); /* not tried again */

    FreeDuelProgress_Reset();
    Tables_Clear();
    while (document_count) Json_Free(documents[--document_count]);
    puts("free duel progress: ok");
    return 0;
}
