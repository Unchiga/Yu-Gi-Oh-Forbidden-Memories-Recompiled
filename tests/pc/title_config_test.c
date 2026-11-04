/* The mods' "title" key as read (src/pc/platform/title_config.c): real
 * manifests through the real JSON reader. What is checked without a screen:
 * the retail defaults, each key landing where title_screen.c looks for it,
 * a later mod winning field by field while text lines add up, the entries'
 * layout around hidden ones, the pictures' files and sizes, and the notes a
 * mistake raises. */
#include "../../src/pc/platform/title_config.c"
#include <stdarg.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            exit(1);                                                            \
        }                                                                       \
    } while (0)

static int notes;
static char note[512];
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    (void)id;
    notes++;
    va_start(arguments, format);
    vsnprintf(note, sizeof(note), format, arguments);
    va_end(arguments);
}

/* paths.c's rule, as far as these tests need it. */
int Paths_Contained(const char *relative)
{
    return relative && *relative && *relative != '/' && !strstr(relative, "..") && !strchr(relative, '\\');
}

/* The tower (pc/tower/tower.h): the mod whose tower the run has, if any. */
static const char *tower_mod, *tower_label = "TOWER";
const char *Tower_Mod(void) { return tower_mod; }
const char *Tower_Label(void) { return tower_label; }

/* TitleConfig_Load's mod list: the tests read manifests themselves. */
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { (void)index; return 0; }
int Mods_Active(int mod) { (void)mod; return 0; }
const char *Mods_Id(int mod) { (void)mod; return "test"; }
const char *Mods_Directory(int mod) { (void)mod; return "/mods/test"; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }

static JsonDocument *documents[8];
static int document_count;

static void add_as(const char *mod, const char *text)
{
    char error[256];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    CHECK(document != NULL);
    CHECK(document_count < 8);
    documents[document_count++] = document;
    TitleConfig_Read(mod, "/mods/test", Json_Root(document));
}

static void add(const char *text)
{
    char error[256];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    CHECK(document != NULL);
    CHECK(document_count < 8);
    documents[document_count++] = document;
    TitleConfig_Read("test", "/mods/test", Json_Root(document));
}

static const TitleConfig *one(const char *text)
{
    while (document_count) Json_Free(documents[--document_count]);
    TitleConfig_Reset();
    notes = 0;
    note[0] = 0;
    add(text);
    TitleConfig_Finish();
    return TitleConfig_Get();
}

static void retail(void)
{
    const TitleConfig *config = one("{\"id\": \"x\"}");
    int i;
    CHECK(notes == 0);
    CHECK(config->song == 0 && !config->skip_movie && config->press_start && config->idle_frames == -1);
    CHECK(config->background[0].picture && config->background[0].shade && config->dim == 0x80 && !config->fade && config->background[0].color == -1);
    CHECK(config->background[0].tint == 0xFFFFFF && config->lines == 0);
    for (i = 0; i < TITLE_LAYERS; i++) CHECK(!config->layers[i].hidden && config->layers[i].tint == 0xFFFFFF);
    /* frontend.c's own places: 50 + 32i, then 42 + 32(i - 5). */
    for (i = 0; i < TITLE_ENTRIES; i++) {
        CHECK(!config->items[i].hidden && config->items[i].x == 0);
        CHECK(config->items[i].y == (i < 5 ? 50 + i * 32 : 42 + (i - 5) * 32));
    }
}

