/* A card's "stars" as cards.c reads them (notes/modding.md, "No star"):
 * Cards_Build, add_entry and replace_model_effect on a stand-in for the
 * disc's tables.
 *
 * The bug this keeps away (Discord, 30/09): a retail monster replaced with
 * "stars": [0, 0] came back with its own retail stars, because the refill
 * meant for a magic card made a monster took any monster without star bits
 * for one. Also the copy entry, a magic card made a monster with and without
 * "stars", an earlier mod's no-star card under a later replace, [none, X],
 * and what is refused with a note. */
#define _POSIX_C_SOURCE 200809L /* before any header: cards.c's strdup */
#include <stdint.h>

/* The disc's tables, here. cards.c reads them at fixed addresses. */
static unsigned test_stats[722];
static short test_sort_keys[722];
static unsigned char test_level_attr[723];
static unsigned short test_name_offsets[723];
static unsigned char test_text_bank[16] = {0xFF};
static unsigned test_glyphs[256];
#define RETAIL_STATS ((uintptr_t)test_stats)
#define RETAIL_SORT_KEYS ((uintptr_t)test_sort_keys)
#define RETAIL_LEVEL_ATTR ((uintptr_t)test_level_attr)
#define RETAIL_NAME_OFFSETS ((uintptr_t)test_name_offsets)
#define TEXT_BANK ((uintptr_t)test_text_bank)
#define GLYPH_TABLE ((uintptr_t)test_glyphs)

#include "../../src/pc/cards/cards.c"
#include <assert.h>
#include <limits.h>

int gCard_nCount = CARD_COUNT, gCard_nExtraOwner;
unsigned short gCard_awBaseId[CARD_TABLE_ID_END], gDuel_awPlayerDeck[1024];
unsigned char gCard_abExtraChest[CARD_TABLE_ID_END],
    gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8];
unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END],
    gCard_abPairPending[2][CARD_TABLE_ID_END];
int gDuel_adwCardStats[CARD_TABLE_ID_END];
short gCard_asNameSortKey[CARD_TABLE_ID_END];
unsigned char gDuel_abCardLevelAttr[CARD_TABLE_ID_END];

static int
    notes[8]; /* per mod, "a" to "h" (h: not counting a slot replaced twice) */
static int effect_notes; /* of them, about "monster_effects" */

void Mods_Note(const char *id, const char *format, ...) {
  char note[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(note, sizeof(note), format, arguments);
  va_end(arguments);
  fprintf(stderr, "note: %s: %s\n", id, note);
  if (id[0] >= 'a' && id[0] <= 'h' && !id[1] &&
      (id[0] != 'h' || !strstr(note, "is replaced by")))
    notes[id[0] - 'a']++;
  if (strstr(note, "monster_effects"))
    effect_notes++;
}

/* What cards.c and stars.c reach that these checks never get to. */
int Log_Wanted(LogChannel channel) {
  (void)channel;
  return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...) {
  (void)channel;
  (void)format;
}
int Campaign_TestStoryFlag(int flag) {
  (void)flag;
  return 0;
}
int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw,
                 int *ch, int *width, int *height) {
  (void)path;
  (void)w;
  (void)h;
  (void)x;
  (void)y;
  (void)cw;
  (void)ch;
  (void)width;
  (void)height;
  return 0;
}
int CardArt_FromImage(const char *path, unsigned char *record, char *why,
                      size_t why_size) {
  (void)path;
  (void)record;
  (void)why;
  (void)why_size;
  return 0;
}
int CardArt_ThumbnailFromImage(const char *path, unsigned char *record,
                               char *why, size_t why_size) {
  (void)path;
  (void)record;
  (void)why;
  (void)why_size;
  return 0;
}
int CardArt_FieldArtFromImage(const char *path, unsigned char *record,
                              char *why, size_t why_size) {
  (void)path;
  (void)record;
  (void)why;
  (void)why_size;
  return 0;
}
int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why,
                           size_t why_size) {
  (void)path;
  (void)plate;
  (void)why;
  (void)why_size;
  return 0;
}
int CardArt_TitleFromName(const char *name, unsigned char *plate) {
  (void)name;
  (void)plate;
  return 0;
}
int CardLayout_FullBleed(void) { return 0; }
int CardLayout_StyleOf(int card_id, CardLayoutStyle *style) {
  (void)card_id;
  (void)style;
  return 0;
}
void CardLayoutArt_Prewarm(void) {}
int CardNotes_Tag(const char *text, const char *key, char *out, size_t size) {
  (void)text;
  (void)key;
  (void)out;
  (void)size;
  return 0;
}
void Duelists_Build(void) {}
uint32_t Glyphs_Character(int code) { return (uint32_t)code; }
int Glyphs_Code(uint32_t character) {
  return character < 0x80 ? (int)character : -1;
}
uint32_t Glyphs_NextCharacter(const char **text) {
  return (unsigned char)*(*text)++;
}
void Library_UpdateCardUsedFlag(int flag) { (void)flag; }
int Memories_Rand(void) { return 0; }
#ifdef MEMORIES_LOW_MEMORY
/* Without the low region these are macros for malloc and free
 * (pc/guest/low_memory.h): defining them would define malloc itself. */
