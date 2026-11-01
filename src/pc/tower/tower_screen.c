/* The tower's screen (tower.h): mode TOWER_MODE, run by the game's unused
 * developer-mode runner (main_mode_runners.c), entered from the title's second menu and come back
 * to after every floor's duel.
 *
 * Everything on it is the game's own: its text boxes with their window,
 * letters, page arrow, choice cursor and sounds over the title's wall, and
 * a story told as the campaign tells a scene's dialogue (the stage, below):
 * a scene, the campaign's large characters and its stone-framed box, small
 * portraits beside it drawn by the dialogue's portrait effect (F6 41-59)
 * from the 48 x 48 cells the campaign's scene package fills. The text is
 * composed here in the game's codes into gTower.text, a game variable, so
 * the boxes point into the game's data and a save state keeps them.
 *
 * Its steps (gTower.step):
 *
 *   BEGIN    the first frame: the music, the result of the duel just come
 *            back from (Tower_ApplyResult, the reward), then what to tell
 *   STORY    a story on its stage, in one box, page by page
 *   NOTICE   one box of what happened (a floor won, a loss), then on
 *   STATUS   the tower's name, the floor, its opponent and rules: DUEL or
 *            LEAVE
 *   SAVE     a checkpoint reached: SAVE? YES/NO, then the game's own save
 *            (the memory card dialog, the port's slot menu)
 *   DUEL     the floor's duel armed as the Free Duel screen arms one; the
 *            game comes back here after it (D_8009B368)
 *   LEAVE    the title's second menu, the tower's button under the cursor
 */
#include "tower.h"
#include "pc/free_duel/duelists.h"
#include "pc/free_duel/busts.h"
#include "pc/cards/cards.h"
#include "pc/cards/disc_art.h"
#include "pc/cards/tables.h"
#include "pc/cards/fields.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "pc/debug/log.h"
#ifdef MEMORIES_TRANSLATED
#include "pc/guest/translated_runtime.h"
#endif
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "game/duel_effect.h"
#include "game/text_box_lifecycle.h"
#include "game/func_80039794.h"
#include "game/dialog_choice.h"
#include "game/fade.h"
#include "game/sound.h"
#include "game/func_80024DC8.h"
#include "game/duel_rewards.h"
#include "game/main_modes.h"
#include "game/save_data.h"
#include "game/data_transfer_request.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The game's bytes, declared plainly: this unit is native (title_jump.c). */
extern u8 D_8009B26C, D_8009B26D, D_8009B268, D_8009B368, D_8009B362, gDuel_bTerrain;
extern u16 gDuel_awPlayerDeck[];

/* The box's string id: past the port's own (text.h), clear of the disc's. */
#define TOWER_TEXT_ID 0xFFF8
#define TEXT_START 16                   /* gTower.text[0] is the {end} a choice jumps to */
#define LETTER 8                        /* a letter's width in a box */
#define DIALOG_LINES 3                  /* a page of the dialogue box */
#define SECOND_MENU 5                   /* D_8009B26D: the title opens on its second menu */
#define TOWER_DEFAULT_SONG 0x2C0        /* the Free Duel screen's (SD_BGMPlay(0x72C0)) */
#define SE_CHOOSE 0x30                  /* the Free Duel screen's for a duel chosen */
#define SE_BUZZ 9
#define FREE_DUEL_PORTRAITS_SECTOR 0x1EAA   /* WA: the Free Duel package's portrait phase */
#define CAMPAIGN_PORTRAITS_SECTOR 0x1E6A    /* WA 0xF35000: the scene package's */
#define PORTRAIT_RECORD 0x980
#define PORTRAIT_IMAGE 0x900
#define PORTRAIT_EFFECT 0x41            /* F6 id of cell 0 */

/* The dialogue box (Script_OpShowDialog's place) and the menu box. */
enum { DIALOG_X = 0x10, DIALOG_Y = 0xB0, DIALOG_W = 0x120, DIALOG_H = 0x30 };
enum { MENU_X = 0x18, MENU_W = 0x110 };
/* TextBox_CreateFlagged's flags: 0x20 the dark window behind it
 * (func_800391E4 builds it), 0x08 kept up at its end until a button, which
 * func_80039794 clears. (Not 0x1000, which says a menu's choices are being
 * laid out.) A story's line ends in a page wait instead, and a choice is
 * its own button. */
#define BOX_WINDOW 0x20
#define BOX_KEPT 0x08

enum { STEP_BEGIN, STEP_STORY, STEP_NOTICE, STEP_STATUS, STEP_SAVE_ASK, STEP_SAVE, STEP_DUEL, STEP_LEAVE,
       STEP_NO_DECK };
enum { STORY_INTRO = 1, STORY_BEFORE, STORY_AFTER, STORY_ENDING, STORY_LOSE };
enum { OUTCOME_NONE, OUTCOME_WON, OUTCOME_CHECKPOINT, OUTCOME_COMPLETED, OUTCOME_LOST };

/* --- the text --------------------------------------------------------------------- */

typedef struct {
    u8 *at, *end;
    int x, limit;                       /* pixels into the line, and the line's width */
} Out;

static Out begin(int limit)
{
    Out out;
    memset(gTower.text, 0xFF, TEXT_START);
    out.at = gTower.text + TEXT_START;
    out.end = gTower.text + TOWER_TEXT_SIZE - 8;
    out.x = 0;
    out.limit = limit;
    return out;
}

static void put(Out *out, int byte)
{
    if (out->at < out->end) *out->at++ = (u8)byte;
}

static void finish(Out *out)
{
    *out->at = 0xFF;
}

static void newline(Out *out)
{
    put(out, 0xFE);
    out->x = 0;
}

static void glyph(Out *out, int code)
{
    if (code < 0 || out->x + LETTER > out->limit) return;
    if (out->at + (code >= 0xF0 ? 2 : 1) > out->end) return;   /* never half of a two-byte code */
    if (code >= 0xF0) put(out, 0xF0 + (code >> 8));
    put(out, code & 0xFF);
    out->x += LETTER;
}

