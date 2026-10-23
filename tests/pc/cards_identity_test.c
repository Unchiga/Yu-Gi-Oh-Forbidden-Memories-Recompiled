/* Test save remapping independently of the retail data/image renderer. */
#include "pc/compat/fs.h"
#include "../../src/pc/cards/cards.c"
#include "scratch.h"
#include <assert.h>
#include <unistd.h>
int gCard_nCount = 724, gCard_nExtraOwner;
unsigned short gCard_awBaseId[CARD_TABLE_ID_END], gDuel_awPlayerDeck[1024];
unsigned char gCard_abExtraChest[CARD_TABLE_ID_END], gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8];
unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END], gCard_abPairPending[2][CARD_TABLE_ID_END];
int Log_Wanted(LogChannel channel)
{
    (void)channel;
    return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...)
{
    (void)channel;
    (void)format;
}
/* Card text: letters stand for themselves here, so the codes show. */
uint32_t Glyphs_NextCharacter(const char **text) { return (unsigned char)*(*text)++; }
int Glyphs_Code(uint32_t character) { return character >= 'A' && character <= 'z' ? (int)character : -1; }
/* No mod adds a battle model here (mods.c's "models"). */
int Mods_ModelCount(void) { return 0; }
int Mods_Model(int i, const char **name, const struct JsonValue **base, int *mod)
{
    (void)i; (void)name; (void)base; (void)mod;
    return -1;
}
int Mods_ModelSector(int i) { (void)i; return -1; }
int Mods_ModelWindow(int slot, int i) { (void)slot; (void)i; return -1; }

void Mods_Note(const char *id, const char *format, ...)
{
    (void)id;
    (void)format;
}
static void card_text_codes(void)
{
    /* As the FM Editor shows a ROM hack's text: an icon is two letters of
     * the line (the card view draws it 16 pixels across), a color none,
     * and both are the game's own bytes. */
    static const unsigned char icon[] = {'a', 0, 0xF8, 0x0B, 0x04, 0, 'm', 0xFF};
    static const unsigned char color[] = {0xF8, 0x0A, 0x02, 'R', 'e', 'd', 0xFE, 'G', 0xF1, 0x23, 0xFF};
    unsigned char *text = encode_description("t", "a {f8 0B 04} m", 1);
    assert(!memcmp(text, icon, sizeof(icon)));
    Memories_LowFree(text);
    text = encode_description("t", "{f8 0A 02}Red\nG{g 123}", 1);
    assert(!memcmp(text, color, sizeof(color)));
    Memories_LowFree(text);
    /* Twenty letters with the icon's two, so "c" still fits; one more
     * letter before it and "c" goes to the next line. An unknown code is its
     * letters (and the braces, which this stub has no glyph for). */
    text = encode_description("t", "aaaaaaaaaaaaaaa {f8 0B 04} c", 1);
    assert(text[15] == 0 && text[16] == 0xF8 && text[19] == 0 && text[20] == 'c' && text[21] == 0xFF);
    Memories_LowFree(text);
    text = encode_description("t", "aaaaaaaaaaaaaaaa {f8 0B 04} c", 1);
    assert(text[16] == 0 && text[17] == 0xF8 && text[20] == 0xFE && text[21] == 'c' && text[22] == 0xFF);
    Memories_LowFree(text);
    text = encode_description("t", "{f8 99 04}", 1);
    assert(text[0] == 'f' && text[1] == 0);   /* "f", a space, and the rest has no glyph */
    Memories_LowFree(text);
}
static void set_recent(unsigned char *state, const unsigned short *ids)
{
    memcpy(state + SAVE_RECENT, ids, DUEL_RECENT_CARD_DROP_COUNT * sizeof(*ids));
}

static int recent_at(const unsigned char *state, int index)
{
    unsigned short id;
    memcpy(&id, state + SAVE_RECENT + index * 2, sizeof(id));
    return id;
}

static void read_sidecar(int code, char *text, size_t size)
{
    char path[1024];
    FILE *file;
    size_t got;
    assert(!sidecar_path(path, sizeof(path), code));
    file = fopen(path, "r");
    assert(file);
    got = fread(text, 1, size - 1, file);
    text[got] = 0;
    fclose(file);
}

/* The save's recent-drops list (Build Deck's "New!"): added ids follow
 * their identity through a renumbering, the disc's never move. */