void *Memories_LowAlloc(size_t size) { return malloc(size); }
void Memories_LowFree(void *ptr) { free(ptr); }
#endif
void Mods_SetCardNotes(const char *(*which)(int id),
                       int (*tag)(int id, const char *key, char *out,
                                  size_t size)) {
  (void)which;
  (void)tag;
}
void Mods_SetCardResolver(int (*resolve)(const char *)) { (void)resolve; }
void Mods_SetOverlapCards(int (*card)(const char *, long),
                          int (*name)(int, char *, size_t),
                          int (*info)(int, int *, int *, int *)) {
  (void)card, (void)name, (void)info;
}
void Mods_SetCardSignature(unsigned signature) { (void)signature; }
void Mods_SetLimitSource(long (*source)(const char *name)) { (void)source; }
int Mods_Setting(const char *id, const char *key, int fallback) {
  (void)id;
  (void)key;
  return fallback;
}
/* The mods' "cards", in load order, as Cards_Build visits them. */
static const char *const mod_cards[][2] = {
    /* The report: a retail monster replaced with no star keeps none; [none,
       X] is the one star X; 0, null and "none" are all none. A copy has its
       own stars, and a copy of a no-star card without "stars" none either. */
    {"a",
     "[{\"replace\": 1, \"stars\": [\"(none)\", \"(none)\"], \"frame_style\": "
     "\"legend\"},"
     " {\"replace\": 2, \"stars\": [0, \"Sun\"]},"
     " {\"replace\": 3, \"stars\": [null, \"none\"]},"
     " {\"copy\": 4, \"id\": \"none\", \"stars\": [0, 0]},"
     " {\"copy\": 1, \"id\": \"of-none\"},"
     /* "effect": 0, as copies wrote it before names: the base's, no note. */
     " {\"copy\": 4, \"id\": \"zero-effect\", \"effect\": 0}]"},
    /* A magic card made a monster: with no "stars", its model's or the Sun
       and the Moon; with [0, 0], none. */
    {"b", "[{\"replace\": 300, \"type\": \"Dragon\"},"
          " {\"replace\": 301, \"type\": \"Dragon\", \"model\": 1},"
          " {\"replace\": 302, \"type\": \"Dragon\", \"stars\": [0, 0]}]"},
    /* A later mod's entry for an earlier mod's no-star card that leaves
       "stars" out does not bring the disc's back. */
    {"c", "[{\"replace\": 1, \"attack\": 2000}]"},
    /* Refused, each with a note, the card keeping what it had; a star left
       out with none for the other is still put first. */
    {"d", "[{\"replace\": 5, \"stars\": \"Mars\"},"
          " {\"replace\": 6, \"stars\": [\"Mars\"]},"
          " {\"replace\": 7, \"stars\": [\"Nowhere\", \"Mars\"]},"
          " {\"replace\": 8, \"stars\": [0, \"Nowhere\"]}]"},
    /* Monsters made a magic, an equip and a trap card: no ATK or DEF, an
       "attack" given said to be left out, and each another kind than on the
       disc, so out of its fusion and equip tables (Cards_KindChanged). */
    {"e", "[{\"replace\": 10, \"type\": \"Magic\"},"
          " {\"replace\": 11, \"type\": \"Equip\", \"attack\": 1500},"
          " {\"replace\": 12, \"type\": \"Trap\", \"defense\": 0}]"},
};

void Mods_VisitCards(void (*visit)(const char *id, const char *directory,
                                   const struct JsonValue *cards,
                                   void *context),
                     void *context) {
  size_t i;
  for (i = 0; i < sizeof(mod_cards) / sizeof(mod_cards[0]); i++) {
    char error[256];
    /* Kept: the cards keep their entries (definitions[]). */
    JsonDocument *document = Json_Parse(mod_cards[i][1], error, sizeof(error));
    if (!document)
      fprintf(stderr, "%s: %s\n", mod_cards[i][0], error);
    assert(document);
    visit(mod_cards[i][0], ".", Json_Root(document), context);
  }
}
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) {
  (void)mod;
  return 0;
}
const char *Mods_Id(int mod) {
  (void)mod;
  return "";
}
const char *Mods_Directory(int mod) {
  (void)mod;
  return ".";
}
const struct JsonValue *Mods_Manifest(int mod) {
  (void)mod;
  return NULL;
}
/* No mod here declares "card_types": build_monster_types
 * (cards.c) still calls this once, visiting nothing. */
void Mods_VisitManifests(void (*visit)(const char *id, const char *directory,
                                       const struct JsonValue *manifest,
                                       void *context),
                         void *context) {
  (void)visit;
  (void)context;
}
/* The custom type icon bank (type_icons.c): this test never asks for a
 * card's type icon, so a no-op stands in for it. */
void TypeIcons_Clear(void) {}
int Paths_Contained(const char *relative) {
  return relative[0] != '/' && !strstr(relative, "..");
}
int Paths_MakeDirs(const char *path) {
  (void)path;
  return -1;
}
int Paths_User(char *out, size_t size, const char *relative) {
  (void)out;
  (void)size;
  (void)relative;
  return -1;
}
void Paths_WriteBegin(void) {}
const char *Paths_WriteError(char *out, size_t size, const char *path) {
  (void)size;
  (void)path;
  return out;
}
int Settings_Get(SettingId id) {
  (void)id;
  return 0;
}
void Starter_Build(void) {}
void Packs_Build(void) {}
void Tower_Build(void) {}
const UiConfig *UiConfig_Load(void) { return NULL; }
unsigned Packs_Signature(void) { return 0; }
void Mods_SetPackSignature(unsigned signature) { (void)signature; }
void Tables_Build(void) {}
long Tables_Limit(const char *name) {
  (void)name;
  return -1;
}
int Tables_StatCapEither(void) { return 9999; }
int Text_Overridden(int id) {
  (void)id;
  return 0;
}
const unsigned char *Text_Resolve(int id, const unsigned char *retail) {
  (void)id;
  return retail;
}
void TextureDump_Written(const void *destination, unsigned bytes) {
  (void)destination;
  (void)bytes;
}
int TexturePack_AddMade(const void *pixels, int words, int rows, int bpp,
                        const void *clut, int clut_entries, const char *file,
                        int x, int y, int w, int h) {
  (void)pixels;
  (void)words;
  (void)rows;
  (void)bpp;
  (void)clut;
  (void)clut_entries;
  (void)file;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return 0;
}
int Language_Current(void) { return 0; }
const char *Language_Code(int which) {
  (void)which;
  return "en-us";
}

#define STATS(type, star1, star2)                                              \
  (((unsigned)(type) << 26) | ((unsigned)(star1) << 22) |                      \
   ((unsigned)(star2) << 18) | 100u)
#define MARS 1
#define SUN 8
#define MOON 9

static void stars_of(int id, int *first, int *second) {
  unsigned stats = (unsigned)gDuel_adwCardStats[id - 1];
  *first = (int)((stats >> 22) & 0xF);
  *second = (int)((stats >> 18) & 0xF);
}

static void expect(int id, int first, int second) {
  int a, b;
  stars_of(id, &a, &b);
  if (a != first || b != second) {
    fprintf(stderr, "card %d: stars %d, %d; expected %d, %d\n", id, a, b, first,
            second);
    assert(0);
  }
}