static void words(Out *out, const char *text)
{
    while (*text) {
        uint32_t character = Glyphs_NextCharacter(&text);
        if (character == GLYPHS_NOT_UTF8) continue;
        glyph(out, character == ' ' ? 0 : Glyphs_Code(character));
    }
}

/* Codes as the game keeps a name (ending in FF). */
static void codes(Out *out, const unsigned char *text)
{
    while (text && *text != 0xFF && *text != 0xFE) {
        if (*text >= 0xF0 && *text < 0xF6) {
            glyph(out, ((text[0] - 0xF0) << 8) | text[1]);
            text += 2;
        } else if (*text < 0xF0) {
            glyph(out, *text++);
        } else {
            break;
        }
    }
}

static void number(Out *out, unsigned value)
{
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", value);
    words(out, digits);
}

/* One of the port's own strings (text.h): a translation's, else `english`;
 * each %d the next number, each %s the next name (UTF-8). A translation may
 * hold more of them than the port gives: past OWN_NUMBERS and OWN_NAMES
 * they are 0 and nothing. */
enum { OWN_NUMBERS = 2, OWN_NAMES = 1 };
static void own(Out *out, int id, const char *english, const unsigned *numbers, const char *const *names)
{
    const u8 *text = Text_Own(id);
    int percent = Glyphs_Code('%'), letter_d = Glyphs_Code('d'), letter_s = Glyphs_Code('s'), used = 0, named = 0;
    if (!text) {
        const char *c;
        for (c = english; *c; c++) {
            if (c[0] == '%' && c[1] == 'd') { number(out, numbers && used < OWN_NUMBERS ? numbers[used++] : 0); c++; }
            else if (c[0] == '%' && c[1] == 's') { words(out, names && named < OWN_NAMES ? names[named++] : ""); c++; }
            else glyph(out, *c == ' ' ? 0 : Glyphs_Code((unsigned char)*c));
        }
        return;
    }
    while (*text != 0xFF) {
        int code = *text++;
        if (code >= 0xF6) break;                       /* letters and spaces only */
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *text++;
        if (code == percent && *text == letter_d) {
            text++;
            number(out, numbers && used < OWN_NUMBERS ? numbers[used++] : 0);
        } else if (code == percent && *text == letter_s) {
            text++;
            words(out, names && named < OWN_NAMES ? names[named++] : "");
        } else {
            glyph(out, code);
        }
    }
}

/* A choice of two under up to three lines of heading, as the game lays one
 * out ({choice}, FB): its byte says the rows in all (the low three bits)
 * and how many of them head the choices (bits 4-5, which also make it a
 * choice of two; 0x40 as the password shop's EXCHANGE/QUIT has it), and
 * every row comes after it, since the command clears what the box showed.
 * Then the rows, and the jump the answer takes, which Tower_Retarget ends
 * the text at. `heading` is that many lines, FE between them. */
static void choices(Out *out, const char *const *heading, const int *heading_ids, int rows, int first,
                    const char *first_english, int second, const char *second_english)
{
    int row;
    put(out, 0xFB);
    put(out, 0x40 | rows << 4 | (rows + 2));
    for (row = 0; row < rows; row++) {
        if (heading_ids && heading_ids[row]) own(out, heading_ids[row], heading[row], NULL, NULL);
        else codes(out, (const unsigned char *)heading[row]);
        newline(out);
    }
    glyph(out, 0);
    own(out, first, first_english, NULL, NULL);
    newline(out);
    glyph(out, 0);
    own(out, second, second_english, NULL, NULL);
    newline(out);
    put(out, 0xFB);
    put(out, 0x80);
    put(out, 0); put(out, 0);
    put(out, 0); put(out, 0);
}

/* `text` (UTF-8) in lines of the box's width, broken at spaces, a page of
 * DIALOG_LINES at a time: a page wait (FA) between pages. A word longer
 * than a line is cut. */
static void wrapped(Out *out, const char *text, int lines_a_page)
{
    int line = 0;
    while (*text) {
        const char *word = text, *after;
        int letters = 0, room = (out->limit - out->x) / LETTER;
        if (*text == '\n') {
            text++;
            goto next_line;
        }
        if (*text == ' ') {
            if (out->x) glyph(out, 0);
            text++;
            continue;
        }
        after = word;
        while (*after && *after != ' ' && *after != '\n') {
            uint32_t character = Glyphs_NextCharacter(&after);
            if (character != GLYPHS_NOT_UTF8) letters++;
        }
        if (letters > room && out->x) goto next_line;
        while (text < after) {
            uint32_t character = Glyphs_NextCharacter(&text);
            if (character == GLYPHS_NOT_UTF8) continue;
            glyph(out, Glyphs_Code(character));
        }
        continue;
next_line:
        if (++line % lines_a_page == 0) {
            put(out, 0xFA);   /* the page is full: a button, then the next from its top */
            out->x = 0;
        } else {
            newline(out);
        }
        while (*text == ' ') text++;
    }
}

const unsigned char *Tower_Text(int id)
{
    if (id != TOWER_TEXT_ID || (D_8009B26C & 0x1F) != TOWER_MODE || !Tower_Config()) return NULL;
    return gTower.text + TEXT_START;
}

static uintptr_t text_address(const void *pointer)
{
#ifdef MEMORIES_TRANSLATED
    return GuestRuntime_EncodePointer((void *)pointer);
#else
    return (uintptr_t)pointer;
#endif
}

unsigned char *Tower_Retarget(const unsigned char *cursor)
{
    uintptr_t address = text_address(cursor), base = text_address(gTower.text);
    if (address >= base && address - base < TOWER_TEXT_SIZE) {
        gTower.text[0] = 0xFF;
        return gTower.text;
    }
    return NULL;
}

/* --- portraits -------------------------------------------------------------------- */

/* A 0x980-byte record (48 x 48 at 8 bits, then 64 colors) into cell
 * `cell` of the portrait grid, where the portrait effect draws it from:
 * x 0x380 + (cell % 5) * 24 words, y (cell / 5) * 48, the palette at
 * 0x380 + (cell / 16) * 64, 0xF0 + cell % 16 (Campaign_LoadScenePackage). */