static void keys(void)
{
    const TitleConfig *config = one(
        "{\"title\": {\"music\": \"0x010\", \"skip_intro\": true, \"press_start\": false, \"idle_seconds\": 3,"
        " \"background\": {\"picture\": false, \"shade\": false, \"tint\": \"#9070FF\", \"color\": \"102040\","
        "                  \"dim\": 64, \"fade\": 90},"
        " \"logo\": {\"x\": -10, \"y\": 4}, \"copyright\": {\"hide\": true}, \"prompt\": {\"tint\": 16769088},"
        " \"entries\": {\"options\": {\"x\": 80, \"tint\": \"#FF8080\"}, \"9\": {\"y\": 7}},"
        " \"text\": [{\"text\": \"v1\", \"x\": 316, \"y\": 232, \"align\": \"right\", \"color\": \"#FFD000\","
        "            \"size\": 2, \"show\": \"menu\"}, {\"text\": \"hi\"}]}}");
    CHECK(notes == 0);
    CHECK(config->song == 0x10 && config->skip_movie && !config->press_start && config->idle_frames == 180);
    CHECK(!config->background[0].picture && !config->background[0].shade && config->background[0].tint == 0x9070FF && config->background[0].color == 0x102040);
    CHECK(config->dim == 64 && config->fade == 60);
    CHECK(config->layers[0].x == -10 && config->layers[0].y == 4 && config->layers[1].hidden);
    CHECK(config->layers[2].tint == 0xFFE040);
    CHECK(config->items[4].x == 80 && config->items[4].tint == 0xFF8080);
    CHECK(config->items[9].set_y && config->items[9].y == 7);
    CHECK(config->lines == 2);
    CHECK(!strcmp(config->line[0].text, "v1") && config->line[0].x == 316 && config->line[0].y == 232);
    CHECK(config->line[0].align == TITLE_ALIGN_RIGHT && config->line[0].color == 0xFFD000);
    CHECK(config->line[0].size == 2 && config->line[0].show == TITLE_SHOW_MENU);
    /* A line's defaults: centred at the bottom, white, size 1, always. */
    CHECK(config->line[1].x == 160 && config->line[1].y == 220 && config->line[1].align == TITLE_ALIGN_CENTRE);
    CHECK(config->line[1].color == 0xFFFFFF && config->line[1].size == 1 && config->line[1].show == TITLE_SHOW_ALWAYS);
}

static void layout(void)
{
    /* Two of the first menu's five hidden: the other three close up around
     * the retail middle (y 114), 32 apart; the hidden ones go off screen. */
    const TitleConfig *config = one("{\"title\": {\"entries\": {\"duel\": {\"hide\": true}, \"trade\": {\"hide\": true}}}}");
    CHECK(config->items[0].y == 82 && config->items[1].y == 114 && config->items[4].y == 146);
    CHECK(config->items[2].y == TITLE_PARKED_Y && config->items[3].y == TITLE_PARKED_Y);
    CHECK(config->items[5].y == 42);
    /* Wider apart, one "y" of the mod's own left alone. */
    config = one("{\"title\": {\"spacing\": 40, \"entries\": {\"options\": {\"y\": 200}}}}");
    CHECK(config->items[0].y == 34 && config->items[1].y == 74 && config->items[4].y == 200);
    /* Every entry of a menu hidden: none is. */
    config = one("{\"title\": {\"entries\": {\"new_game\": {\"hide\": true}, \"load\": {\"hide\": true},"
                 " \"duel\": {\"hide\": true}, \"trade\": {\"hide\": true}, \"options\": {\"hide\": true},"
                 " \"save\": {\"hide\": true}}}}");
    CHECK(!config->items[0].hidden && !config->items[4].hidden && config->items[0].y == 50);
    CHECK(config->items[10].hidden && config->items[10].y == TITLE_PARKED_Y);
}

static void pictures(void)
{
    const TitleConfig *config = one("{\"title\": {\"background\": {\"image\": \"art/bg.png\"},"
                                    " \"logo\": {\"image\": \"logo.png\", \"width\": 300},"
                                    " \"prompt\": {\"image\": \"press.png\", \"width\": 400, \"height\": -2}}}");
    CHECK(notes == 0);
    CHECK(!strcmp(config->background[0].image.file, "/mods/test/art/bg.png") && !strcmp(config->background[0].image.mod, "test"));
    CHECK(!strcmp(config->layers[0].image.file, "/mods/test/logo.png") && config->layers[0].image.width == 300);
    CHECK(config->layers[0].image.height == 0 && !config->layers[1].image.file[0]);
    /* Out of range: worked out from the PNG instead. */
    CHECK(config->layers[2].image.width == 0 && config->layers[2].image.height == 0);
    config = one("{\"title\": {\"logo\": {\"image\": \"../other/logo.png\"}}}");
    CHECK(notes == 1 && strstr(note, "outside the mod") && !config->layers[0].image.file[0]);
    /* A later mod's "" puts the game's own picture back. */
    one("{\"title\": {\"logo\": {\"image\": \"logo.png\"}}}");
    add("{\"title\": {\"logo\": {\"image\": \"\"}}}");
    CHECK(!TitleConfig_Get()->layers[0].image.file[0]);
}

