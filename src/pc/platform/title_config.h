#ifndef MEMORIES_PC_PLATFORM_TITLE_CONFIG_H
#define MEMORIES_PC_PLATFORM_TITLE_CONFIG_H
/* The mods' "title" and "menu" keys as read (title_config.c); title_screen.h
 * puts them on the screen and title_menu.h runs the menus. Places are in the
 * game's 320 x 240, colors 0xRRGGBB. */
#include <stdint.h>

struct JsonValue;

/* The three sprites, in "title" order: the logo, the copyright line and
 * PUSH START BUTTON (D_80184558, D_8018455C, D_80184560); the eleven
 * entries in the game's order, the first menu's five then the second's six
 * (main_menu_selection.h), then the buttons the mods add. */
enum { TITLE_LAYERS = 3, TITLE_ENTRIES = 11, TITLE_FIRST_MENU = 5, TITLE_MAX_LINES = 16, TITLE_LINE_TEXT = 96,
       TITLE_PATH = 1024, TITLE_MAX_BUTTONS = 16, TITLE_ITEMS = TITLE_ENTRIES + TITLE_MAX_BUTTONS,
       TITLE_LABEL = 32, TITLE_NOTICE = 512, TITLE_NAME = 96, TITLE_MENUS = 2, TITLE_MAX_PICTURES = 8,
       TITLE_MAX_FRAMES = 12, TITLE_MAX_SHEET_FRAMES = 64, TITLE_FRAME_TIME = 8 };
enum { TITLE_SHOW_ALWAYS, TITLE_SHOW_PROMPT, TITLE_SHOW_MENU };
enum { TITLE_ALIGN_LEFT, TITLE_ALIGN_CENTRE, TITLE_ALIGN_RIGHT };
/* A hidden entry's y: off the screen, afterimages and all. */
enum { TITLE_PARKED_Y = -400 };
/* An item's size in percent ("scale"), about its middle: the menu's
 * "scale" for one without its own, 100 as the game has it. */
enum { TITLE_SCALE_MIN = 25, TITLE_SCALE_MAX = 400 };
/* What an item does when chosen: its own (an entry's retail choice), one of
 * the eleven retail choices (MainMenuSelection, 0 to 10), or one of these. */
enum {
    TITLE_ACTION_OWN = -1,
    TITLE_ACTION_BACK = 100, /* the first menu: back to PUSH START BUTTON; the second: to the first */
    TITLE_ACTION_NOTICE,     /* a box with the item's "notice" */
    TITLE_ACTION_QUIT,       /* asks, then quits the game */
    TITLE_ACTION_DEBUG_MENU, /* the game's own debug menu */
    TITLE_ACTION_EVENT,      /* only a code mod's MEMORIES_EVENT_MENU */
    TITLE_ACTION_NONE,       /* nothing: a buzz */
    TITLE_ACTION_TOWER       /* the tower a mod's "tower" made (pc/tower/tower.h); the second menu's */
};
extern const char *const TitleConfig_LayerNames[TITLE_LAYERS];
extern const char *const TitleConfig_EntryNames[TITLE_ENTRIES];

/* Widescreen (View > Aspect 16:9): the picture is 4/3 as wide, a margin of
 * TITLE_WIDE_MARGIN of the game's pixels either side of its 320 (soft_gpu.c,
 * SoftGpu_WideMargin), which polygons are drawn into and sprites are not.
 * A place given for widescreen ("wide_x", "wide_y") is used instead of the
 * other while it is on. */
enum { TITLE_WIDE_MARGIN = 54, TITLE_WIDE_WIDTH = 320 + 2 * TITLE_WIDE_MARGIN };
typedef struct {
    int x, y, set_x, set_y;
} TitleWide;
static inline int TitleWide_X(const TitleWide *wide, int x, int on) { return on && wide->set_x ? wide->x : x; }
static inline int TitleWide_Y(const TitleWide *wide, int y, int on) { return on && wide->set_y ? wide->y : y; }

typedef struct {
    char text[TITLE_LINE_TEXT];
    int x, y, align, show, size;
    uint32_t color;
    TitleWide wide;
} TitleLine;

/* A picture of the mod's own in place of the game's: its PNG ("" for none),
 * the mod that named it, and the size it is drawn at in the game's pixels
 * (0: worked out from the PNG). */
typedef struct {
    char file[TITLE_PATH];
    char mod[64];
    int width, height;
} TitleImage;

/* The background: the game's tiled picture of hieroglyphs and a shade over
 * it, or a picture of the mod's own, over a solid color. */
typedef struct {
    int picture, shade;        /* the picture (the game's or the mod's), the dark-to-light shade */
    long color;               /* under the picture, or -1 */
    uint32_t tint;             /* 0xFFFFFF unchanged */
    TitleImage image;          /* the mod's picture, "" for the game's */
    /* Widescreen: whether it fills the sides (the game's wall tiles on, the
     * shade and color widen; a 4:3 picture keeps its shape, with the color
     * beside it), and a picture 4/3 as wide drawn instead. */
    int wide;
    TitleImage wide_image;
    /* An animated picture: "frames" (and "wide_frames") drawn in turn in
     * place of the image, each for frame_time of the game's frames; none,
     * the image alone. Either a list of PNGs or, `frame_sheet` set, one PNG
     * (frames[0]) with them one under another (title_images.c). */
    TitleImage frames[TITLE_MAX_FRAMES], wide_frames[TITLE_MAX_FRAMES];
    int frame_count, wide_frame_count, frame_time;
    int frame_sheet, wide_frame_sheet;
} TitleBackground;