static void upload(int cell, const unsigned char *record)
{
    RECT rect;
    rect.x = (s16)(0x380 + (cell % 5) * 24);
    rect.y = (s16)((cell / 5) * 48);
    rect.w = 24;
    rect.h = 48;
    LoadImage2(&rect, (u32 *)record);
    rect.x = (s16)(0x380 + (cell / 16) * 64);
    rect.y = (s16)(0xF0 + cell % 16);
    rect.w = 64;
    rect.h = 1;
    LoadImage2(&rect, (u32 *)(record + PORTRAIT_IMAGE));
    DrawSync(0);
}

/* Record `index` of the bank at WA sector `sector`, off the disc. */
static int disc_record(int sector, int index, unsigned char record[PORTRAIT_RECORD])
{
    static unsigned char sectors[3 * 2048];
    int offset = index * PORTRAIT_RECORD;
    if (!DiscArt_Read(sector + offset / 2048, sectors, (int)sizeof(sectors))) return 0;
    memcpy(record, sectors + offset % 2048, PORTRAIT_RECORD);
    return 1;
}

/* A duelist's own face (its Free Duel portrait: a mod's, else its base's,
 * off the disc) into `cell`; 0 when it has none. */
static int duelist_face(int duelist, int cell)
{
    unsigned char record[PORTRAIT_RECORD];
    if (duelist <= 0 || !Duelists_Valid(duelist)) return 0;
    if (Duelists_Portrait(duelist)) memcpy(record, Duelists_Portrait(duelist), PORTRAIT_RECORD);
    else if (!disc_record(FREE_DUEL_PORTRAITS_SECTOR, Duelists_BaseId(duelist), record)) return 0;
    upload(cell, record);
    return 1;
}

/* --- the boxes --------------------------------------------------------------------- */

static DuelEffectChannel *box(void) { return &D_800EB0F8[0]; }

static void close_box(void)
{
    if (box()->flags_34 & 0x8000) TextBox_Destroy((struct DuelEffectChannel *)box());
}

static void open_box(int x, int y, int width, int height, int flags)
{
    close_box();
    Tower_CreateBox(TOWER_TEXT_ID, x, y, width, height, BOX_WINDOW | flags);
}

/* Its text all shown, and, for one kept up, the button pressed. */
static int box_done(void)
{
    return (box()->flags_34 & TEXT_BOX_FLAG_DONE) && !(box()->flags_34 & BOX_KEPT);
}

/* --- what is told ------------------------------------------------------------------ */

static const TowerConfig *tower(void) { return Tower_Config(); }

static TowerStory story_of(int which, int floor)
{
    TowerStory none = {0, 0};
    const TowerConfig *config = tower();
    if (!config) return none;
    switch (which) {
    case STORY_INTRO: return config->intro;
    case STORY_ENDING: return config->ending;
    case STORY_LOSE: return config->lose;
    case STORY_BEFORE: return floor >= 0 && floor < config->floor_count ? config->floors[floor].before : none;
    case STORY_AFTER: return floor >= 0 && floor < config->floor_count ? config->floors[floor].after : none;
    }
    return none;
}

/* Tell story `which` (of floor `floor`), then go to step `then`. */
static void tell(int which, int floor, int then)
{
    gTower.story = (s16)which;
    gTower.floor = (s16)floor;
    gTower.line = 0;
    gTower.then = (s16)then;
    gTower.step = STEP_STORY;
    gTower.phase = 0;
}

/* --- the stage (tower.h, notes/tower-mode.md: Story) -------------------------------
 *
 * A story is told as the campaign tells a scene's dialogue: one text in its
 * stone-framed box (Script_OpShowDialog's place, no dark window), its
 * pages a button apart, over a scene picture (Tower_Scene), with the
 * campaign's large characters brought in, quieted and taken out by the
 * text's own codes, as the campaign's dialogue does it:
 *
 *   F6 id 1x       a character in on its side (bit 0: the right), its
 *                  expression in bits 1-2 (Text_HandleDisplayEffectCommand)
 *   F6 id 4e       its expression changed to e
 *   F6 id 20 / 21  speaking (its mouth moves with the words) / not
 *   F6 id 80       out
 *   F6 41+n 00/80  a small portrait in and out beside the box
 *   F7 05 id       another scene, crossfaded (func_8003767C, Script_OpShowImage)
 *   F7 0B x y n    the view panned (func_8003771C, Script_UpdateViewportTween)
 *
 * The scene's crossfade copies the screen to VRAM (448, 256)-(767, 415),
 * across the right side's character (640-831, 256-511), so a right
 * character goes out before another scene and comes back after it, as the
 * campaign's own scripts order it. One text, because a new box resets the
 * display-effect records (TextBox_BuildStep's func_80039E9C) and would
 * leave the characters' objects behind. */

typedef struct {
    int fx;                             /* the F6 id on the side, 0 none */
    int expression;
    int duelist;                        /* who it is, for a speaker: a duelist, TOWER_SPEAKER_FLOOR, 0 */
} Side;

static const u8 pool_ids[TOWER_POOL_SIZE] = TOWER_POOL_IDS;

/* The F6 id a mod's picture is told as this story: one of the characters
 * the disc keeps empty (gTower.pool). 0 when the story has five already. */
static int pool_character(int art)
{
    int k;
    const TowerConfig *config = tower();
    if (!config || art < 0 || art >= config->art_count || !config->arts[art].ready) return 0;
    for (k = 0; k < TOWER_POOL_SIZE; k++) {
        if (gTower.pool[k] == art + 1) return pool_ids[k];
    }
    for (k = 0; k < TOWER_POOL_SIZE; k++) {
        if (!gTower.pool[k]) {
            gTower.pool[k] = (u8)(art + 1);
            return pool_ids[k];
        }
    }
    LOG(LOG_MODS, "tower: a story shows at most %d pictures of the mod's; %s is left out", TOWER_POOL_SIZE,
        config->arts[art].path);
    return 0;
}