static void mods_add_up(void)
{
    const TitleConfig *config;
    one("{\"title\": {\"music\": 5, \"background\": {\"tint\": \"#FF0000\"}, \"text\": [{\"text\": \"a\"}]}}");
    add("{\"title\": {\"background\": {\"dim\": 0}, \"text\": [{\"text\": \"b\"}]}}");
    TitleConfig_Finish();
    config = TitleConfig_Get();
    CHECK(config->song == 5 && config->background[0].tint == 0xFF0000 && config->dim == 0);
    CHECK(config->lines == 2 && !strcmp(config->line[1].text, "b"));
}

static void mistakes(void)
{
    const TitleConfig *config = one("{\"title\": 3}");
    CHECK(notes == 1 && strstr(note, "is an object"));
    config = one("{\"title\": {\"musik\": 1}}");
    CHECK(notes == 1 && strstr(note, "unknown key \"musik\""));
    config = one("{\"title\": {\"music\": 4096}}");
    CHECK(notes == 1 && config->song == 0);
    config = one("{\"title\": {\"background\": {\"tint\": \"#12345\"}}}");
    CHECK(notes == 1 && strstr(note, "color") && config->background[0].tint == 0xFFFFFF);
    config = one("{\"title\": {\"entries\": {\"quit\": {\"hide\": true}}}}");
    CHECK(notes == 1 && strstr(note, "no entry \"quit\""));
    config = one("{\"title\": {\"entries\": {\"11\": {\"hide\": true}}}}");
    CHECK(notes == 1);
    config = one("{\"title\": {\"text\": [{\"x\": 3}]}}");
    CHECK(notes == 1 && config->lines == 0);
    config = one("{\"title\": {\"idle_seconds\": -1}}");
    CHECK(notes == 1 && config->idle_frames == -1);
    config = one("{\"title\": {\"background\": {\"dim\": 500}}}");
    CHECK(notes == 0 && config->dim == 0x80);
}

static void buttons(void)
{
    const TitleConfig *config = one(
        "{\"menu\": {\"buttons\": [{\"id\": \"credits\", \"label\": \"CREDITS\","
        "                          \"notice\": {\"title\": \"Credits\", \"text\": \"Made by me\"}},"
        "                         {\"id\": \"quick\", \"menu\": \"second\", \"image\": \"q.png\","
        "                          \"selected_image\": \"q-on.png\", \"width\": 96, \"action\": \"free_duel\","
        "                          \"x\": 8, \"value\": 3}],"
        "            \"order\": [\"credits\", \"new_game\"],"
        "            \"entries\": {\"trade\": {\"label\": \"SWAP\", \"action\": \"debug_menu\"}}}}");
    const TitleItem *credits = &config->items[TITLE_ENTRIES], *quick = &config->items[TITLE_ENTRIES + 1];
    CHECK(notes == 0);
    CHECK(credits->used && !strcmp(credits->name, "test:credits") && credits->menu == 0);
    CHECK(!strcmp(credits->label, "CREDITS") && credits->action == TITLE_ACTION_NOTICE);
    CHECK(!strcmp(credits->notice_title, "Credits") && !strcmp(credits->notice, "Made by me"));
    CHECK(quick->menu == 1 && quick->action == 6 && quick->x == 8 && quick->value == 3);
    CHECK(!strcmp(quick->image.file, "/mods/test/q.png") && !strcmp(quick->selected.file, "/mods/test/q-on.png"));
    CHECK(quick->image.width == 96 && quick->selected.width == 96);
    CHECK(TitleConfig_Drawn(credits) && TitleConfig_Drawn(quick) && !TitleConfig_Drawn(&config->items[0]));
    CHECK(!strcmp(config->items[3].label, "SWAP") && config->items[3].action == TITLE_ACTION_DEBUG_MENU);
    CHECK(config->items[0].action == TITLE_ACTION_OWN);
    /* The order: the list's, then the rest as they were. */
    CHECK(config->shown[0] == 6 && config->order[0][0] == TITLE_ENTRIES && config->order[0][1] == 0);
    CHECK(config->order[0][2] == 1 && config->order[0][5] == 4);
    /* Six 32 apart around the first menu's middle (114). */
    CHECK(credits->y == 34 && config->items[0].y == 66 && config->items[4].y == 194);
    /* Seven in the second: closer (31 apart) and kept above y 204. */
    CHECK(config->shown[1] == 7 && config->order[1][6] == TITLE_ENTRIES + 1);
    CHECK(config->items[5].y == 18 && quick->y == 204);
}