enum { TITLE_BACKGROUND_PICTURE = 1, TITLE_BACKGROUND_SHADE = 2, TITLE_BACKGROUND_COLOR = 4,
       TITLE_BACKGROUND_TINT = 8, TITLE_BACKGROUND_IMAGE = 16, TITLE_BACKGROUND_WIDE = 32,
       TITLE_BACKGROUND_WIDE_IMAGE = 64, TITLE_BACKGROUND_FRAMES = 128, TITLE_BACKGROUND_WIDE_FRAMES = 256,
       TITLE_BACKGROUND_FRAME_TIME = 512,
       /* The British spelling released SDKs had (v0.2.0); kept for mods built against them. */
       TITLE_BACKGROUND_COLOUR = TITLE_BACKGROUND_COLOR };

/* One thing a menu offers: one of the eleven entries, or a button a mod
 * adds. An entry is the game's sprite unless it is given a picture or a
 * label; a button always is one of those, drawn by the port. */
typedef struct {
    char name[TITLE_NAME];     /* an entry's name, or a button's "mod:id" */
    char mod[64];              /* the mod that made or last changed it */
    int used;                  /* a button slot in use (entries always are) */
    int menu;                  /* 0 the first menu, 1 the second */
    int hidden, x, y, set_y;   /* x added to the middle (160); y its middle */
    int scale;                 /* its size in percent; 0 the menu's (TitleConfig.scale), until TitleConfig_Finish */
    TitleWide wide;            /* the same in widescreen; y worked out as y is when not given */
    uint32_t tint;
    TitleImage image, selected; /* its picture, and the picture while the cursor is on it */
    char label[TITLE_LABEL];   /* or its words, on a frame of the game's look */
    int font_game;             /* "font": "game": the words in the entries' own letters */
    int action, value;         /* TITLE_ACTION_*, and the number a code mod's event gets */
    char notice_title[64];
    char notice[TITLE_NOTICE];
} TitleItem;

/* A picture a mod adds to the title ("images"): its middle's place in the
 * game's 320 x 240 (TitleWide in widescreen), its colors multiplied, and
 * when it shows (TITLE_SHOW_*). */
typedef struct {
    TitleImage image;
    int x, y, show;
    uint32_t tint;
    TitleWide wide;
} TitlePicture;

typedef struct {
    int song;                  /* 0x000 retail */
    int skip_movie, press_start;
    int idle_frames;           /* -1 retail, 0 never */
    int dim;                   /* dim 0-0x80, retail 0x80 */
    int fade;                  /* frames each way through black as a menu opens or closes, 0 none (retail) */
    int smooth_slide;          /* the menu's "slide": "smooth": a 24-tick eased slide, no afterimages; 0 the game's own */
    /* The title's background, then the one while a menu is up: the title's
     * but for what a mod's "menu" "background" set (TITLE_BACKGROUND_*). */
    TitleBackground background[TITLE_MENUS];
    unsigned menu_set;
    struct { int x, y, hidden, show; uint32_t tint; TitleImage image; TitleWide wide; } layers[TITLE_LAYERS]; /* x, y added to the game's */
    TitleItem items[TITLE_ITEMS];
    int spacing, lines;
    int scale;                 /* the menu's "scale": every item's without its own, 100 retail */
    TitleLine line[TITLE_MAX_LINES];
    int pictures;              /* every mod's "images", in load order */
    TitlePicture picture[TITLE_MAX_PICTURES];
    /* Worked out by TitleConfig_Finish: each menu's shown items, top to
     * bottom as the cursor goes, and how many. */
    int order[TITLE_MENUS][TITLE_ITEMS], shown[TITLE_MENUS];
    /* The mods' "order" lists, names as given, read at TitleConfig_Finish. */
    char order_names[TITLE_MENUS][TITLE_ITEMS][TITLE_NAME];
    char order_mod[TITLE_MENUS][64];
    int order_count[TITLE_MENUS];
} TitleConfig;

/* The retail title, then each applied mod's "title" and "menu" over it,
 * then the menus laid out (TitleConfig_Finish). The mod whose "tower" the
 * run has (Tower_Mod) gets a button of its own first, "<mod>:tower" in the
 * second menu, which its "menu" may change like any of its buttons. */
const TitleConfig *TitleConfig_Load(void);
/* The last one loaded (the retail one before any). */
const TitleConfig *TitleConfig_Get(void);
/* The pieces of TitleConfig_Load, for tests: back to retail; one manifest's
 * "title" and "menu" over what there is; the menus worked out -- a menu
 * with every item hidden shows its entries, the "order" lists put the
 * items in order, each item gets its size (its own "scale" or the menu's),
 * and the shown ones stand `spacing` apart around the retail menu's middle
 * unless given a "y" -- `spacing` at their size 100, each taking room as
 * its size, so bigger items do not run into each other. `directory` is
 * the mod's, which
 * its "image" files are named from. */
void TitleConfig_Reset(void);
void TitleConfig_Read(const char *mod, const char *directory, const struct JsonValue *manifest);
void TitleConfig_Finish(void);
/* Whether the port draws item `i` (a picture or a label), not the game. */
int TitleConfig_Drawn(const TitleItem *item);
/* The name of an action, for messages and the mods' API. */
const char *TitleConfig_ActionName(int action);
#endif