/* The F6 id a duelist's bust record is told as this story: its replaced
 * campaign character for the bust's first record (busts.h, replace_bust),
 * else one of the characters the disc keeps empty, as a tower's PNG. */
static int pool_bust(int bust, int record)
{
    int k, value = TOWER_POOL_BUST + record;
    const u16 *clut;
    const u8 *bank;
    if (Busts_Character(bust) && Busts_Replacing(Busts_Character(bust)) == record) return Busts_Character(bust);
    if (record < 0 || !Busts_Record(record, &clut, &bank) || value > 0xFF) return 0;
    for (k = 0; k < TOWER_POOL_SIZE; k++) {
        if (gTower.pool[k] == value) return pool_ids[k];
    }
    for (k = 0; k < TOWER_POOL_SIZE; k++) {
        if (!gTower.pool[k]) {
            gTower.pool[k] = (u8)value;
            return pool_ids[k];
        }
    }
    LOG(LOG_MODS, "tower: a story shows at most %d pictures of the mod's; a bust's is left out", TOWER_POOL_SIZE);
    return 0;
}

/* Duelist `duelist` on a side: its bust's expression (by the cast's name,
 * else by the codes' number), or the campaign's character it is shown as
 * -- a disc duelist whose bust only adds expressions, named without one
 * of them. */
static int duelist_character(int duelist, const TowerCast *cast, int *expression)
{
    int bust = Duelists_Bust(duelist), e, record, slot;
    *expression = cast->expression;
    if (bust < 0 || (!cast->expression_name[0] && duelist < DUELISTS_RETAIL_COUNT && !Duelists_ReplacesBust(duelist)))
        return Tower_DuelistCharacter(duelist);
    e = cast->expression_name[0] ? Busts_Named(bust, cast->expression_name) : Busts_Ordered(bust, cast->expression);
    if (e < 0) e = Busts_Default(bust);
    if (!Busts_Place(bust, e, &record, &slot)) {
        LOG(LOG_MODS, "tower: %s's bust was not made; the campaign's character is shown",
            Duelists_Name(duelist) ? Duelists_Name(duelist) : "a duelist");
        return Tower_DuelistCharacter(duelist);
    }
    *expression = slot;
    return pool_bust(bust, record);
}

/* What a cast stands for on the stage of floor `floor_duelist`'s story. */
static Side side_of(const TowerCast *cast, int floor_duelist)
{
    Side side = {0, 0, 0};
    side.expression = cast->expression;
    side.duelist = cast->duelist;
    switch (cast->kind) {
    case TOWER_CAST_RETAIL: side.fx = cast->id; break;
    case TOWER_CAST_ART: side.fx = pool_character(cast->id); break;
    case TOWER_CAST_DUELIST: side.fx = duelist_character(cast->duelist, cast, &side.expression); break;
    case TOWER_CAST_OPPONENT:
        side.fx = duelist_character(floor_duelist, cast, &side.expression);
        side.duelist = TOWER_SPEAKER_FLOOR;
        break;
    }
    if (!side.fx) side.duelist = 0;
    return side;
}

/* The scene a line asks for, one that can be shown: a mod's picture that
 * was not made is the tower's own scene, then the torch corridor. */
static int scene_shown(int scene)
{
    const TowerConfig *config = tower();
    const u16 *clut;
    if (scene >= TOWER_SCENE_ART && !Tower_ArtScene(scene - TOWER_SCENE_ART, &clut)) {
        scene = config->scene;
        if (scene >= TOWER_SCENE_ART && !Tower_ArtScene(scene - TOWER_SCENE_ART, &clut)) scene = TOWER_SCENE_DEFAULT;
    }
    return scene;
}

static int story_scene(TowerStory story)
{
    const TowerConfig *config = tower();
    int scene = story.count ? config->lines[story.first].scene : TOWER_SCENE_KEEP;
    return scene_shown(scene == TOWER_SCENE_KEEP ? config->scene : scene);
}

static void put16(Out *out, int value)
{
    put(out, value & 0xFF);
    put(out, (value >> 8) & 0xFF);
}

static void fx(Out *out, int id, int flags)
{
    put(out, 0xF6);
    put(out, id);
    put(out, flags);
}

/* The side a line's speaker stands on, -1 for none on the stage. */
static int speaking_side(int speaker, const Side sides[TOWER_SIDES], int floor_duelist)
{
    int s, want;
    if (speaker == TOWER_SPEAKER_LEFT) return sides[0].fx ? 0 : -1;
    if (speaker == TOWER_SPEAKER_RIGHT) return sides[1].fx ? 1 : -1;
    if (speaker == TOWER_SPEAKER_NONE) return -1;
    for (s = 0; s < TOWER_SIDES; s++) {
        if (sides[s].fx && sides[s].duelist == speaker) return s;
    }
    want = Tower_DuelistCharacter(speaker == TOWER_SPEAKER_FLOOR ? floor_duelist : speaker);
    for (s = 0; s < TOWER_SIDES; s++) {
        if (want && sides[s].fx == want) return s;
    }
    return -1;
}

/* The whole of story `which` in the game's codes, its small portraits put
 * in their cells (the campaign's 25 are there already, Tower_Scene; a
 * duelist's own face takes a cell from 24 down that the story's campaign
 * portraits leave free). */