static void button_mistakes(void)
{
    const TitleConfig *config = one("{\"menu\": {\"buttons\": [{\"id\": \"plain\"}]}}");
    /* No picture or words: it shows its id, and does nothing. */
    CHECK(notes == 1 && strstr(note, "no \"image\" or \"label\""));
    CHECK(!strcmp(config->items[TITLE_ENTRIES].label, "plain") && config->items[TITLE_ENTRIES].action == TITLE_ACTION_NONE);
    config = one("{\"menu\": {\"buttons\": [{\"id\": \"a\", \"label\": \"A\", \"action\": \"campaign\"}]}}");
    CHECK(notes == 1 && strstr(note, "cannot campaign from the first menu"));
    CHECK(config->items[TITLE_ENTRIES].action == TITLE_ACTION_NONE);
    config = one("{\"menu\": {\"entries\": {\"save\": {\"action\": \"load\"}}}}");
    CHECK(notes == 1 && strstr(note, "cannot load from the second menu") && config->items[10].action == TITLE_ACTION_NONE);
    one("{\"menu\": {\"buttons\": [{\"id\": \"a\", \"label\": \"A\", \"action\": \"fly\"}]}}");
    CHECK(notes == 1 && strstr(note, "no action \"fly\""));
    one("{\"menu\": {\"buttons\": [{\"id\": \"a b\", \"label\": \"A\"}]}}");
    CHECK(notes == 1 && strstr(note, "letters, digits"));
    one("{\"menu\": {\"buttons\": [{\"id\": \"load\", \"label\": \"A\"}]}}");
    CHECK(notes == 1 && strstr(note, "an entry's name"));
    one("{\"menu\": {\"buttons\": [{\"id\": \"other:x\", \"hide\": true}]}}");
    CHECK(notes == 1 && strstr(note, "no button \"other:x\""));
    one("{\"menu\": {\"buttons\": [{\"id\": \"a\", \"label\": \"A\", \"menu\": \"third\", \"color\": 1}]}}");
    CHECK(notes == 2);
    one("{\"menu\": {\"order\": [\"new_game\", \"nothing\", \"campaign\"]}}");
    CHECK(notes == 2 && strstr(note, "\"campaign\" in the first menu"));
    one("{\"menu\": {\"buttons\": [{\"id\": \"a\", \"label\": \"A very long label that goes on and on\"}]}}");
    CHECK(notes == 1 && strstr(note, "longer than"));
    one("{\"menu\": {\"buttons\": ["
        "{\"id\": \"b0\", \"label\": \"x\"}, {\"id\": \"b1\", \"label\": \"x\"}, {\"id\": \"b2\", \"label\": \"x\"},"
        "{\"id\": \"b3\", \"label\": \"x\"}, {\"id\": \"b4\", \"label\": \"x\"}, {\"id\": \"b5\", \"label\": \"x\"},"
        "{\"id\": \"b6\", \"label\": \"x\"}, {\"id\": \"b7\", \"label\": \"x\"}, {\"id\": \"b8\", \"label\": \"x\"},"
        "{\"id\": \"b9\", \"label\": \"x\"}, {\"id\": \"c0\", \"label\": \"x\"}, {\"id\": \"c1\", \"label\": \"x\"},"
        "{\"id\": \"c2\", \"label\": \"x\"}, {\"id\": \"c3\", \"label\": \"x\"}, {\"id\": \"c4\", \"label\": \"x\"},"
        "{\"id\": \"c5\", \"label\": \"x\"}, {\"id\": \"c6\", \"label\": \"x\"}]}}");
    CHECK(notes == 1 && strstr(note, "more than 16 buttons"));
    config = one("{\"menu\": 3}");
    CHECK(notes == 1 && strstr(note, "\"menu\" is an object"));
    config = one("{\"menu\": {\"button\": []}}");
    CHECK(notes == 1 && strstr(note, "unknown key \"button\""));
}