static void recent_drops(unsigned char *state)
{
    static const unsigned short saved[DUEL_RECENT_CARD_DROP_COUNT] = {724, 5, 723, 730, 724};
    char text[4096], path[1024];
    int code = 789, count = 0;
    unsigned sequence = 1;
    const char *at;
    FILE *file;
    Cards_SetSlotTokens(0, NULL, 0);
    memset(state, 0, 2048);
    memset(gCard_abExtraChest, 0, sizeof(gCard_abExtraChest));
    memset(gCard_abExtraSeen, 0, sizeof(gCard_abExtraSeen));
    memcpy(state + SAVE_DUELIST_CODE, &code, 4);
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    gCard_nCount = 724;
    identities[723] = "alpha:dragon:1"; gCard_awBaseId[723] = 1;
    identities[724] = "beta:mage:1"; gCard_awBaseId[724] = 2;
    /* Saved in order alpha, beta: 730 is a card this run does not have. */
    set_recent(state, saved);
    Cards_SaveWritten(state, 1);
    read_sidecar(code, text, sizeof(text));
    assert(strstr(text, "recent2 0 724 beta:mage:1\n"));
    assert(strstr(text, "recent2 2 723 alpha:dragon:1\n"));
    assert(strstr(text, "recent2 3 730 -\n"));
    assert(strstr(text, "recent2 4 724 beta:mage:1\n"));
    assert(!strstr(text, "recent2 1 "));   /* the disc's card 5 needs no line */
    /* Loaded with the order turned round: beta is 723 now, alpha 724. */
    identities[723] = "beta:mage:1"; gCard_awBaseId[723] = 2;
    identities[724] = "alpha:dragon:1"; gCard_awBaseId[724] = 1;
    Cards_SaveLoaded(state);
    assert(recent_at(state, 0) == 723 && recent_at(state, 1) == 5 && recent_at(state, 2) == 724);
    assert(recent_at(state, 3) == 0 && recent_at(state, 4) == 723 && recent_at(state, 5) == 0);
    /* A trade rewrites the section without the list: its lines stay, once. */
    write_section(code, 1, 0, gCard_abExtraChest, gCard_abExtraSeen, (const unsigned short *)state, NULL);
    read_sidecar(code, text, sizeof(text));
    assert(strstr(text, "recent2 0 724 beta:mage:1\n") && strstr(text, "recent2 3 730 -\n"));
    for (at = text; (at = strstr(at, "recent2 ")); at++) count++;
    assert(count == 4);
    /* A later save with no section of its own reads save 1's: an old id is
     * found wherever it now stands in the list. */
    sequence = 2;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    {
        const unsigned short later[DUEL_RECENT_CARD_DROP_COUNT] = {9, 8, 724, 5, 723};
        set_recent(state, later);
    }
    Cards_SaveLoaded(state);
    assert(recent_at(state, 0) == 9 && recent_at(state, 1) == 8);
    assert(recent_at(state, 2) == 723 && recent_at(state, 3) == 5 && recent_at(state, 4) == 724);
    /* Alpha's mod is missing: its entries are none, beta's still found. */
    sequence = 1;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    set_recent(state, saved);
    gCard_nCount = 723;
    identities[723] = "beta:mage:1";
    identities[724] = NULL;
    Cards_SaveLoaded(state);
    assert(recent_at(state, 0) == 723 && recent_at(state, 1) == 5 && recent_at(state, 2) == 0);
    /* A section from before recent2 says nothing of the list: it is kept. */
    gCard_nCount = 724;
    identities[724] = "alpha:dragon:1";
    assert(!sidecar_path(path, sizeof(path), code));
    file = fopen(path, "w");
    assert(file);
    fputs("save 1\nchest2 alpha:dragon:1 2\ndeck2 0 723 1 alpha:dragon:1\nend\n", file);
    fclose(file);
    set_recent(state, saved);
    Cards_SaveLoaded(state);
    assert(!memcmp(state + SAVE_RECENT, saved, sizeof(saved)));
    /* No sidecar at all (a save that never met a card mod): kept too. */
    remove(path);
    Cards_SaveLoaded(state);
    assert(!memcmp(state + SAVE_RECENT, saved, sizeof(saved)));
}