static void compose_story(int which, TowerStory story, int floor_duelist)
{
    const TowerConfig *config = tower();
    Out out = begin(DIALOG_W - 2 * LETTER);
    Side sides[TOWER_SIDES] = {{0, 0, 0}, {0, 0, 0}};
    int scene = story_scene(story), i, s, face_cell = TOWER_CAMPAIGN_PORTRAITS - 1, faces[8][2], face_count = 0;
    char used[TOWER_CAMPAIGN_PORTRAITS];
    memset(used, 0, sizeof(used));
    for (i = 0; i < story.count; i++) {
        int portrait = config->lines[story.first + i].portrait;
        if (portrait >= 0 && portrait < TOWER_CAMPAIGN_PORTRAITS) used[portrait] = 1;
    }
    for (i = 0; i < story.count; i++) {
        const TowerLine *line = &config->lines[story.first + i];
        Side want[TOWER_SIDES];
        int talking, portrait = line->portrait, cell = -1, speaker_duelist;
        /* Room for this line at its most (two bytes a letter, a code a
         * row, its stage's codes) and the closing ones: else the story
         * ends here, whole (tower_config.c keeps a story under it). */
        if (out.end - out.at < (long)(2 * strlen(line->text) + strlen(line->text) / 16 + TOWER_LINE_CODES + 16)) {
            LOG(LOG_MODS, "tower: the box is full; the story ends before line %d", i + 1);
            break;
        }
        for (s = 0; s < TOWER_SIDES; s++) {
            TowerCast cast = line->cast[s];
            if (cast.kind == TOWER_CAST_KEEP && i == 0) {
                /* A floor's story has its opponent on the right unless it
                 * says otherwise. */
                cast.kind = (which == STORY_BEFORE || which == STORY_AFTER) && s == TOWER_SIDE_RIGHT
                                ? TOWER_CAST_OPPONENT : TOWER_CAST_NONE;
            }
            want[s] = cast.kind == TOWER_CAST_KEEP ? sides[s] : side_of(&cast, floor_duelist);
        }
        if (want[0].fx && want[0].fx == want[1].fx) {
            /* The game finds a character by its id: one of each a stage. */
            LOG(LOG_MODS, "tower: character %d is on the left already; the right is left empty", want[1].fx);
            want[1].fx = 0;
            want[1].duelist = 0;
        }
        /* Another scene: the right side out over its crossfade, in again after. */
        if (i > 0 && line->scene != TOWER_SCENE_KEEP && scene_shown(line->scene) != scene) {
            scene = scene_shown(line->scene);
            if (sides[1].fx) {
                fx(&out, sides[1].fx, 0x80);
                sides[1].fx = 0;
            }
            put(&out, 0xF7);
            put(&out, 5);
            put16(&out, scene);
        }
        /* Every side going out before any comes in: the game finds a
         * character by its id, so one moving across must leave first. */
        for (s = 0; s < TOWER_SIDES; s++) {
            if (want[s].fx != sides[s].fx && sides[s].fx) fx(&out, sides[s].fx, 0x80);
        }
        for (s = 0; s < TOWER_SIDES; s++) {
            if (want[s].fx != sides[s].fx) {
                if (want[s].fx) fx(&out, want[s].fx, 0x10 | (want[s].expression & 3) << 1 | s);
            } else if (want[s].fx && want[s].expression != sides[s].expression) {
                fx(&out, want[s].fx, 0x40 | (want[s].expression & 3));
            }
            sides[s] = want[s];
        }
        if (i == 0) {
            static const char *const names[] = {"", "intro", "before", "after", "ending", "lose"};
            LOG(LOG_MODS, "tower: the %s on scene 0x%03x, characters %d and %d", names[which], scene, sides[0].fx,
                sides[1].fx);
        }
        talking = speaking_side(line->speaker, sides, floor_duelist);
        for (s = 0; s < TOWER_SIDES; s++) {
            if (sides[s].fx) fx(&out, sides[s].fx, s == talking ? 0x20 : 0x21);
        }
        speaker_duelist = line->speaker == TOWER_SPEAKER_FLOOR ? floor_duelist : line->speaker > 0 ? line->speaker : 0;
        if (talking >= 0 && (line->speaker == TOWER_SPEAKER_LEFT || line->speaker == TOWER_SPEAKER_RIGHT))
            speaker_duelist = sides[talking].duelist == TOWER_SPEAKER_FLOOR ? floor_duelist :
                              sides[talking].duelist > 0 ? sides[talking].duelist : 0;
        if (portrait == TOWER_PORTRAIT_AUTO) portrait = talking < 0 && speaker_duelist ? TOWER_PORTRAIT_SPEAKER : -1;
        if (portrait >= 0 && portrait < TOWER_CAMPAIGN_PORTRAITS) {
            cell = portrait;
        } else if (portrait == TOWER_PORTRAIT_SPEAKER && speaker_duelist) {
            int k;
            for (k = 0; k < face_count && faces[k][0] != speaker_duelist; k++) {}
            if (k < face_count) {
                cell = faces[k][1];
            } else {
                while (face_cell >= 0 && used[face_cell]) face_cell--;
                if (face_cell < 0 || face_count == 8)
                    LOG(LOG_MODS, "tower: a story shows at most 8 duelists' own faces; %s's is left out",
                        Duelists_Name(speaker_duelist) ? Duelists_Name(speaker_duelist) : "a duelist");
                else if (duelist_face(speaker_duelist, face_cell)) {
                    faces[face_count][0] = speaker_duelist;
                    faces[face_count++][1] = face_cell;
                    cell = face_cell--;
                }
            }
        }
        if (cell >= 0) fx(&out, PORTRAIT_EFFECT + cell, 0);
        if (line->pan) {
            put(&out, 0xF7);
            put(&out, 0x0B);
            put16(&out, line->pan_x);
            put16(&out, line->pan_y + (line->pan_wait ? 0x1000 : 0));
            put16(&out, line->pan_frames);
        }
        out.x = 0;
        wrapped(&out, line->text, DIALOG_LINES);
        put(&out, 0xFA);
        if (cell >= 0) fx(&out, PORTRAIT_EFFECT + cell, 0x80);
    }
    for (s = 0; s < TOWER_SIDES; s++) {
        if (sides[s].fx) fx(&out, sides[s].fx, 0x80);
    }
    finish(&out);
}

