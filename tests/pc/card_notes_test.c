/* A card's notes (src/pc/cards/card_notes.h): tags read from them, and how
 * the "cards" entries of mods add up to them (cards.c). */
#include "../../src/pc/cards/cards.c"
#include <assert.h>
int gCard_nCount = 723, gCard_nExtraOwner;
unsigned short gCard_awBaseId[CARD_TABLE_ID_END], gDuel_awPlayerDeck[1024];
unsigned char gCard_abExtraChest[CARD_TABLE_ID_END], gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8];
unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END], gCard_abPairPending[2][CARD_TABLE_ID_END];
static int noted;
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
/* No mod adds a battle model here (mods.c's "models"). */
int Mods_ModelCount(void) { return 0; }
int Mods_Model(int i, const char **name, const struct JsonValue **base, int *mod)
{
    (void)i; (void)name; (void)base; (void)mod;
    return -1;
}
int Mods_ModelSector(int i) { (void)i; return -1; }
int Mods_ModelWindow(int slot, int i) { (void)slot; (void)i; return -1; }
int Mods_ModelPending(int slot) { (void)slot; return 0; }
void Mods_ModelForget(int slot) { (void)slot; }
void Mods_ModelParsed(int slot) { (void)slot; }
unsigned char *Mods_ModelArena(int arena, unsigned *size) { (void)arena; (void)size; return 0; }
int BattleModels_Check(int slot) { (void)slot; return 0; }

void Mods_Note(const char *id, const char *format, ...)
{
    (void)id;
    (void)format;
    noted++;
}

static void tags(void)
{
    const char *notes = "Burns the opponent. <burn: 300>\n<No-Fusion> < ref : mod:card:1 > <burn:450> <:x> <open";
    char out[16];
    assert(CardNotes_Tag(notes, "burn", out, sizeof(out)) == 3 && !strcmp(out, "450"));   /* the last one */
    assert(CardNotes_Tag(notes, "no-fusion", out, sizeof(out)) == 0 && !strcmp(out, ""));  /* any case */
    assert(CardNotes_Tag(notes, "ref", out, sizeof(out)) == 10 && !strcmp(out, "mod:card:1"));
    assert(CardNotes_Tag(notes, "open", out, sizeof(out)) == -1);          /* never closed */
    assert(CardNotes_Tag(notes, "", out, sizeof(out)) == -1);              /* no name, no tag */
    assert(CardNotes_Tag(notes, "burns", out, sizeof(out)) == -1);
    assert(CardNotes_Tag(notes, "bur", out, sizeof(out)) == -1);
    assert(CardNotes_Tag(notes, "ref", out, 4) == 10 && !strcmp(out, "mod"));   /* cut to fit */
    assert(CardNotes_Tag(notes, "ref", NULL, 0) == 10);
    assert(CardNotes_Tag("<a <b: 2>", "b", out, sizeof(out)) == 1 && !strcmp(out, "2"));
    assert(CardNotes_Tag("<a <b: 2>", "a", out, sizeof(out)) == -1);
    assert(CardNotes_Tag(NULL, "a", out, sizeof(out)) == -1 && !*out);
}

static const JsonValue *entry(JsonDocument **document, const char *text)
{
    char error[128];
    *document = Json_Parse(text, error, sizeof(error));
    assert(*document);
    return Json_Root(*document);
}

static void entries(void)
{
    JsonDocument *a, *b, *c, *d;
    char out[16];
    assert(notes_only(entry(&a, "{\"replace\": 5, \"notes\": \"First. <power: 1>\"}")));
    assert(!notes_only(entry(&b, "{\"replace\": 5, \"name\": \"X\", \"notes\": \"Second. <power: 2>\"}")));
    assert(!notes_only(entry(&c, "{\"replace\": 5}")));
    add_notes("one", 0, 5, Json_Member(Json_Root(a), "notes"));
    add_notes("two", 0, 5, Json_Member(Json_Root(b), "notes"));
    assert(!strcmp(Cards_Notes(5), "First. <power: 1>\nSecond. <power: 2>"));
    assert(Cards_NoteTag(5, "power", out, sizeof(out)) == 1 && !strcmp(out, "2"));
    assert(!Cards_Notes(6) && Cards_NoteTag(6, "power", out, sizeof(out)) == -1);
    assert(!Cards_Notes(0) && !Cards_Notes(CARD_TABLE_ID_END + 5));
    add_notes("three", 0, 6, Json_Member(entry(&d, "{\"notes\": 12}"), "notes"));
    assert(noted == 1 && !Cards_Notes(6));
    Json_Free(a);
    Json_Free(b);
    Json_Free(c);
    Json_Free(d);
}

int main(void)
{
    tags();
    entries();
    return 0;
}