static void buttons_across_mods(void)
{
    const TitleConfig *config;
    TitleConfig_Reset();
    notes = 0;
    add_as("first", "{\"menu\": {\"buttons\": [{\"id\": \"x\", \"label\": \"X\"}, {\"id\": \"y\", \"label\": \"Y\"}]}}");
    /* Another mod hides one of them, names both in its order and adds its own. */
    add_as("second", "{\"menu\": {\"buttons\": [{\"id\": \"first:x\", \"hide\": true}, {\"id\": \"x\", \"label\": \"Z\"}],"
                     "          \"order\": [\"first:y\", \"x\", \"load\"]}}");
    TitleConfig_Finish();
    config = TitleConfig_Get();
    CHECK(notes == 0);
    CHECK(config->items[TITLE_ENTRIES].hidden && !strcmp(config->items[TITLE_ENTRIES + 2].name, "second:x"));
    CHECK(config->order[0][0] == TITLE_ENTRIES + 1 && config->order[0][1] == TITLE_ENTRIES + 2);
    CHECK(config->order[0][2] == 1 && config->order[0][3] == 0 && config->shown[0] == 7);
    CHECK(config->items[TITLE_ENTRIES].y == TITLE_PARKED_Y);
}

/* A mod's tower (notes/tower-mode.md) is a button of its mod's in the
 * second menu, "<mod>:tower", made before the mod's "menu" is read so the
 * mod can change it and name it in its "order"; only the tower's own mod
 * gets it. "tower" is an action of the second menu's. */
static void tower(void)
{
    const TitleConfig *config;
    const TitleItem *button = NULL;
    int i;
    tower_mod = "test";
    tower_label = "SHADOW TOWER";
    config = one("{\"menu\": {\"buttons\": [{\"id\": \"tower\", \"tint\": \"#FF8000\"}],"
                 "            \"order\": {\"second\": [\"tower\", \"campaign\"]}}}");
    for (i = TITLE_ENTRIES; i < TITLE_ITEMS; i++) {
        if (config->items[i].used && !strcmp(config->items[i].name, "test:tower")) button = &config->items[i];
    }
    CHECK(notes == 0 && button != NULL);
    CHECK(button->menu == 1 && button->action == TITLE_ACTION_TOWER && !strcmp(button->label, "SHADOW TOWER"));
    CHECK(button->tint == 0xFF8000);
    CHECK(config->shown[1] == 7 && config->order[1][0] == button - config->items && config->order[1][1] == 5);
    CHECK(!strcmp(TitleConfig_ActionName(TITLE_ACTION_TOWER), "tower"));
    /* Another mod's manifest gets none; without a tower, nobody does. */
    TitleConfig_Reset();
    notes = 0;
    add_as("other", "{\"menu\": {}}");
    TitleConfig_Finish();
    CHECK(TitleConfig_Get()->shown[1] == 6 && TitleConfig_Get()->shown[0] == 5);
    tower_mod = NULL;
    config = one("{\"menu\": {}}");
    CHECK(config->shown[1] == 6 && !config->items[TITLE_ENTRIES].used);
    /* A button of a mod's own may take the tower's action, in the second
     * menu only. */
    config = one("{\"menu\": {\"buttons\": [{\"id\": \"climb\", \"label\": \"CLIMB\", \"menu\": \"second\","
                 "\"action\": \"tower\"}, {\"id\": \"up\", \"label\": \"UP\", \"action\": \"tower\"}]}}");
    CHECK(config->items[TITLE_ENTRIES].action == TITLE_ACTION_TOWER);
    CHECK(notes == 1 && strstr(note, "cannot tower from the first menu"));
    CHECK(config->items[TITLE_ENTRIES + 1].action == TITLE_ACTION_NONE);
    tower_label = "TOWER";
}

static void menu_background(void)
{
    const TitleConfig *config = one(
        "{\"title\": {\"background\": {\"tint\": \"#FF0000\", \"image\": \"bg.png\"}, \"logo\": {\"show\": \"press_start\"},"
        "             \"copyright\": {\"show\": \"menu\"}},"
        " \"menu\": {\"background\": {\"picture\": false, \"color\": \"#000010\", \"dim\": 0}}}");
    CHECK(notes == 0);
    CHECK(!config->background[1].picture && config->background[1].color == 0x10 && config->dim == 0);
    /* What the menus' did not set is the title's. */
    CHECK(config->background[1].tint == 0xFF0000 && config->background[1].shade);
    CHECK(!strcmp(config->background[1].image.file, "/mods/test/bg.png"));
    CHECK(config->background[0].picture && config->background[0].color == -1);
    CHECK(config->layers[0].show == TITLE_SHOW_PROMPT && config->layers[1].show == TITLE_SHOW_MENU);
    config = one("{\"menu\": {\"background\": {\"image\": \"menu.png\"}}}");
    CHECK(!strcmp(config->background[1].image.file, "/mods/test/menu.png") && !config->background[0].image.file[0]);
    one("{\"title\": {\"prompt\": {\"show\": \"menu\"}}}");
    CHECK(notes == 1 && strstr(note, "prompt \"show\""));
}