static void step_story(void)
{
    const TowerStory story = story_of(gTower.story, gTower.floor);
    const TowerConfig *config = tower();
    int duelist = gTower.floor >= 0 && gTower.floor < config->floor_count ? config->floors[gTower.floor].duelist : 0;
    if (gTower.scene_on) Tower_SceneFrame();
    if (gTower.phase == 0) {
        if (!story.count) {
            gTower.step = gTower.then;
            return;
        }
        /* The stage up from black, the story's one box on it. */
        close_box();
        if (!gTower.faded) Fade_WaitOut();
        Tower_Scene(1, story_scene(story));
        gTower.dim = (s16)config->dim;
        compose_story(gTower.story, story, duelist);
        Tower_CreateBox(TOWER_TEXT_ID, DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, 0);
        Fade_WaitIn();
        gTower.faded = 0;
        gTower.phase = 1;
        return;
    }
    if (gTower.phase == 1) {
        if (!box_done()) return;
        gTower.phase = 2;
    }
    /* Its characters gone, the stage down again, to black: the next step
     * fades its own screen in (gTower.faded; a duel's own screen does, and
     * the wall is not brought back for it). */
    if (Tower_SceneBusy()) return;
    close_box();
    Fade_WaitOut();
    Tower_Scene(0, gTower.then != STEP_DUEL);
    gTower.faded = 1;
    gTower.line = (s16)story.count;
    gTower.step = gTower.then;
    gTower.phase = 0;
}

static unsigned deck_size(void)
{
    unsigned n = 0;
    int i;
    for (i = 0; i < TOWER_DECK; i++) n += gDuel_awPlayerDeck[i] != 0;
    return n;
}

/* The tower's name, the floor and who is on it, its rules: DUEL or LEAVE. */
static void show_status(void)
{
    const TowerConfig *config = tower();
    const TowerProgress *done = &gTower.progress;
    int floor = done->floor < config->floor_count ? done->floor : 0;
    const TowerFloor *one = &config->floors[floor];
    unsigned numbers[OWN_NUMBERS] = {0, 0};
    const char *names[OWN_NAMES] = {""};
    /* The heading's rows composed apart (each ends in FF), then laid under
     * the choice command (choices). */
    static u8 rows[3][160];
    const char *heading[3];
    int count = 0, i;
    Out out;
    for (i = 0; i < 3; i++) {
        Out row = {rows[i], rows[i] + sizeof(rows[i]) - 4, 0, MENU_W - 2 * LETTER};
        if (i == 0) {
            words(&row, config->name);
        } else if (i == 1) {
            numbers[0] = (unsigned)floor + 1;
            numbers[1] = (unsigned)config->floor_count;
            names[0] = Duelists_Name(one->duelist);
            own(&row, TEXT_OWN_TOWER_FLOOR, "FLOOR %d OF %d: %s", numbers, names);
        } else {
            int any = 0;
            if (one->rules & TOWER_RULE_NO_FUSIONS) {
                own(&row, TEXT_OWN_TOWER_NO_FUSIONS, "NO FUSIONS", NULL, NULL);
                any = 1;
            }
            if (one->rules & TOWER_RULE_NO_EQUIPS) {
                if (any) { glyph(&row, 0); glyph(&row, 0); }
                own(&row, TEXT_OWN_TOWER_NO_EQUIPS, "NO EQUIPS", NULL, NULL);
                any = 1;
            }
            if (one->life_points[0] || one->life_points[1]) {
                if (any) { glyph(&row, 0); glyph(&row, 0); }
                numbers[0] = one->life_points[0] ? (unsigned)one->life_points[0] : 8000;
                numbers[1] = one->life_points[1] ? (unsigned)one->life_points[1] : 8000;
                own(&row, TEXT_OWN_TOWER_LIFE_POINTS, "LP YOU %d COM %d", numbers, NULL);
                any = 1;
            }
            if (done->completions) {
                if (any) { glyph(&row, 0); glyph(&row, 0); }
                numbers[0] = done->completions;
                own(&row, TEXT_OWN_TOWER_CLEARED, "CLEARED x%d", numbers, NULL);
                any = 1;
            }
            if (!any) break;
        }
        *row.at = 0xFF;
        heading[count++] = (const char *)rows[i];
    }
    out = begin(MENU_W - 2 * LETTER);
    choices(&out, heading, NULL, count, TEXT_OWN_TOWER_DUEL, "DUEL", TEXT_OWN_TOWER_LEAVE, "LEAVE");
    finish(&out);
    open_box(MENU_X, 0x78 - ((count + 2) * 12 + 16) / 2, MENU_W, (count + 2) * 12 + 16, 0);
}

static void step_status(void)
{
    if (gTower.phase == 0) {
        show_status();
        gTower.phase = 1;
        return;
    }
    if (!box_done()) return;
    close_box();
    gTower.phase = 0;
    if (gDialog_bChoice != 0) {
        gTower.step = STEP_LEAVE;
        return;
    }
    if (deck_size() < TOWER_DECK) {
        gTower.step = STEP_NO_DECK;
        return;
    }
    tell(STORY_BEFORE, gTower.progress.floor, STEP_DUEL);
}

/* The Free Duel screen's own answer to a short deck (string 8), in its
 * own box. */
static void step_no_deck(void)
{
    if (gTower.phase == 0) {
        SD_SEPlayFull(SE_BUZZ);
        close_box();
        Tower_CreateBox(8, 0x30, 0x6C, 0xE0, 0x10, 0x1028);   /* as the Free Duel screen's */
        gTower.phase = 1;
        return;
    }
    if (!box_done()) return;
    close_box();
    gTower.step = STEP_STATUS;
    gTower.phase = 0;
}

/* What the duel just fought did: FLOOR N CLEARED, the reward, a
 * checkpoint; or the loss and the floor it goes back to. */
/* The next line of a notice: a page wait instead after every
 * NOTICE_LINES, so a long reward stays inside a box no taller than the
 * status box (the end-to-end test pages an eight-line notice). */
enum { NOTICE_LINES = 4 };
static void notice_line(Out *out, int *lines)
{
    if (*lines % NOTICE_LINES == 0) {
        put(out, 0xFA);
        out->x = 0;
    } else {
        newline(out);
    }
    ++*lines;
}