int main(void)
{
    card_text_codes();
    char directory[SCRATCH_MAX];
    unsigned char state[2048] = {0};
    int code = 123;
    unsigned sequence = 1;
    assert(scratch_dir(directory, sizeof(directory), "memories-card-identities"));
    setenv("MEMORIES_USER_DIR", directory, 1);
    memcpy(state + SAVE_DUELIST_CODE, &code, 4);
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    identities[723] = "alpha:dragon:1";
    identities[724] = "beta:mage:1";
    gCard_awBaseId[723] = 1;
    gCard_awBaseId[724] = 2;
    gCard_abExtraChest[723] = 3;
    gCard_abExtraChest[724] = 5;
    gCard_abExtraSeen[723 >> 3] |= 1u << (723 & 7);
    ((unsigned short *)state)[0] = 723;
    Cards_SaveWritten(state, 1);
    identities[723] = "beta:mage:1";
    identities[724] = "alpha:dragon:1";
    gCard_awBaseId[723] = 2;
    gCard_awBaseId[724] = 1;
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[724] == 3 && gCard_abExtraChest[723] == 5);
    assert(((unsigned short *)state)[0] == 724);
    assert((gCard_abExtraSeen[724 >> 3] >> (724 & 7)) & 1);
    /* Temporarily remove alpha, save beta, then restore alpha: ownership survives. */
    gCard_nCount = 723;
    identities[724] = NULL;
    ((unsigned short *)state)[0] = 723;
    Cards_SaveLoaded(state);
    assert(((unsigned short *)state)[0] == 1);
    Cards_SaveWritten(state, 2);
    gCard_nCount = 724;
    identities[724] = "alpha:dragon:1";
    sequence = 2;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[724] == 3 && gCard_abExtraChest[723] == 5);
    /* Ambiguous legacy IDs are preserved, never assigned to a new card. */
    char path[1024], backup[1040];
    assert(!sidecar_path(path, sizeof(path), code));
    FILE *file = fopen(path, "w");
    assert(file);
    fputs("save 2\nchest 723 9\ndeck 0 723 1\nend\n", file);
    fclose(file);
    ((unsigned short *)state)[0] = 723;
    Cards_SaveLoaded(state);
    assert(!gCard_abExtraChest[723] && ((unsigned short *)state)[0] == 1);
    /* New progress is still saved beside the unmigrated legacy lines. */
    gCard_abExtraChest[724] = 4;
    Cards_SaveWritten(state, 3);
    file = fopen(path, "r");
    char text[1024] = {0};
    assert(file);
    fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    assert(strstr(text, "chest 723 9"));
    assert(strstr(text, "chest2 alpha:dragon:1 4"));
    gCard_abExtraChest[724] = 0;
    setenv("MEMORIES_MIGRATE_CARD_IDS", "1", 1);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[723] == 9);
    Cards_SaveWritten(state, 3);
    snprintf(backup, sizeof(backup), "%s.legacy", path);
    file = fopen(backup, "r");
    assert(file);
    fclose(file);
    unsetenv("MEMORIES_MIGRATE_CARD_IDS");
    sequence = 3;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[723] == 9);

    /* Two slots of one duelist at the same sequence (saved twice, then played
     * on from the older): each reads back its own cards, and a section a
     * slot still holds outlives any number of later saves. */
    {
        unsigned live[2] = {0xA1, 0xB2};
        code = 456;
        memcpy(state + SAVE_DUELIST_CODE, &code, 4);
        sequence = 10;
        memcpy(state + SAVE_SEQUENCE, &sequence, 4);
        memset(gCard_abExtraChest, 0, sizeof(gCard_abExtraChest));
        gCard_abExtraChest[724] = 1;
        Cards_SetSlotTokens(0xA1, live, 2);
        Cards_SaveWritten(state, 10);
        gCard_abExtraChest[724] = 2;
        Cards_SetSlotTokens(0xB2, live, 2);
        Cards_SaveWritten(state, 10);
        for (unsigned later = 11; later < 30; later++) {
            unsigned token = 0x100 + later;
            gCard_abExtraChest[724] = 7;
            Cards_SetSlotTokens(token, live, 2);
            Cards_SaveWritten(state, later);
        }
        Cards_SetSlotTokens(0xA1, live, 2);
        Cards_SaveLoaded(state);
        assert(gCard_abExtraChest[724] == 1);
        Cards_SetSlotTokens(0xB2, live, 2);
        Cards_SaveLoaded(state);
        assert(gCard_abExtraChest[724] == 2);
    }
    recent_drops(state);
    return 0;
}