static void widescreen(void)
{
    const TitleConfig *config = one(
        "{\"title\": {\"background\": {\"wide\": true}, \"logo\": {\"wide_x\": -40},"
        "             \"text\": [{\"text\": \"v1\", \"x\": 300, \"wide_x\": 360}]},"
        " \"menu\": {\"background\": {\"image\": \"m.png\", \"wide_image\": \"m-wide.png\"},"
        "          \"entries\": {\"load\": {\"wide_x\": -100, \"wide_y\": 30}},"
        "          \"buttons\": [{\"id\": \"b\", \"label\": \"B\", \"x\": 10, \"wide_x\": 150}]}}");
    const TitleItem *load = &config->items[1], *b = &config->items[TITLE_ENTRIES];
    CHECK(notes == 0);
    CHECK(config->background[0].wide && !config->background[0].wide_image.file[0]);
    /* A picture for widescreen makes the background fill it. */
    CHECK(config->background[1].wide && !strcmp(config->background[1].wide_image.file, "/mods/test/m-wide.png"));
    CHECK(config->layers[0].wide.set_x && config->layers[0].wide.x == -40 && !config->layers[0].wide.set_y);
    CHECK(config->line[0].wide.set_x && TitleWide_X(&config->line[0].wide, config->line[0].x, 1) == 360);
    CHECK(TitleWide_X(&config->line[0].wide, config->line[0].x, 0) == 300);
    CHECK(TitleWide_X(&load->wide, load->x, 1) == -100 && TitleWide_Y(&load->wide, load->y, 1) == 30);
    /* 4:3 keeps the stacked place; widescreen without a wide_y does too. */
    CHECK(TitleWide_Y(&load->wide, load->y, 0) == load->y && load->y != 30);
    CHECK(TitleWide_X(&b->wide, b->x, 1) == 150 && TitleWide_X(&b->wide, b->x, 0) == 10);
    CHECK(TitleWide_Y(&b->wide, b->y, 1) == b->y);
    /* The menus' own 4:3 picture without a wide one: not the title's wide one. */
    config = one("{\"title\": {\"background\": {\"wide_image\": \"t-wide.png\"}},"
                 " \"menu\": {\"background\": {\"image\": \"m.png\"}}}");
    CHECK(config->background[0].wide && !strcmp(config->background[0].wide_image.file, "/mods/test/t-wide.png"));
    CHECK(!config->background[1].wide_image.file[0] && config->background[1].wide);
    /* Nothing said: the menus' is the title's. */
    config = one("{\"title\": {\"background\": {\"wide_image\": \"t-wide.png\"}}}");
    CHECK(!strcmp(config->background[1].wide_image.file, "/mods/test/t-wide.png"));
    one("{\"menu\": {\"background\": {\"wide_imag\": \"x.png\"}}}");
    CHECK(notes == 1 && strstr(note, "unknown key \"wide_imag\""));
}

/* "images": pictures of the mods' own, adding up across mods, each centred
 * at its x and y (160, 120 unless given). */