static void show_notice(void)
{
    const TowerConfig *config = tower();
    const TowerFloor *one = &config->floors[gTower.floor >= 0 && gTower.floor < config->floor_count ? gTower.floor : 0];
    unsigned numbers[OWN_NUMBERS] = {0, 0};
    const char *names[OWN_NAMES] = {""};
    int lines = 1, i;
    char name[128];
    Out out = begin(MENU_W - 2 * LETTER);
    if (gTower.outcome == OUTCOME_LOST) {
        numbers[0] = (unsigned)gTower.progress.floor + 1;
        own(&out, TEXT_OWN_TOWER_LOST, "YOU LOST. BACK TO FLOOR %d.", numbers, NULL);
    } else {
        numbers[0] = (unsigned)gTower.floor + 1;
        own(&out, TEXT_OWN_TOWER_FLOOR_WON, "FLOOR %d CLEARED!", numbers, NULL);
        /* The reward, the first time only (gTower.choice holds that). */
        if (gTower.choice) {
            for (i = 0; i < one->reward_card_count; i++) {
                notice_line(&out, &lines);
                codes(&out, Cards_NameCodes(one->reward_cards[i]));
            }
            if (one->reward_starchips) {
                notice_line(&out, &lines);
                numbers[0] = one->reward_starchips;
                own(&out, TEXT_OWN_TOWER_STARCHIPS, "%d STARCHIPS", numbers, NULL);
            }
            for (i = 0; i < one->reward_duelist_count; i++) {
                notice_line(&out, &lines);
                snprintf(name, sizeof(name), "%s", Duelists_Name(one->reward_duelists[i]));
                names[0] = name;
                own(&out, TEXT_OWN_TOWER_UNLOCKED, "%s UNLOCKED", NULL, names);
            }
        }
        if (gTower.outcome == OUTCOME_CHECKPOINT) {
            notice_line(&out, &lines);
            own(&out, TEXT_OWN_TOWER_CHECKPOINT, "CHECKPOINT", NULL, NULL);
        }
        if (gTower.outcome == OUTCOME_COMPLETED) {
            notice_line(&out, &lines);
            own(&out, TEXT_OWN_TOWER_COMPLETE, "THE TOWER IS CLEARED!", NULL, NULL);
        }
    }
    finish(&out);
    if (lines > NOTICE_LINES) lines = NOTICE_LINES;
    open_box(MENU_X, 0x78 - (lines * 12 + 16) / 2, MENU_W, lines * 12 + 16, BOX_KEPT);
}

static void step_notice(void)
{
    if (gTower.phase == 0) {
        show_notice();
        gTower.phase = 1;
        return;
    }
    if (!box_done()) return;
    close_box();
    gTower.phase = 0;
    if (gTower.outcome == OUTCOME_CHECKPOINT) gTower.step = STEP_SAVE_ASK;
    else if (gTower.outcome == OUTCOME_COMPLETED) tell(STORY_ENDING, -1, STEP_SAVE_ASK);   /* the top: a save too */
    else gTower.step = STEP_STATUS;
    gTower.outcome = OUTCOME_NONE;
}

static void step_save_ask(void)
{
    Out out;
    if (gTower.phase == 0) {
        static const char *const question[] = {"SAVE YOUR PROGRESS?"};
        static const int question_id[] = {TEXT_OWN_TOWER_SAVE};
        out = begin(MENU_W - 2 * LETTER);
        choices(&out, question, question_id, 1, TEXT_OWN_TOWER_YES, "YES", TEXT_OWN_TOWER_NO, "NO");
        finish(&out);
        open_box(MENU_X, 0x78 - 26, MENU_W, 3 * 12 + 16, 0);
        gTower.phase = 1;
        return;
    }
    if (!box_done()) return;
    close_box();
    gTower.phase = 0;
    gTower.step = gDialog_bChoice == 0 ? STEP_SAVE : STEP_STATUS;
}

/* The game's own save: the memory card dialog, as the credits ask for
 * theirs (Main_RunCredits), which on the PC is the slot menu. */
static void step_save(void)
{
    int answer;
    if (gTower.phase == 0) {
        SaveData_RequestWrite();
        gTower.phase = 1;
    }
    answer = MemCardDialog_Poll();
    if (answer == 0) return;
    if (answer == 2) {             /* again (the credits' retry) */
        gTower.phase = 0;
        return;
    }
    gTower.step = STEP_STATUS;
    gTower.phase = 0;
}

/* The floor's duel, armed as the Free Duel screen arms one
 * (screen_runtime.c): func_80024DC8 clears the field and the way back, and
 * sets the duel's mode; then the floor's field, and the way back here
 * rather than to the Free Duel screen, whose return flag 0x80 would count
 * the result on its grid (gFreeDuel_bReturnFlags is left alone). */
static void step_duel(void)
{
    const TowerConfig *config = tower();
    int floor = gTower.progress.floor < config->floor_count ? gTower.progress.floor : 0;
    const TowerFloor *one = &config->floors[floor];
    close_box();
    Tower_Background(0);
    SD_SEPlayFull(SE_CHOOSE);
    SD_BGMFadeOut();
    gTower.armed = (s16)(floor + 1);
    gTower.rules = (s16)one->rules;
    func_80024DC8(-1, one->duelist, 0x6000, 0x6000);
    /* An added field (7-255) is drawn on the retail board it borrows; the
       game's own code only knows terrains 0-6. */
    gDuel_bTerrain = (u8)Fields_Carrier(one->terrain);
    if (one->terrain > FIELDS_RETAIL) Fields_Start(one->terrain);
    D_8009B368 = TOWER_MODE;
    LOG(LOG_MODS, "tower: floor %d against %s (field %d, rules %d)", floor + 1, Duelists_Name(one->duelist),
        one->terrain, one->rules);
}

static void step_leave(void)
{
    const TowerConfig *config = tower();
    if (config && config->on_leave != TOWER_ON_LEAVE_RESUME) {
        /* "on_leave": the climb goes back to the checkpoint (or floor 1)
         * as the tower is left, so no save made outside keeps more. */
        Tower_Bank(config, &gTower.progress);
        LOG(LOG_MODS, "tower: left; next floor %u, checkpoint %u", (unsigned)gTower.progress.floor + 1,
            (unsigned)gTower.progress.checkpoint + 1);
    }
    Tower_Background(0);
    close_box();
    SD_SEPlayFull(8);
    SD_BGMFadeOut();
    D_8009B268 = 1;
    D_8009B26D = SECOND_MENU;
    D_8009B26C = MAIN_MODE_MENU;
}