static void magic_conversions(void) {
  BuildContext context = {0};
  JsonDocument *document;
  char json[256], error[128];
  int id, base;
  /* Real retail effect identities, independent of the cards occupying
     those slots after a mod. Every retail slot can become Raigeki. */
  test_stats[337 - 1] = STATS(CARD_TYPE_MAGIC, 0, 0);
  test_stats[343 - 1] = STATS(CARD_TYPE_MAGIC, 0, 0);
  for (id = 1; id <= CARD_COUNT; id++) {
    snprintf(json, sizeof(json),
             "{\"replace\":%d,\"type\":\"Magic\",\"effect\":337}", id);
    document = Json_Parse(json, error, sizeof(error));
    assert(document);
    add_entry("conversion", ".", id, Json_Root(document), &context);
    assert(Cards_Type(id) == CARD_TYPE_MAGIC);
    assert(Cards_EffectId(id) == 337);
    assert(Cards_AiId(id) == 337);
    assert(Cards_TrapId(id) == 0);
    assert(!((unsigned)gDuel_adwCardStats[id - 1] & 0x3FFFFu));
  }
  /* Copies of each original kind may cross to magic with a matching
     effect. No monster stats survive to confuse the AI's ranking. */
  for (base = 1; base <= 5; base++) {
    int type = base == 1 ? 0 : CARD_TYPE_MAGIC + base - 2;
    gDuel_adwCardStats[base - 1] = test_stats[base - 1] =
        STATS(type, SUN, MOON);
    snprintf(
        json, sizeof(json),
        "{\"copy\":%d,\"id\":\"magic-%d\",\"type\":\"Magic\",\"effect\":337}",
        base, base);
    document = Json_Parse(json, error, sizeof(error));
    assert(document);
    id = gCard_nCount + 1;
    add_entry("conversion", ".", base, Json_Root(document), &context);
    assert(gCard_nCount == id && Cards_Type(id) == CARD_TYPE_MAGIC);
    assert(Cards_EffectId(id) == 337 && Cards_AiId(id) == 337);
    assert(Cards_KindChanged(id) == (type != CARD_TYPE_MAGIC));
    assert(!((unsigned)gDuel_adwCardStats[id - 1] & 0x3FFFFu));
  }
  /* Raigeki itself now plays Sparks; explicit references to Raigeki
     still mean retail Raigeki, for existing cards and added copies. */
  effect_ids[337] = 343;
  assert(Cards_EffectId(337) == 343 && Cards_AiId(337) == 343);
  assert(Cards_EffectId(6) == 337 && Cards_AiId(6) == 337);
  assert(Cards_EffectId(id) == 337 && Cards_AiId(id) == 337);
  gDuel_adwCardStats[337 - 1] = STATS(0, SUN, MOON);
  assert(Cards_AiId(337) == -1);
  assert(Cards_AiId(6) == 337 && Cards_AiId(id) == 337);
}

static void trap_conversions(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i, copy;
  JsonDocument *doc =
      Json_Parse("[{\"replace\":1,\"type\":\"Trap\",\"effect\":681,\"trap_"
                 "threshold\":1234},"
                 "{\"copy\":1,\"id\":\"inherited-trap\"},"
                 "{\"copy\":1,\"id\":\"default-trap\",\"trap_threshold\":null},"
                 "{\"copy\":1,\"id\":\"zero-trap\",\"trap_threshold\":0},"
                 "{\"replace\":2,\"type\":\"Trap\",\"effect\":681,\"trap_"
                 "threshold\":65535},"
                 "{\"replace\":681,\"type\":\"Dragon\"}]",
                 error, sizeof(error));
  assert(doc);
  test_stats[680] = STATS(CARD_TYPE_TRAP, 0, 0);
  copy = gCard_nCount + 1;
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("traps", ".", i, entry, &context);
  assert(Cards_TrapId(1) == 681 && Cards_AiId(1) == 681);
  assert(Cards_TrapId(681) == 0 && Cards_AiId(681) == -1);
  assert(Cards_TrapThreshold(1, -1) == 1234 &&
         Cards_TrapThreshold(copy, -1) == 1234);
  assert(Cards_TrapThreshold(copy + 1, -1) == -1);
  assert(Cards_TrapThreshold(copy + 2, -1) == 0);
  assert(Cards_TrapThreshold(2, -1) == 65535);
  effect_ids[1] = 687;
  assert(Cards_TrapThreshold(1, -1) ==
         -1); /* a reflector never gets an ATK trigger */
}

/* "monster_effects" (monster_effects.h): read into the card, a copy taking
 * its base's unless it has its own, [] taking them away, and what the game
 * cannot do left out with a note. */