static void added_pictures(void)
{
    const TitleConfig *config = one("{\"title\": {\"images\": [{\"image\": \"art/seal.png\", \"x\": 40, \"y\": 30,"
                                    " \"width\": 64, \"tint\": \"#FF8000\", \"show\": \"menu\", \"wide_x\": -10},"
                                    " {\"image\": \"art/b.png\"}]}}");
    CHECK(notes == 0);
    CHECK(config->pictures == 2);
    CHECK(!strcmp(config->picture[0].image.file, "/mods/test/art/seal.png") && config->picture[0].image.width == 64);
    CHECK(config->picture[0].x == 40 && config->picture[0].y == 30 && config->picture[0].tint == 0xFF8000);
    CHECK(config->picture[0].show == TITLE_SHOW_MENU && config->picture[0].wide.set_x && config->picture[0].wide.x == -10);
    CHECK(config->picture[1].x == 160 && config->picture[1].y == 120 && config->picture[1].tint == 0xFFFFFF);
    CHECK(config->picture[1].show == TITLE_SHOW_ALWAYS);
    /* A second mod's add to the first's. */
    add_as("other", "{\"title\": {\"images\": [{\"image\": \"c.png\", \"y\": 200}]}}");
    CHECK(config->pictures == 3 && config->picture[2].y == 200);
    /* Mistakes: no image, outside the mod, an unknown key, a bad show, too many. */
    one("{\"title\": {\"images\": [{\"x\": 3}]}}");
    CHECK(notes == 1 && TitleConfig_Get()->pictures == 0);
    one("{\"title\": {\"images\": [{\"image\": \"../x.png\"}]}}");
    CHECK(notes == 1 && TitleConfig_Get()->pictures == 0);
    one("{\"title\": {\"images\": [{\"image\": \"a.png\", \"spin\": 1, \"show\": \"never\"}]}}");
    CHECK(notes == 2 && TitleConfig_Get()->pictures == 1);
    one("{\"title\": {\"images\": [{\"image\": \"1.png\"}, {\"image\": \"2.png\"}, {\"image\": \"3.png\"},"
        " {\"image\": \"4.png\"}, {\"image\": \"5.png\"}, {\"image\": \"6.png\"}, {\"image\": \"7.png\"},"
        " {\"image\": \"8.png\"}, {\"image\": \"9.png\"}]}}");
    CHECK(notes == 1 && TitleConfig_Get()->pictures == TITLE_MAX_PICTURES);
    one("{\"title\": {\"images\": {\"image\": \"a.png\"}}}");
    CHECK(notes == 1 && TitleConfig_Get()->pictures == 0);
}

/* "scale": each item's size, its own or the menu's, and the room each
 * takes in the menu: `spacing` at 100, as its size, two the halves of
 * theirs apart. */
static void sizes(void)
{
    const TitleConfig *config = one("{\"id\": \"x\"}");
    int i;
    CHECK(config->scale == 100);
    for (i = 0; i < TITLE_ENTRIES; i++) CHECK(config->items[i].scale == 100);
    /* The whole menu at 120: 38.4 apart, from the middle (114) out. */
    config = one("{\"menu\": {\"scale\": 120}}");
    CHECK(notes == 0 && config->scale == 120);
    for (i = 0; i < TITLE_ENTRIES; i++) CHECK(config->items[i].scale == 120);
    CHECK(config->items[0].y == 38 && config->items[1].y == 76 && config->items[2].y == 114);
    CHECK(config->items[3].y == 153 && config->items[4].y == 191);
    /* The second menu's six would not fit: 30 apart at 100 (36 at 120),
     * the ends 17 in (14 at 120 %) from y 2 and 218, ending at 201. */
    CHECK(config->items[5].y == 21 && config->items[6].y == 57 && config->items[10].y == 201);
    /* One entry's own size over the menu's: only it takes more room. */
    config = one("{\"menu\": {\"scale\": 80, \"entries\": {\"load\": {\"scale\": 150}},"
                 " \"buttons\": [{\"id\": \"big\", \"label\": \"BIG\", \"menu\": \"second\", \"scale\": 60}]}}");
    CHECK(notes == 0);
    CHECK(config->items[1].scale == 150 && config->items[0].scale == 80 && config->items[TITLE_ENTRIES].scale == 60);
    CHECK(config->items[0].y == 52 && config->items[1].y == 88 && config->items[2].y == 125);
    CHECK(config->items[3].y == 151 && config->items[4].y == 176);
    CHECK(config->items[5].y == 47 && config->items[6].y == 72 && config->items[TITLE_ENTRIES].y == 197);
    /* A hidden item's size takes no room. */
    config = one("{\"menu\": {\"entries\": {\"trade\": {\"scale\": 300, \"hide\": true}}}}");
    CHECK(config->items[0].y == 66 && config->items[1].y == 98 && config->items[4].y == 162);
    /* A mod's "spacing", the title's or the menu's, is the room at 100. */
    config = one("{\"menu\": {\"scale\": 150, \"spacing\": 20}}");
    CHECK(config->items[0].y == 54 && config->items[1].y == 84 && config->items[4].y == 174);
    config = one("{\"title\": {\"spacing\": 24}, \"menu\": {\"scale\": 150}}");
    CHECK(config->items[0].y == 42 && config->items[1].y == 78 && config->items[4].y == 186);
    /* "entries" under "title" take a "scale" too. */
    config = one("{\"title\": {\"entries\": {\"options\": {\"scale\": 90}}}}");
    CHECK(notes == 0 && config->items[4].scale == 90 && config->items[0].scale == 100);
    /* A later mod's menu size leaves an earlier one's own sizes be. */
    while (document_count) Json_Free(documents[--document_count]);
    TitleConfig_Reset();
    add_as("a", "{\"menu\": {\"entries\": {\"load\": {\"scale\": 70}}}}");
    add_as("b", "{\"menu\": {\"scale\": 130}}");
    TitleConfig_Finish();
    config = TitleConfig_Get();
    CHECK(config->items[1].scale == 70 && config->items[0].scale == 130);
    /* Out of 25 to 400, or not a number: noted and left out. */
    config = one("{\"menu\": {\"scale\": 500}}");
    CHECK(notes == 1 && strstr(note, "\"scale\" is a whole number from 25 to 400") && config->scale == 100);
    config = one("{\"menu\": {\"entries\": {\"load\": {\"scale\": \"big\"}}}}");
    CHECK(notes == 1 && strstr(note, "load \"scale\"") && config->items[1].scale == 100);
    config = one("{\"menu\": {\"buttons\": [{\"id\": \"b\", \"label\": \"B\", \"scale\": 10}]}}");
    CHECK(notes == 1 && strstr(note, "test:b \"scale\"") && config->items[TITLE_ENTRIES].scale == 100);
}