/* The duel just come back from: the progress moves on, the reward is
 * given the first time, and what to tell is chosen. */
static void take_result(void)
{
    const TowerConfig *config = tower();
    int floor = gTower.armed - 1, won = D_8009B362 == 0, i;
    TowerOutcome outcome;
    gTower.armed = 0;
    gTower.rules = 0;
    if (floor < 0 || floor >= config->floor_count) return;
    outcome = Tower_ApplyResult(config, &gTower.progress, floor, won);
    gTower.floor = (s16)floor;
    gTower.choice = (s16)outcome.first_clear;   /* the notice lists the reward the first time */
    LOG(LOG_MODS, "tower: floor %d %s; next %u, checkpoint %u", floor + 1, won ? "won" : "lost",
        (unsigned)gTower.progress.floor, (unsigned)gTower.progress.checkpoint);
    if (!won) {
        gTower.outcome = OUTCOME_LOST;
        tell(STORY_LOSE, floor, STEP_NOTICE);
        return;
    }
    if (outcome.first_clear) {
        const TowerFloor *one = &config->floors[floor];
        SaveDataState *save = (SaveDataState *)gDuel_awPlayerDeck;
        for (i = 0; i < one->reward_card_count; i++) Duel_AwardCard(one->reward_cards[i]);
        if (one->reward_starchips) {
            unsigned cap = Tables_StarchipCap(), now = save->starchips;
            save->starchips = now + one->reward_starchips > cap ? cap : now + one->reward_starchips;
        }
        LOG(LOG_MODS, "tower: floor %d's reward: %d cards, %u starchips (now %u), %d duelists", floor + 1,
            one->reward_card_count, one->reward_starchips, (unsigned)save->starchips, one->reward_duelist_count);
    }
    /* The disc's duelists the floor unlocks, on every win: a flag set again
     * changes nothing, and a floor won before the mod named them still
     * gives them. */
    Tower_UnlockRetailDuelists(&config->floors[floor]);
    gTower.outcome = outcome.completed ? OUTCOME_COMPLETED : outcome.checkpoint ? OUTCOME_CHECKPOINT : OUTCOME_WON;
    tell(STORY_AFTER, floor, STEP_NOTICE);
}

void Tower_Enter(void)
{
    /* From the title: a climb left any other way than LEAVE (a jump, the
     * title's own reset) is banked as LEAVE banks it ("on_leave"). */
    Tower_Bank(tower(), &gTower.progress);
    gTower.step = STEP_BEGIN;
    gTower.phase = 0;
    gTower.armed = 0;
    gTower.outcome = OUTCOME_NONE;
    D_8009B26C = TOWER_MODE;
}

/* The step the log said last (the tests follow it): said again each time
 * the screen is entered, however it was left. */
static int logged_step = -1;

void Tower_RunMode(void)
{
    const TowerConfig *config = tower();
    if (!config) {
        /* A state of a run with the tower's mod, loaded without it. */
        step_leave();
        return;
    }
    if ((D_8009B26C & 0x40) == 0) {
        int song = config->music >= 0 ? config->music : TOWER_DEFAULT_SONG;
        D_8009B26C |= 0x40;
        logged_step = -1;
        Tower_Frame();   /* the save's progress is of this tower (tower.c) */
        SD_BGMPlay(0x7000 + (u32)song);
        gTower.step = STEP_BEGIN;
        gTower.phase = 0;
        if (gTower.armed) {
            take_result();
        } else if (config->intro.count && !(gTower.progress.flags & TOWER_PROGRESS_INTRO_SEEN)) {
            gTower.progress.flags |= TOWER_PROGRESS_INTRO_SEEN;
            tell(STORY_INTRO, -1, STEP_STATUS);
        } else {
            gTower.step = STEP_STATUS;
        }
        /* The screen comes from black (the menu and the duel fade out to
         * it): a story brings its stage up itself, anything else the wall. */
        gTower.faded = 1;
        if (gTower.step != STEP_STORY || !story_of(gTower.story, gTower.floor).count) {
            Tower_Background(1);
            Fade_WaitIn();
            gTower.faded = 0;
        }
        return;
    }
    {   /* For the log (MEMORIES_TRACE=mods), which the tests follow. */
        static const char *const names[] = {"begin", "story", "notice", "status", "save?", "save", "duel", "leave",
                                            "no deck"};
        int line = gTower.step == STEP_STORY ? gTower.story * 100 + gTower.line : 0;
        if (gTower.step * 10000 + line != logged_step && gTower.step >= 0 && gTower.step <= STEP_NO_DECK) {
            logged_step = gTower.step * 10000 + line;
            LOG(LOG_MODS, "tower: %s (floor %u, checkpoint %u%s)", names[gTower.step],
                (unsigned)gTower.progress.floor + 1, (unsigned)gTower.progress.checkpoint + 1,
                gTower.step == STEP_STORY ? ", a line" : "");
        }
    }
    switch (gTower.step) {
    case STEP_STORY: step_story(); break;
    case STEP_NOTICE: step_notice(); break;
    case STEP_STATUS: step_status(); break;
    case STEP_NO_DECK: step_no_deck(); break;
    case STEP_SAVE_ASK: step_save_ask(); break;
    case STEP_SAVE: step_save(); break;
    case STEP_DUEL: step_duel(); return;
    case STEP_LEAVE: step_leave(); return;
    default: gTower.step = STEP_STATUS; gTower.phase = 0; break;
    }
    func_80039794();
    /* After a story: the wall is up again (Tower_Scene), its box begun. */
    if (gTower.faded && gTower.step != STEP_STORY && gTower.step != STEP_DUEL && gTower.step != STEP_LEAVE) {
        Fade_WaitIn();
        gTower.faded = 0;
    }
}