static void monster_effect_entries(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  const MonsterEffect *effects;
  int i, copy = gCard_nCount + 1;
  JsonDocument *doc = Json_Parse(
      "[{\"replace\":20,\"monster_effects\":["
      "  {\"when\":\"summon\",\"do\":\"magic\",\"card\":337},"
      "  {\"when\":\"Face "
      "Up\",\"do\":\"boost\",\"target\":\"others\",\"type\":\"Dragon\","
      "   \"attribute\":\"Light\",\"attack\":300,\"defense\":-200},"
      "  "
      "{\"when\":\"combat\",\"do\":\"boost\",\"target\":\"battle\",\"attack\":-"
      "500},"
      "  {\"when\":\"destroyed\",\"do\":\"damage\",\"amount\":800},"
      "  {\"when\":\"draw\",\"do\":\"heal\",\"amount\":100},"
      "  {\"when\":\"flip\",\"do\":\"magic\",\"card\":675},"
      "  {\"when\":\"face_up\",\"do\":\"magic\",\"card\":337},"
      "  "
      "{\"when\":\"combat\",\"do\":\"boost\",\"target\":\"own\",\"attack\":100}"
      ","
      "  "
      "{\"when\":\"destroyed\",\"do\":\"boost\",\"target\":\"self\",\"attack\":"
      "100},"
      "  {\"when\":\"summon\",\"do\":\"heal\",\"amount\":0},"
      "  {\"when\":\"summon\",\"do\":\"boost\"},"
      "  {\"when\":\"later\",\"do\":\"heal\",\"amount\":1},"
      "  {\"when\":\"destroy_opponent\",\"do\":\"heal\",\"amount\":300},"
      "  {\"when\":\"flip\",\"do\":\"destroy\",\"type\":\"Warrior\"},"
      "  {\"when\":\"summon\",\"do\":\"destroy\",\"target\":\"own\"},"
      "  {\"when\":\"summon\",\"do\":\"destroy\",\"target\":\"battle\"}]},"
      "{\"copy\":20,\"id\":\"inherits\"},"
      "{\"copy\":20,\"id\":\"none\",\"monster_effects\":[]},"
      "{\"replace\":20,\"attack\":1000},"
      "{\"replace\":21,\"monster_effects\":{\"when\":\"summon\"}},"
      "{\"replace\":22,\"monster_effects\":[{\"when\":\"draw\",\"do\":\"heal\","
      "\"amount\":1}],"
      " \"frame\":\"Type\"},"
      "{\"replace\":23,\"monster_effects\":[{\"when\":\"draw\",\"do\":\"heal\","
      "\"amount\":1}],"
      " \"frame\":\"Monster\"}]",
      error, sizeof(error));
  assert(doc);
  test_stats[336 - 1] = test_stats[337 - 1] = STATS(CARD_TYPE_MAGIC, 0, 0);
  test_stats[675 - 1] = STATS(CARD_TYPE_RITUAL, 0, 0);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("f", ".", i, entry, &context);
  /* Seven taken; a ritual's effect, magic while face up, a combat boost of
     its side, a destroyed card's own boost, no amount, no boost, an
     unknown "when", a destroy of its own side and one of the monster it
     battles outside a flip left out, each noted. */
  assert(Cards_MonsterEffects(20, &effects) == 7);
  assert(effects[0].when == MONSTER_WHEN_SUMMON &&
         effects[0].action == MONSTER_DO_MAGIC && effects[0].card == 337);
  assert(effects[1].when == MONSTER_WHEN_FACE_UP &&
         effects[1].action == MONSTER_DO_BOOST);
  assert(effects[1].target == MONSTER_TARGET_OTHERS && effects[1].type == 0 &&
         effects[1].attribute == 0);
  assert(effects[1].attack == 300 && effects[1].defense == -200);
  assert(effects[2].when == MONSTER_WHEN_COMBAT &&
         effects[2].target == MONSTER_TARGET_BATTLE);
  assert(effects[3].when == MONSTER_WHEN_DESTROYED &&
         effects[3].action == MONSTER_DO_DAMAGE && effects[3].amount == 800);
  assert(effects[4].when == MONSTER_WHEN_DRAW &&
         effects[4].action == MONSTER_DO_HEAL && effects[4].amount == 100);
  assert(effects[5].when == MONSTER_WHEN_DESTROY_OPPONENT &&
         effects[5].action == MONSTER_DO_HEAL);
  /* A flip's destroy without a target: the monster attacking it. */
  assert(effects[6].when == MONSTER_WHEN_FLIP &&
         effects[6].action == MONSTER_DO_DESTROY &&
         effects[6].target == MONSTER_TARGET_BATTLE && effects[6].type == 3);
  assert(effect_notes == 10); /* the nine left out, and 21's that is no list */
  /* A copy has its base's; [] none; a later replace without the key keeps them.
   */
  assert(Cards_MonsterEffects(copy, &effects) == 7);
  assert(Cards_MonsterEffects(copy + 1, &effects) == 0);
  /* Not a list: noted, the card keeps what it had (none). */
  assert(Cards_MonsterEffects(21, &effects) == 0);
  assert(MonsterEffect_MagicUsable(337) && MonsterEffect_MagicUsable(336) &&
         !MonsterEffect_MagicUsable(675));
  /* Drawn orange, as an effect monster, unless its "frame" says: "Type"
   * its type's, "Monster" gold. A monster without effects, its type's. */
  for (i = 20; i <= 23; i++)
    gDuel_adwCardStats[i - 1] = (int)STATS(0, SUN, MOON);
  assert(Cards_FrameColor(20) == CARD_FRAME_ORANGE);
  assert(Cards_FrameColor(21) == -1);
  assert(Cards_FrameColor(22) == -1);
  assert(Cards_FrameColor(23) == CARD_FRAME_MONSTER);
  gDuel_adwCardStats[20 - 1] = (int)STATS(CARD_TYPE_MAGIC, 0, 0);
  assert(Cards_FrameColor(20) == -1); /* not a monster: its type's */
  assert(!MonsterEffect_MagicUsable(300) && !MonsterEffect_MagicUsable(1));
}

/* "for_each": whose monsters a boost, heal or damage counts, "all" when
 * unsaid; not on a magic or destroy, nor whose it does not know. */