/* An animated background: its frames, at most TITLE_MAX_FRAMES, and the
 * menus' taking the title's unless they have a picture of their own. */
static void frames(void)
{
    const TitleConfig *config = one(
        "{\"title\": {\"background\": {\"frames\": [\"a.png\", \"b.png\"], \"frame_time\": 5,"
        "                             \"wide_frames\": [\"wa.png\"]}}}");
    CHECK(notes == 0);
    CHECK(config->background[0].frame_count == 2 && config->background[0].frame_time == 5);
    CHECK(!strcmp(config->background[0].frames[1].file, "/mods/test/b.png"));
    CHECK(config->background[0].wide_frame_count == 1);
    CHECK(config->background[1].frame_count == 2 && config->background[1].frame_time == 5);
    config = one("{\"title\": {\"background\": {\"frames\": [\"a.png\"]}},"
                 " \"menu\": {\"background\": {\"image\": \"m.png\"}}}");
    CHECK(config->background[0].frame_count == 1 && !config->background[1].frame_count);
    CHECK(config->background[1].frame_time == TITLE_FRAME_TIME);
    config = one("{\"menu\": {\"background\": {\"frames\": [\"1.png\", \"2.png\", \"3.png\", \"4.png\", \"5.png\","
                 " \"6.png\", \"7.png\", \"8.png\", \"9.png\", \"10.png\", \"11.png\", \"12.png\", \"13.png\"],"
                 " \"frame_time\": 0}}}");
    CHECK(notes == 1 && strstr(note, "at most"));
    CHECK(config->background[1].frame_count == TITLE_MAX_FRAMES && config->background[1].frame_time == 1);
    CHECK(!config->background[0].frame_count);
    /* One PNG: a sheet of them, one under another. */
    config = one("{\"title\": {\"background\": {\"frames\": \"sheet.png\", \"wide_frames\": \"wide.png\"}}}");
    CHECK(notes == 0);
    CHECK(config->background[0].frame_count == 1 && config->background[0].frame_sheet);
    CHECK(!strcmp(config->background[0].frames[0].file, "/mods/test/sheet.png"));
    CHECK(config->background[0].wide_frame_count == 1 && config->background[0].wide_frame_sheet);
    CHECK(config->background[1].frame_sheet && config->background[1].wide_frame_sheet);
    /* A list after a sheet is a list again. */
    config = one("{\"title\": {\"background\": {\"frames\": [\"a.png\", \"b.png\"]}}}");
    CHECK(!config->background[0].frame_sheet && config->background[0].frame_count == 2);
    one("{\"title\": {\"background\": {\"frames\": 5}}}");
    CHECK(notes == 1 && strstr(note, "one under another"));
}

int main(void)
{
    retail();
    keys();
    layout();
    pictures();
    mods_add_up();
    mistakes();
    buttons();
    button_mistakes();
    buttons_across_mods();
    tower();
    menu_background();
    widescreen();
    added_pictures();
    sizes();
    frames();
    while (document_count) Json_Free(documents[--document_count]);
    printf("title config: ok\n");
    return 0;
}