static void monster_effect_for_each(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  const MonsterEffect *effects;
  int i, before = effect_notes;
  JsonDocument *doc = Json_Parse(
      "[{\"replace\":24,\"monster_effects\":["
      "  {\"when\":\"face_up\",\"do\":\"boost\",\"attack\":300,\"defense\":300,"
      "   \"for_each\":{\"whose\":\"own\",\"type\":\"Dragon\"}},"
      "  "
      "{\"when\":\"summon\",\"do\":\"heal\",\"amount\":200,\"for_each\":{"
      "\"attribute\":\"Light\"}},"
      "  "
      "{\"when\":\"combat\",\"do\":\"damage\",\"amount\":100,\"for_each\":{"
      "\"whose\":\"Opponent\"}},"
      "  "
      "{\"when\":\"draw\",\"do\":\"boost\",\"target\":\"own\",\"attack\":100},"
      "  {\"when\":\"summon\",\"do\":\"magic\",\"card\":337,\"for_each\":{}},"
      "  {\"when\":\"summon\",\"do\":\"destroy\",\"for_each\":{}},"
      "  "
      "{\"when\":\"summon\",\"do\":\"heal\",\"amount\":1,\"for_each\":{"
      "\"whose\":\"mine\"}},"
      "  "
      "{\"when\":\"summon\",\"do\":\"heal\",\"amount\":1,\"for_each\":"
      "\"Dragon\"},"
      "  "
      "{\"when\":\"summon\",\"do\":\"heal\",\"amount\":1,\"for_each\":{"
      "\"type\":\"Trap\"}}]}]",
      error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("f", ".", i, entry, &context);
  assert(Cards_MonsterEffects(24, &effects) == 4);
  assert(effects[0].each == MONSTER_EACH_OWN && effects[0].each_type == 0 &&
         effects[0].each_attribute == -1);
  assert(effects[0].type == -1 && effects[0].attack == 300 &&
         effects[0].defense == 300);
  assert(effects[1].each == MONSTER_EACH_ALL && effects[1].each_type == -1 &&
         effects[1].each_attribute == 0);
  assert(effects[2].each == MONSTER_EACH_OPPONENT &&
         effects[2].action == MONSTER_DO_DAMAGE);
  assert(effects[3].each == MONSTER_EACH_NONE && effects[3].each_type == -1);
  assert(effect_notes ==
         before + 5); /* magic, destroy, "mine", no object, a Trap counted */
  assert(MonsterEffect_EachAllowed(MONSTER_DO_BOOST) &&
         MonsterEffect_EachAllowed(MONSTER_DO_HEAL) &&
         MonsterEffect_EachAllowed(MONSTER_DO_DAMAGE));
  assert(!MonsterEffect_EachAllowed(MONSTER_DO_MAGIC) &&
         !MonsterEffect_EachAllowed(MONSTER_DO_DESTROY));
}

/* API 19: battle immunity and conditions system */
static void monster_effect_conditions_and_battle_immunity(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  const MonsterEffect *effects;
  int i, before = effect_notes;
  JsonDocument *doc =
      Json_Parse("[{\"replace\":26,\"monster_effects\":["
                 "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
                 "   \"conditions\":["
                 "     {\"type\":\"named_card\",\"card\":1,\"whose\":\"own\"},"
                 "     {\"type\":\"fewer_monsters\"},"
                 "     "
                 "{\"type\":\"stat_threshold\",\"stat\":\"def\",\"comparison\":"
                 "\"<=\",\"value\":2000},"
                 "     {\"type\":\"lowest_atk\"},"
                 "     {\"type\":\"highest_atk\"}],"
                 "   \"condition_mode\":\"any\"},"
                 "  {\"when\":\"face_up\",\"do\":\"boost\",\"attack\":500,"
                 "   "
                 "\"condition\":{\"type\":\"stat_threshold\",\"stat\":\"atk\","
                 "\"op\":\">\",\"value\":1500}},"
                 "  {\"when\":\"combat\",\"do\":\"battle_immunity\"},"
                 "  "
                 "{\"when\":\"face_up\",\"do\":\"battle_immunity\","
                 "\"conditions\":[{\"type\":\"unknown_cond\"}]}]}]",
                 error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("f", ".", i, entry, &context);
  assert(Cards_MonsterEffects(26, &effects) == 2);
  /* Effect 0: battle immunity with 4 conditions (max MONSTER_CONDITIONS_MAX is
   * 4, 5th is truncated) */
  assert(effects[0].when == MONSTER_WHEN_FACE_UP);
  assert(effects[0].action == MONSTER_DO_BATTLE_IMMUNITY);
  assert(effects[0].target == MONSTER_TARGET_SELF);
  assert(effects[0].cond_mode == MONSTER_COND_MODE_ANY);
  assert(effects[0].cond_count == 4);
  assert(effects[0].conditions[0].type == MONSTER_COND_NAMED_CARD);
  assert(effects[0].conditions[0].card == 1);
  assert(effects[0].conditions[0].whose == MONSTER_EACH_OWN);
  assert(effects[0].conditions[1].type == MONSTER_COND_FEWER_MONSTERS);
  assert(effects[0].conditions[2].type == MONSTER_COND_STAT_THRESHOLD);
  assert(effects[0].conditions[2].stat == MONSTER_STAT_DEF);
  assert(effects[0].conditions[2].op == MONSTER_OP_LE);
  assert(effects[0].conditions[2].value == 2000);
  assert(effects[0].conditions[3].type == MONSTER_COND_LOWEST_ATK);
  /* Effect 1: boost with single condition */
  assert(effects[1].when == MONSTER_WHEN_FACE_UP);
  assert(effects[1].action == MONSTER_DO_BOOST);
  assert(effects[1].attack == 500);
  assert(effects[1].cond_mode == MONSTER_COND_MODE_ALL);
  assert(effects[1].cond_count == 1);
  assert(effects[1].conditions[0].type == MONSTER_COND_STAT_THRESHOLD);
  assert(effects[1].conditions[0].stat == MONSTER_STAT_ATK);
  assert(effects[1].conditions[0].op == MONSTER_OP_GT);
  assert(effects[1].conditions[0].value == 1500);
  /* combat battle_immunity rejected, unknown condition rejected, max conditions
   * note */
  assert(effect_notes > before);

  /* Attack counts are battle immunity modifiers, not conditions. */
  JsonDocument *doc2 = Json_Parse(
      "[{\"replace\":27,\"monster_effects\":["
      "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
      "   \"modifier\":\"destroyed_after_2_attacks\",\"reset\":\"summon\"},"
      "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
      "   \"modifier\":\"destroyed_after_3_attacks\",\"reset\":\"turn\"},"
      "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
      "   \"modifier\":\"first_attacked\",\"reset\":\"duel\"},"
      "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
      "   \"conditions\":[{\"type\":\"first_attacked\"}]}]}]",
      error, sizeof(error));
  assert(doc2);
  before = effect_notes;
  for (i = 0, entry = Json_At(Json_Root(doc2), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("f", ".", i, entry, &context);
  assert(Cards_MonsterEffects(27, &effects) == 3);
  assert(effects[0].cond_count == 0);
  assert(effects[0].battle_modifier == MONSTER_BATTLE_MOD_AFTER_2_ATTACKS);
  assert(effects[0].battle_reset == MONSTER_RESET_SUMMON);
  assert(effects[1].battle_modifier == MONSTER_BATTLE_MOD_AFTER_3_ATTACKS);
  assert(effects[1].battle_reset == MONSTER_RESET_TURN);
  assert(effects[2].battle_modifier == MONSTER_BATTLE_MOD_FIRST_ATTACKED);
  assert(effects[2].battle_reset == MONSTER_RESET_DUEL);
  /* the old condition spelling is noted and left out */
  assert(effect_notes > before);
  /* Test fewer_monsters and more_monsters conditions */
  JsonDocument *doc3 = Json_Parse(
      "[{\"replace\":29,\"monster_effects\":["
      "  {\"when\":\"face_up\",\"do\":\"battle_immunity\","
      "   \"conditions\":["
      "     {\"type\":\"fewer_monsters\"},"
      "     {\"type\":\"more_monsters\"},"
      "     {\"type\":\"fewer_monsters_than_opponent\"},"
      "     {\"type\":\"more_monsters_than_opponent\"}]}]}]",
      error, sizeof(error));
  assert(doc3);
  for (i = 0, entry = Json_At(Json_Root(doc3), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("f", ".", i, entry, &context);
  assert(Cards_MonsterEffects(29, &effects) == 1);
  assert(effects[0].cond_count == 4);
  assert(effects[0].conditions[0].type == MONSTER_COND_FEWER_MONSTERS);
  assert(effects[0].conditions[1].type == MONSTER_COND_MORE_MONSTERS);
  assert(effects[0].conditions[2].type == MONSTER_COND_FEWER_MONSTERS);
  assert(effects[0].conditions[3].type == MONSTER_COND_MORE_MONSTERS);
}

/* A monster-effect's "type" (monster_effects.c filter()) reads Json_Number's
 * long before narrowing to int: 2^32 must be rejected as out of range, not
 * silently narrowed first and read back as the valid type 0. */
static void monster_effect_type_overflow(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  const MonsterEffect *effects;
  int i, before = effect_notes;
  JsonDocument *doc =
      Json_Parse("[{\"replace\":25,\"monster_effects\":["
                 "  "
                 "{\"when\":\"summon\",\"do\":\"boost\",\"target\":\"own\","
                 "\"attack\":500,\"type\":4294967296}]}]",
                 error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("i", ".", i, entry, &context);
  assert(Cards_MonsterEffects(25, &effects) == 0);
  assert(effect_notes == before + 1);
}

/* Cards_Reference reads Json_Number's long before Cards_Valid's int: a
 * reference far past int range must not wrap into a valid card id (card 1,
 * from 2^32 + 1 narrowed to 1 on a typical 64-bit build). */
static void reference_overflow(void) {
  char error[128];
  JsonDocument *doc = Json_Parse("4294967297", error, sizeof(error));
  assert(doc);
  assert(Cards_Reference(Json_Root(doc)) == -1);
  Json_Free(doc);
}

/* add_entry's own "copy"/"replace" id resolution has the same int-narrowing
 * shape as Cards_Reference, just not through it: an oversized id must be
 * refused, not silently narrowed into card 1 and applied to it. */
static void copy_id_overflow(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i, before_count = gCard_nCount;
  unsigned before_stats = (unsigned)gDuel_adwCardStats[0];
  JsonDocument *doc = Json_Parse(
      "[{\"copy\":4294967297,\"id\":\"overflow-copy\",\"attack\":3000},"
      "{\"replace\":4294967297,\"attack\":3000}]",
      error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("i", ".", i, entry, &context);
  assert(gCard_nCount == before_count); /* no copy added */
  assert((unsigned)gDuel_adwCardStats[0] ==
         before_stats); /* card 1 (2^32+1 narrowed) untouched */
}

/* "attack"/"defense" (notes/modding.md): a value the field's own comment
 * says is always said, never quietly changed. Before the fix, 2^32 (past
 * int range) narrowed to 0 first, which is neither "> 0x1FF*10" nor "not a
 * multiple of 10" -- so it skipped both notes and was written as a silent
 * 0, the one thing the comment says this field never does. */
static void stat_overflow_attack_defense(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i;
  JsonDocument *doc;
  gDuel_adwCardStats[25] = test_stats[25] =
      STATS(0, SUN, MOON); /* magic_conversions made it Magic */
  doc = Json_Parse("[{\"replace\":26,\"attack\":4294967296}]", error,
                   sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("j", ".", i, entry, &context);
  assert(((unsigned)gDuel_adwCardStats[25] & 0x1FF) ==
         0x1FF); /* clamped to the max, not zeroed */
}

/* choice() (shared by "type", "frame" and "frame_color"): 2^32 + 5 must not
 * narrow into the valid type 5 (Beast) and get applied -- whatever type the
 * card already had (Magic, by this point: magic_conversions touched every
 * card) must still be what it has. */
static void type_overflow_via_choice(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i;
  unsigned before = (unsigned)gDuel_adwCardStats[26];
  JsonDocument *doc = Json_Parse("[{\"replace\":27,\"type\":4294967301}]",
                                 error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("j", ".", i, entry, &context);
  assert((unsigned)gDuel_adwCardStats[26] == before);
}

/* "count" and "level" have the same shape: each oversized value must come
 * back as narrow()'s own -1 (no copy made / the base's level kept), not an
 * arbitrary wrapped-around value that happens to still be in range. */
static void count_and_level_overflow(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i, before_count = gCard_nCount;
  unsigned char before_level = (unsigned char)(gDuel_abCardLevelAttr[28] & 0xF);
  JsonDocument *doc =
      Json_Parse("[{\"copy\":1,\"id\":\"count-overflow\",\"count\":4294967296},"
                 "{\"replace\":28,\"level\":4294967301}]",
                 error, sizeof(error));
  assert(doc);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("j", ".", i, entry, &context);
  assert(gCard_nCount ==
         before_count); /* "count" narrowed to -1, then clamped to 0 copies */
  assert((gDuel_abCardLevelAttr[28] & 0xF) == before_level);
}

/* Copies of magic cards made monsters (as a replace may be): a monster's
 * type, ATK and DEF, stars as a replaced card gets them, out of the magic
 * card's tables; made a Trap without a trap's effect, still refused. */
static void copies_made_monsters(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  int i, first = gCard_nCount + 1;
  JsonDocument *doc = Json_Parse(
      "[{\"copy\":500,\"id\":\"magic-dragon\",\"type\":\"Dragon\",\"attack\":"
      "1500,\"defense\":1200,"
      "  "
      "\"monster_effects\":[{\"when\":\"summon\",\"do\":\"heal\",\"amount\":"
      "500}]},"
      "{\"copy\":500,\"id\":\"with-model\",\"type\":\"fiend\",\"model\":400},"
      "{\"copy\":500,\"id\":\"no-stars\",\"type\":\"Dragon\",\"stars\":[0,0]},"
      "{\"copy\":500,\"id\":\"bad-model\",\"type\":\"Dragon\",\"model\":"
      "\"Nowhere\"},"
      "{\"copy\":500,\"id\":\"magic-trap\",\"type\":\"Trap\"}]",
      error, sizeof(error));
  const MonsterEffect *effects;
  assert(doc);
  test_stats[400 - 1] = STATS(0, MOON, MARS);
  gDuel_adwCardStats[500 - 1] =
      (int)(test_stats[500 - 1] =
                STATS(CARD_TYPE_MAGIC, 0, 0)); /* a clean magic card */
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("g", ".", i, entry, &context);
  assert(gCard_nCount == first + 4);
  assert(Cards_Type(first) == 0 && Cards_Type(first + 1) == 7 &&
         Cards_Type(first + 2) == 0);
  assert(((unsigned)gDuel_adwCardStats[first - 1] & 0x1FF) == 150);
  assert((((unsigned)gDuel_adwCardStats[first - 1] >> 9) & 0x1FF) == 120);
  expect(first, SUN, MOON);      /* no model: the Sun and the Moon */
  expect(first + 1, MOON, MARS); /* its model's */
  expect(first + 2, 0, 0);
  assert(Cards_ModelId(first) == 500 && Cards_ModelId(first + 1) == 400 &&
         !Cards_HasModel(first));
  assert(Cards_KindChanged(first) && Cards_AiId(first) == -1 &&
         Cards_TrapId(first) == 0);
  assert(Cards_MonsterEffects(first, &effects) == 1 &&
         effects[0].action == MONSTER_DO_HEAL);
  /* A "model" naming no monster, and the Trap without a trap's effect: noted.
   */
  assert(Cards_ModelId(first + 3) == 500);
  assert(Cards_Type(first + 4) == CARD_TYPE_MAGIC);
  assert(notes[6] == 2);
}

/* "card_effects", "card_effects_mode" and "ai_effect" (notes/more-cards.md,
 * "Spell and trap effects"): read with no "when", their default targets, the
 * targets a card that is no monster on the field cannot have, what a copy
 * inherits, and all three refused on a card that is not a Magic or Trap. */
static void card_effect_entries(void) {
  BuildContext context = {0};
  char error[128];
  const JsonValue *entry;
  const MonsterEffect *effects;
  int i, copy = gCard_nCount + 1;
  JsonDocument *doc = Json_Parse(
      "[{\"replace\":600,\"type\":\"Magic\",\"card_effects_mode\":\"replace\","
      "\"ai_effect\":337,"
      "  "
      "\"card_effects\":[{\"do\":\"damage\",\"amount\":800},{\"do\":\"heal\","
      "\"amount\":500},"
      "                   "
      "{\"do\":\"destroy\"},{\"do\":\"boost\",\"target\":\"own\",\"attack\":"
      "200}]},"
      "{\"copy\":600,\"id\":\"inherits\"},"
      "{\"copy\":600,\"id\":\"own-list\",\"card_effects\":[{\"do\":\"heal\","
      "\"amount\":100}]},"
      "{\"copy\":600,\"id\":\"mode-only\",\"card_effects_mode\":\"add\"},"
      "{\"copy\":600,\"id\":\"made-monster\",\"type\":\"Dragon\"},"
      "{\"replace\":601,\"type\":\"Magic\",\"card_effects_mode\":\"replace\","
      "  \"card_effects\":[{\"do\":\"heal\",\"amount\":100}]},"
      "{\"replace\":602,\"type\":\"Magic\",\"card_effects\":["
      "  "
      "{\"do\":\"heal\",\"amount\":1,\"target\":\"all\"},{\"do\":\"damage\","
      "\"amount\":1,\"target\":\"all\"},"
      "  "
      "{\"do\":\"boost\",\"target\":\"self\",\"attack\":1},{\"do\":\"boost\","
      "\"target\":\"others\",\"attack\":1},"
      "  "
      "{\"do\":\"destroy\",\"target\":\"battle\"},{\"do\":\"heal\",\"amount\":"
      "1,\"when\":\"flip\"},"
      "  "
      "{\"do\":\"destroy\",\"target\":\"all\"},{\"do\":\"boost\",\"target\":"
      "\"all\",\"attack\":100}]},"
      "{\"replace\":603,\"type\":\"Dragon\",\"card_effects_mode\":\"replace\"},"
      "{\"replace\":604,\"type\":\"Dragon\",\"ai_effect\":337,"
      "  \"card_effects\":[{\"do\":\"heal\",\"amount\":1}]},"
      "{\"replace\":605,\"type\":\"Magic\",\"card_effects_mode\":\"swap\"},"
      "{\"replace\":606,\"type\":\"Magic\",\"card_effects_mode\":\"replace\","
      "\"card_effects\":[],"
      "  \"ai_effect\":681}]",
      error, sizeof(error));
  assert(doc);
  test_stats[337 - 1] = STATS(CARD_TYPE_MAGIC, 0, 0);
  test_stats[681 - 1] = STATS(CARD_TYPE_TRAP, 0, 0);
  for (i = 0, entry = Json_At(Json_Root(doc), 0); entry;
       i++, entry = Json_Next(entry))
    add_entry("h", ".", i, entry, &context);
  assert(gCard_nCount == copy + 3);

  /* No "when": the card itself is the occasion. Damage and destroy go to
   * the opponent, the rest to its owner unless a target is given. */
  assert(Cards_CardEffects(600, &effects) == 4 &&
         Cards_CardEffectsReplace(600));
  for (i = 0; i < 4; i++)
    assert(effects[i].when == MONSTER_WHEN_SUMMON);
  assert(effects[0].action == MONSTER_DO_DAMAGE && effects[0].amount == 800 &&
         effects[0].target == MONSTER_TARGET_OPPONENT);
  assert(effects[1].action == MONSTER_DO_HEAL &&
         effects[1].target == MONSTER_TARGET_OWN);
  assert(effects[2].action == MONSTER_DO_DESTROY &&
         effects[2].target == MONSTER_TARGET_OPPONENT);
  assert(effects[3].action == MONSTER_DO_BOOST &&
         effects[3].target == MONSTER_TARGET_OWN && effects[3].attack == 200);
  assert(!Cards_MonsterEffects(600, &effects));
  /* A replacement reads as its "ai_effect" only. */
  assert(Cards_AiId(600) == 337);

  /* A copy inherits the list, the mode and the analogue; its own list
   * starts over in "add"; a mode alone changes only the mode. */
  assert(Cards_CardEffects(copy, &effects) == 4 &&
         Cards_CardEffectsReplace(copy) && Cards_AiId(copy) == 337);
  assert(Cards_CardEffects(copy + 1, &effects) == 1 &&
         effects[0].amount == 100 && !Cards_CardEffectsReplace(copy + 1));
  assert(Cards_CardEffects(copy + 2, &effects) == 4 &&
         !Cards_CardEffectsReplace(copy + 2));
  /* Made a monster, it inherits none of them, and its AI identity is not
   * forced to "no card" by its base's replacement. */
  assert(Cards_Type(copy + 3) == 0 && !Cards_CardEffects(copy + 3, &effects) &&
         !Cards_CardEffectsReplace(copy + 3));

  /* A replacement without "ai_effect": the CPU takes it for no retail card. */
  assert(Cards_CardEffects(601, &effects) == 1 &&
         Cards_CardEffectsReplace(601) && Cards_AiId(601) == -1);

  /* Not a monster on the field: no self, others or battle, nor an LP
   * change to "all"; "when" is not read. A destroy or boost of all is. */
  assert(Cards_CardEffects(602, &effects) == 3 &&
         !Cards_CardEffectsReplace(602));
  assert(effects[0].action == MONSTER_DO_HEAL &&
         effects[0].when == MONSTER_WHEN_SUMMON);
  assert(effects[1].action == MONSTER_DO_DESTROY &&
         effects[1].target == MONSTER_TARGET_ALL);
  assert(effects[2].action == MONSTER_DO_BOOST &&
         effects[2].target == MONSTER_TARGET_ALL);

  /* Refused on a monster: no replacement, so its AI identity is its own. */
  assert(Cards_Type(603) == 0 && !Cards_CardEffectsReplace(603) &&
         !Cards_CardEffects(603, &effects));
  assert(Cards_Type(604) == 0 && !Cards_CardEffectsReplace(604) &&
         !Cards_CardEffects(604, &effects));
  assert(!ai_effect_ids[603] && !ai_effect_ids[604]);

  /* An unknown mode, and an analogue of another type: refused. */
  assert(!Cards_CardEffectsReplace(605));
  assert(Cards_CardEffectsReplace(606) && Cards_AiId(606) == -1);

  /* 602's five targets; 603 and 604 on a monster; 605's mode; 606's analogue.
   */
  assert(notes[7] == 9);
}

int main(void) {
  int id;
  /* The disc, as far as these checks go: 1-3 monsters with two stars,
     300-302 magic cards (no stars), the rest plain Dragons. */
  for (id = 1; id <= CARD_COUNT; id++)
    test_stats[id - 1] = STATS(0, SUN, MOON);
  test_stats[0] = STATS(0, SUN, MARS);  /* Blue-eyes White Dragon */
  test_stats[1] = STATS(6, SUN, 2);     /* Mystical Elf */
  test_stats[2] = STATS(1, MOON, MARS); /* Hitotsu-me Giant */
  for (id = 300; id <= 302; id++)
    test_stats[id - 1] = STATS(CARD_TYPE_MAGIC, 0, 0);
  Stars_Clear();
  assert(!Stars_NoStarUsed());

  Cards_Build();
  assert(Stars_NoStarUsed());

  expect(1, 0, 0);
  expect(2, SUN, 0);
  expect(3, 0, 0);
  assert(gCard_nCount == CARD_COUNT + 3);
  expect(CARD_COUNT + 1, 0, 0);
  expect(CARD_COUNT + 2, 0, 0);
  expect(4, SUN, MOON); /* a copy's base is left alone */
  assert(Cards_EffectId(CARD_COUNT + 3) == 4);
  /* Named styles are card data: a copy inherits one, and a later replace
     without frame_style retains it. The active layout resolves "legend". */
  assert(!strcmp(Cards_FrameStyle(1), "legend"));
  assert(!strcmp(Cards_FrameStyle(CARD_COUNT + 2), "legend"));
  assert(notes[0] == 0);

  expect(300, SUN, MOON);
  expect(301, SUN, MARS);
  expect(302, 0, 0);
  assert(notes[1] == 0);

  /* "c" set the attack of 1, and only that. */
  assert(((unsigned)gDuel_adwCardStats[0] & 0x1FF) == 200);

  assert(notes[3] == 4);
  expect(5, SUN, MOON);
  expect(6, SUN, MOON);
  expect(7, SUN, MARS); /* the star it names, the other kept */
  expect(8, MOON, 0);

  for (id = 10; id <= 12; id++)
    assert(!((unsigned)gDuel_adwCardStats[id - 1] & 0x3FFFF));
  assert(notes[4] == 1);
  assert(Cards_KindChanged(10) && Cards_KindChanged(11) &&
         Cards_KindChanged(12));
  assert(Cards_KindChanged(300) &&
         Cards_KindChanged(301)); /* magic cards made Dragons */
  assert(!Cards_KindChanged(1) && !Cards_KindChanged(4) &&
         !Cards_KindChanged(CARD_COUNT + 1));
  /* The opponent's scripts find none of them by their disc numbers. */
  assert(Cards_AiId(10) == -1 && Cards_AiId(11) == -1 && Cards_AiId(300) == -1);
  assert(Cards_AiId(1) == 1 && Cards_AiId(4) == 4);
  assert(((unsigned)gDuel_adwCardStats[3] & 0x1FF) ==
         100); /* a monster keeps its own */

  magic_conversions();
  trap_conversions();
  monster_effect_entries();
  monster_effect_for_each();
  monster_effect_conditions_and_battle_immunity();
  if (LONG_MAX > INT_MAX)
    monster_effect_type_overflow();
  if (LONG_MAX > INT_MAX)
    reference_overflow();
  if (LONG_MAX > INT_MAX)
    copy_id_overflow();
  if (LONG_MAX > INT_MAX)
    stat_overflow_attack_defense();
  if (LONG_MAX > INT_MAX)
    type_overflow_via_choice();
  if (LONG_MAX > INT_MAX)
    count_and_level_overflow();
  copies_made_monsters();
  card_effect_entries();
  puts("cards stars, magic and trap conversions: ok");
  return 0;
}
