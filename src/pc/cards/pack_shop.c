/* The card packs on the Password screen (pack_shop.h, notes/card-packs.md).
 *
 * States past the game's five, in the same machine (D_8016D424's low bits):
 *
 *   5  LIST     the pack in the digits' panel between ◄ ►, its picture on the
 *               big card, its lines in the message box; ←/→ packs, ↑/↓
 *               shops, ✕ buy, □ details, ○ back (or out, with packs_only)
 *   6  CONFIRM  BUY / QUIT, as the game asks EXCHANGE / QUIT for a card
 *   7  PAY      the starchips count down, as a password's price does
 *   8  REVEAL   each card turned over on the big card
 *   9  SUMMARY  the cards of the pack, three to a page
 *   10 INFO     the pack's details, three lines to a page
 *   11 LEAVE    back to the digits
 *
 * The game's own updater runs a state only once the message box is done,
 * which is also how CONFIRM waits for the answer.
 *
 * Nothing here touches the game's random numbers but a purchase, which
 * spends exactly four a card (packs.h). */
#define MAIN_MODE_STATE_NEXT_AS_SCALAR
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "pack_shop.h"
#include "packs.h"
#include "cards.h"
#include "passwords.h"
#include "tables.h"
#include "art.h"
#include "pc/free_duel/duelists.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "pc/guest/state.h"
#include "pc/saves/save_menu.h"
#include "pc/saves/save_slots.h"
#include "pc/platform/paths.h"
#include "pc/render/texture_pack.h"
#include "pc/debug/cheats.h"
#include "pc/debug/log.h"
#include "pc/rng.h"
#include "pc/mods/mods.h"
#include "pc/compat/fs.h"
#include "types.h"
#include "ygo_types.h"
#include "game/input.h"
#include "game/campaign_flags.h"
#include "game/display_object.h"
#include "game/display_object_layout.h"
#include "game/file_transfer.h"
#include "game/duel_rewards.h"
#include "game/duel_effect.h"
#include "game/duel_card.h"
#include "game/func_80039794.h"
#include "game/text_box_lifecycle.h"
#include "game/text_box_runtime.h"
#include "game/dialog_choice.h"
#include "game/sound.h"
#include "game/fade.h"
#include "game/save_data.h"
#include "game/duel_effect_resource_record.h"
#include "game/duel_effect_resource_setup.h"
#include "game/main_mode_state.h"
#include "overlays/password/shop.h"
#include "overlays/password/module_state.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 Text_LookupString(s32 bank, s32 id);

#define PASSWORD_MODE 10
#define MESSAGE_ID 226               /* "Please enter 'PASSWORD'  ✕OK ○END" */
enum {
    STATE_LIST = 5, STATE_CONFIRM, STATE_PAY, STATE_REVEAL, STATE_SUMMARY, STATE_INFO, STATE_LEAVE
};
/* The screen's own strings, by id (Text_Resolve): past the port's composed
 * ones (text.h), clear of everything the disc has. */
#define TEXT_NAME_ID 0xFFF0
#define TEXT_MESSAGE_ID 0xFFF1
#define TEXT_CONFIRM_ID 0xFFF2

#define SAVE_DUELIST_CODE 0x334
#define BOX_LETTERS 20                /* the message box: 0xA0 wide, 8 a letter */
#define BOX_WIDTH 0xA0
#define NAME_WIDTH 0x80               /* between the digit cursor's arrows */
#define ICON_WIDTH 16
#define FLIP_STEP 8                   /* the game's own turn, state 1 */
#define QUICK_STEP 16
#define QUICK_WAIT 30                 /* frames a quick reveal shows a card */
#define LINES_A_PAGE 3
#define NOTE_MARGIN 4                 /* ALL OWNED's room from the box's right frame */

enum { ICON_CROSS = 0x22, ICON_TRIANGLE = 0x23, ICON_SQUARE = 0x24, ICON_CIRCLE = 0x25, ICON_STAR = 0x26 };
enum { WHITE = 0, GOLD = 1, BLUE = 2, GREY = 4 };

/* --- what the screen shows, kept in a save state --------------------------- */

#define ARENA_SIZE 4096
enum { TEXT_NAME, TEXT_MESSAGE, TEXT_CONFIRM, TEXT_HINT, TEXTS };
static const unsigned text_at[TEXTS] = {16, 272, 2048, 2816};
static const unsigned text_room[TEXTS] = {256, 1776, 768, 1280};

enum { CARD_IDLE, CARD_HIDING, CARD_LOADING, CARD_WAITING, CARD_SHOWING };

#define STATE_VERSION 1
typedef struct {
    u32 version;
    s32 open;                 /* the packs' states are on screen */
    s32 pack;                 /* Packs_At: the pack shown, or bought */
    s32 shop;
    s32 from_password;        /* bought by its password: back to the digits after */
    s32 step;                 /* a state's own progress */
    s32 wait;
    s32 card_phase, card_goal, card_cover, card_pack_art, card_speed, card_sound;
    s32 reveal;               /* the card turning over */
    s32 page;
    u32 price_left;
    PackResult result;
    u8 fresh[PACK_COUNT_MAX];
    u32 owner;                /* the duelist code the progress is a save of */
    u32 owner_set;
    u32 token;                /* the slot token it was read from, or written under */
    PacksProgress progress;
    u8 arena[ARENA_SIZE];     /* arena[0] is the {end} a menu's answer jumps to */
} Screen;
static Screen s;

static int composing;         /* reading string 226 for the hint: the game's own */
static int hint_ready;

/* --- small helpers ------------------------------------------------------------ */

static const PackShopRules *rules(void) { return Packs_Rules(); }

int PackShop_Available(void)
{
    return Packs_Count() > 0;
}

static int on_screen(void)
{
    return PackShop_Available() && (D_8009B26C & 0x1F) == PASSWORD_MODE;
}

static u32 running_code(void)
{
    u32 code;
    memcpy(&code, (const u8 *)gDuel_awPlayerDeck + SAVE_DUELIST_CODE, sizeof(code));
    return code;
}

/* The progress belongs to the running save: NEW GAME, which writes a new
 * duelist code and loads nothing, starts from none. */
static void own_progress(void)
{
    u32 code = running_code();
    if (!s.owner_set || s.owner != code) {
        Packs_ForgetProgress(&s.progress);
        s.owner = code;
        s.owner_set = 1;
        s.token = 0;
    }
}

static int held(int card, void *context)
{
    int count = *Cards_ChestSlot(gDuel_awPlayerDeck, card), i;
    (void)context;
    for (i = 0; i < DECK_SIZE; i++) count += gDuel_awPlayerDeck[i] == card;
    return count;
}

/* Copies of a card the chest still takes: Duel_AwardCard keeps none past
 * Tables_ChestLimit (the disc's 250, or a mod's "chest_overflow" limit). */
static int chest_room(int card, void *context)
{
    int room = Tables_ChestLimit() - *Cards_ChestSlot(gDuel_awPlayerDeck, card);
    (void)context;
    return room > 0 ? room : 0;
}

static int save_condition(const PackUnlock *unlock, void *context)
{
    (void)context;
    return Duelists_ConditionsMet(gDuel_awPlayerDeck, unlock->beat, unlock->wins, unlock->story, unlock->card,
                                  unlock->copies);
}

static int unlocked(int pack) { return Packs_Unlocked(pack, &s.progress, save_condition, NULL); }

static int shop_open(int shop)
{
    const PackShop *one = shop >= 0 && shop < rules()->shop_count ? &rules()->shops[shop] : NULL;
    return one && (!one->has_unlock || Packs_UnlockMet(&one->unlock, &s.progress, save_condition, NULL));
}

/* Whether the list shows pack `pack` in shop `shop`. */
static int listed(int pack, int shop)
{
    const Pack *one = Packs_At(pack);
    if (!one || !one->listed || !(one->shops & (1u << shop))) return 0;
    return one->locked_shown || unlocked(pack);
}

static int listed_count(int shop, int *place, int pack)
{
    int i, n = 0;
    for (i = 0; i < Packs_Count(); i++) {
        if (!listed(i, shop)) continue;
        if (i == pack && place) *place = n;
        n++;
    }
    return n;
}

static int next_listed(int shop, int from, int step)
{
    int count = Packs_Count(), i, at = from;
    for (i = 0; i < count; i++) {
        at = (at + step + count) % count;
        if (listed(at, shop)) return at;
    }
    return listed(from, shop) ? from : -1;
}

static int shops_open(void)
{
    int i, n = 0;
    for (i = 0; i < rules()->shop_count; i++) n += shop_open(i) && listed_count(i, NULL, -1) > 0;
    return n;
}

static int next_shop(int from, int step)
{
    int count = rules()->shop_count, i, at = from;
    for (i = 0; i < count; i++) {
        at = (at + step + count) % count;
        if (shop_open(at) && listed_count(at, NULL, -1) > 0) return at;
    }
    return -1;
}

/* The first Magic card: the frame the pack is drawn in, which has no ATK or
 * DEF to show. A mod may have made any card something else, so it is looked
 * for rather than named. */
static int support_card(void)
{
    static int found = -1;
    int id;
    if (found >= 0) return found;
    found = 0;
    for (id = CARD_ID_FIRST; id <= CARD_COUNT; id++) {
        if (((gDuel_adwCardStats[id - 1] >> 26) & 0x1F) == 0x14) {
            found = id;
            break;
        }
    }
    return found;
}

/* --- the packs' names and pictures, in the game's codes ----------------------- */

static unsigned char *name_codes[PACKS_MAX], *description_codes[PACKS_MAX], *shop_codes[PACK_SHOPS_MAX];
static unsigned char *art_records[PACKS_MAX], *plates[PACKS_MAX];
static int art_built;

/* UTF-8 into the game's codes: glyph 0 is the space; a letter the game and
 * the mods' fonts lack is left out. Ends in FF. */
static unsigned char *encode(const char *text)
{
    size_t length = strlen(text), n = 0;
    unsigned char *codes = malloc(length * 2 + 2);
    if (!codes) return NULL;
    while (*text) {
        uint32_t character = Glyphs_NextCharacter(&text);
        int code;
        if (character == GLYPHS_NOT_UTF8) continue;
        if (character == '\n') { codes[n++] = 0xFE; continue; }
        code = character == ' ' ? 0 : Glyphs_Code(character);
        if (code < 0) continue;
        if (code >= 0xF0) codes[n++] = (unsigned char)(0xF0 + (code >> 8));
        codes[n++] = (unsigned char)code;
    }
    codes[n] = 0xFF;
    return codes;
}

static const unsigned char *pack_name(int pack)
{
    static const unsigned char none[] = {0xFF};
    if (pack < 0 || pack >= PACKS_MAX || !Packs_At(pack)) return none;
    if (!name_codes[pack]) name_codes[pack] = encode(Packs_At(pack)->name);
    return name_codes[pack] ? name_codes[pack] : none;
}

static const unsigned char *pack_description(int pack)
{
    static const unsigned char none[] = {0xFF};
    if (pack < 0 || pack >= PACKS_MAX || !Packs_At(pack)) return none;
    if (!description_codes[pack]) description_codes[pack] = encode(Packs_At(pack)->description);
    return description_codes[pack] ? description_codes[pack] : none;
}

static const unsigned char *shop_name(int shop)
{
    static const unsigned char none[] = {0xFF};
    if (shop < 0 || shop >= rules()->shop_count) return none;
    if (!shop_codes[shop]) shop_codes[shop] = encode(rules()->shops[shop].name);
    return shop_codes[shop] ? shop_codes[shop] : none;
}

/* A picture larger than the console's is drawn from the PNG itself above
 * the console's resolution, as a mod card's art is (cards.c). */
static void add_full_picture(const char *path, const unsigned char *record)
{
    int x, y, cw, ch, width, height;
    if (!CardArt_Crop(path, CARD_ART_WIDTH, CARD_ART_HEIGHT, &x, &y, &cw, &ch, &width, &height) ||
        (cw <= CARD_ART_WIDTH && ch <= CARD_ART_HEIGHT))
        return;
    TexturePack_AddMade(record + CARD_ART_PIXELS, CARD_ART_WIDTH / 2, CARD_ART_HEIGHT, 8, record + CARD_ART_CLUT, 256,
                        path, x, y, cw, ch);
}

/* Each pack's picture: its "image" made as a card's art (102x96 of 256
 * colours), with its name on the title plate as a mod card's is; without
 * one, the plate alone, over its cover card's art. */
static void build_art(void)
{
    int i;
    if (art_built) return;
    art_built = 1;
    for (i = 0; i < Packs_Count() && i < PACKS_MAX; i++) {
        const Pack *pack = Packs_At(i);
        char why[PACK_PATH_MAX + 64];   /* the reason comes after the whole path */
        if (pack->image[0]) {
            unsigned char *record = calloc(1, CARD_ART_RECORD);
            why[0] = '\0';
            if (record && CardArt_FromImage(pack->image, record, why, sizeof(why))) {
                CardArt_TitleFromName(pack->name, record + CARD_TITLE_PIXELS);
                add_full_picture(pack->image, record);
                art_records[i] = record;
            } else {
                free(record);
                Mods_Note(pack->mod, "pack \"%s\": its cover is shown, as its \"image\" cannot be used: %s", pack->id,
                          why[0] ? why : "out of memory");
            }
        }
        if (!art_records[i]) {
            unsigned char *plate = malloc(CARD_TITLE_BYTES);
            if (plate && CardArt_TitleFromName(pack->name, plate)) plates[i] = plate;
            else free(plate);
        }
    }
}

/* --- composing the game's text --------------------------------------------------- */

typedef struct {
    u8 *at, *end;
    int x;                    /* pixels into the line */
    int limit;                /* the line's width: what would go past it is left out */
} Out;

static void put(Out *out, int byte)
{
    if (out->at < out->end) *out->at++ = (u8)byte;
}

static void command(Out *out, int op, int operand)
{
    put(out, 0xF8);
    put(out, op);
    put(out, operand);
}

static void newline(Out *out)
{
    put(out, 0xFE);
    out->x = 0;
}

static void colour(Out *out, int ink) { command(out, 0x0A, ink); }

static void at_x(Out *out, int x)
{
    if (x < 0) x = 0;
    put(out, 0xF8);
    put(out, 0x06);
    put(out, x & 0xFF);
    put(out, (x >> 8) & 0xFF);
    out->x = x;
}

static void space(Out *out)
{
    if (out->x + 8 > out->limit) return;
    command(out, 0x02, 8);
    out->x += 8;
}

static void icon(Out *out, int which)
{
    if (out->x + ICON_WIDTH > out->limit) return;
    command(out, 0x0B, which);
    out->x += ICON_WIDTH;
}

/* A letter; one that would run past the line's end is left out, since the
 * box would wrap it onto a line of its own. */
static void glyph(Out *out, int code)
{
    if (code < 0 || out->end - out->at < 3) return;
    if (code == 0) { space(out); return; }
    if (out->x + 8 > out->limit) return;
    if (code >= 0xF0) put(out, 0xF0 + (code >> 8));
    put(out, code & 0xFF);
    out->x += 8;
}

/* ASCII in the game's letters. */
static void words(Out *out, const char *text)
{
    for (; *text; text++) glyph(out, *text == ' ' ? 0 : Glyphs_Code((unsigned char)*text));
}

static void number(Out *out, unsigned value)
{
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", value);
    words(out, digits);
}

/* Letters of codes up to a line break or the end. */
static int letters(const unsigned char *codes)
{
    int n = 0;
    while (codes && *codes < 0xF6 && *codes != 0xFE) {
        codes += *codes >= 0xF0 ? 2 : 1;
        n++;
    }
    return n;
}

/* At most `room` letters of codes (to a line break), "..." ending a cut. */
static void codes(Out *out, const unsigned char *text, int room)
{
    int length = letters(text), keep = length, i;
    if (length > room) keep = room > 3 ? room - 3 : 0;
    for (i = 0; i < keep; i++) {
        if (*text >= 0xF0) {
            glyph(out, ((text[0] - 0xF0) << 8) | text[1]);
            text += 2;
        } else {
            glyph(out, *text++);
        }
    }
    if (keep < length && room > 3) words(out, "...");
}

/* Whether a translation's string is letters and spaces alone, which is all
 * these lines take. */
static int plain(const u8 *text)
{
    for (; *text != 0xFF; text++) {
        if (*text >= 0xF6) return 0;
        if (*text >= 0xF0 && *++text == 0xFF) return 0;
    }
    return 1;
}

/* One of the port's own strings (text.h): a translation's, else `english`;
 * each %d the next number, each %s the next codes. Returns its width in
 * pixels; written to `out` unless it is NULL. */
/* One pass of own(): each %s given `room` letters. */
static int own_pass(Out *to, int id, const char *english, const unsigned *numbers,
                    const unsigned char *const *names, int room)
{
    const u8 *text = Text_Own(id);
    int percent = Glyphs_Code('%'), letter_d = Glyphs_Code('d'), letter_s = Glyphs_Code('s');
    int used_numbers = 0, used_names = 0, start = to->x;
    if (text && !plain(text)) {
        static unsigned char said[TEXT_OWN_LAST - TEXT_OWN_FIRST + 1];
        if (!said[id - TEXT_OWN_FIRST])
            LOG(LOG_MODS, "text: string %04X has codes other than letters; the port's own is used", id);
        said[id - TEXT_OWN_FIRST] = 1;
        text = NULL;
    }
    if (!text) {
        const char *c;
        for (c = english; *c; c++) {
            if (c[0] == '%' && c[1] == 'd') { number(to, numbers ? numbers[used_numbers++] : 0); c++; }
            else if (c[0] == '%' && c[1] == 's') { codes(to, names ? names[used_names++] : NULL, room); c++; }
            else glyph(to, *c == ' ' ? 0 : Glyphs_Code((unsigned char)*c));
        }
        return to->x - start;
    }
    while (*text != 0xFF) {
        int code = *text++;
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *text++;
        if (code == percent && *text == letter_d) {
            text++;
            number(to, numbers ? numbers[used_numbers++] : 0);
        } else if (code == percent && *text == letter_s) {
            text++;
            codes(to, names ? names[used_names++] : NULL, room);
        } else {
            glyph(to, code);
        }
    }
    return to->x - start;
}

/* One of the port's own strings (text.h): a translation's, else `english`;
 * each %d the next number, each %s the next codes, cut ("...") to what the
 * line has room for past the rest. Returns its width in pixels; written to
 * `out` unless it is NULL. */
static int own(Out *out, int id, const char *english, const unsigned *numbers, const unsigned char *const *names)
{
    Out scratch;
    u8 dummy[512];
    int fixed, room, x = out ? out->x : 0;
    scratch.at = dummy;
    scratch.end = dummy + sizeof(dummy);
    scratch.x = 0;
    scratch.limit = 0x7FFF;
    fixed = own_pass(&scratch, id, english, numbers, names, 0);
    room = ((out ? out->limit : BOX_WIDTH) - x - fixed) / 8;
    if (room < 4) room = 4;
    if (!out) {
        scratch.at = dummy;
        scratch.x = 0;
        return own_pass(&scratch, id, english, numbers, names, room);
    }
    return own_pass(out, id, english, numbers, names, room);
}

static Out begin(int which)
{
    Out out;
    /* Where a menu's answer jumps (PackShop_Retarget): an {end}. */
    memset(s.arena, 0xFF, text_at[TEXT_NAME]);
    out.at = s.arena + text_at[which];
    out.end = s.arena + text_at[which] + text_room[which] - 8;
    out.x = 0;
    out.limit = which == TEXT_NAME ? NAME_WIDTH : BOX_WIDTH;
    return out;
}

static void finish(Out *out)
{
    if (out->at > out->end + 7) out->at = out->end + 7;
    *out->at = 0xFF;
}

/* ✕WORD ○WORD □WORD, as the retail "✕OK ○END" is: an icon and its word. */
static void hint(Out *out, int which, int id, const char *english)
{
    if (out->x) space(out);
    icon(out, which);
    own(out, id, english, NULL, NULL);
}

static int hint_width(int id, const char *english)
{
    return ICON_WIDTH + own(NULL, id, english, NULL, NULL);
}

/* A right-aligned "a/b" at the line's end. */
static void count_at_right(Out *out, unsigned a, unsigned b)
{
    char text[24];
    snprintf(text, sizeof(text), "%u/%u", a, b);
    at_x(out, BOX_WIDTH - 8 * (int)strlen(text));
    words(out, text);
}

/* "5 CARDS", or "1 CARD". */
static void cards_words(Out *out, unsigned count)
{
    if (count == 1) own(out, TEXT_OWN_PACK_CARD, "%d CARD", &count, NULL);
    else own(out, TEXT_OWN_PACK_CARDS, "%d CARDS", &count, NULL);
}

static void price_line(Out *out, int pack)
{
    const Pack *one = Packs_At(pack);
    icon(out, ICON_STAR);
    words(out, "x");
    number(out, one->price);
    if (one->cost_cards) {
        unsigned copies = 0;
        int i;
        for (i = 0; i < one->cost_cards; i++) copies += one->cost_copies[i];
        words(out, " +");
        cards_words(out, copies);
    }
}

/* --- string 226 with △PACKS ---------------------------------------------------------- */

/* The length of a text code at `text` a line of plain words may hold, or 0. */
static int plain_code(const u8 *text)
{
    if (*text == 0xF8) {
        if (text[1] == 0x06) return 4;
        return text[1] == 0x01 || text[1] == 0x02 || text[1] == 0x04 || text[1] == 0x0A || text[1] == 0x0B ? 3 : 0;
    }
    if (*text == 0xFE) return 1;
    if (*text >= 0xF6) return 0;
    return *text >= 0xF0 ? 2 : 1;
}

/* The width of a line of string 226 from `text` to its end or break. */
static int line_width(const u8 *text, const u8 **indent_end, int *indent)
{
    int width = 0, lead = 1;
    if (indent) *indent = 0;
    while (*text != 0xFF && *text != 0xFE) {
        int w;
        if (*text == 0xF8 && text[1] == 0x02) w = text[2];
        else if (*text == 0xF8 && text[1] == 0x0B) w = ICON_WIDTH;
        else if (*text == 0xF8) w = 0;
        else w = 8;
        if (lead && (*text == 0 || (*text == 0xF8 && text[1] == 0x02))) {
            if (indent) *indent += w;
        } else {
            if (lead && indent_end) *indent_end = text;
            lead = 0;
            width += w;
        }
        text += plain_code(text);
    }
    if (lead && indent_end) *indent_end = text;
    return width;
}

/* The message the screen opens with, and returns to, with "△PACKS" after
 * "✕OK ○END": the game's string 226 (or a translation's) with the icon and
 * a word added to its last line, or to the blank line above it when the
 * last has no room. A 226 of more than plain words keeps its own text. */
static void compose_hint(void)
{
    const u8 *text, *p;
    const u8 *lines[8];
    int count = 0, i, extra, last;
    Out out = begin(TEXT_HINT);
    hint_ready = 0;
    composing = 1;
    text = (const u8 *)(uintptr_t)Text_LookupString(0, MESSAGE_ID);
    composing = 0;
    if (!text) return;
    for (p = text; *p != 0xFF; ) {
        int length = plain_code(p), k;
        /* A code cut short by the string's end: the walks below would step
           past the end marker. */
        for (k = 1; k < length; k++) if (p[k] == 0xFF) length = 0;
        if (!length) {
            LOG(LOG_MODS, "packs: string 226 has a code other than plain words (%02X %02X %02X at %d); the Password "
                          "screen does not say \"PACKS\" (the triangle still opens them)", p[0], p[1], p[2],
                (int)(p - text));
            return;
        }
        p += length;
        if (p - text > 400) return;
    }
    lines[count++] = text;
    for (p = text; *p != 0xFF && count < 8; p += plain_code(p)) {
        if (*p == 0xFE) lines[count++] = p + 1;
    }
    extra = 8 + hint_width(TEXT_OWN_PACK_PACKS, "PACKS");
    last = count - 1;
    for (i = 0; i < count; i++) {
        const u8 *from = lines[i], *start = from;
        int indent, width = line_width(from, &start, &indent);
        if (i) newline(&out);
        if (i == last && width + extra <= BOX_WIDTH) {
            int room = BOX_WIDTH - width - extra;
            at_x(&out, indent < room ? indent : room);
            for (p = start; *p != 0xFF && *p != 0xFE; p += plain_code(p)) {
                int k, length = plain_code(p);
                for (k = 0; k < length; k++) put(&out, p[k]);
            }
            space(&out);
            icon(&out, ICON_TRIANGLE);
            own(&out, TEXT_OWN_PACK_PACKS, "PACKS", NULL, NULL);
            continue;
        }
        if (i == last - 1 && width == 0 && line_width(lines[last], NULL, NULL) + extra > BOX_WIDTH) {
            int last_indent;
            line_width(lines[last], NULL, &last_indent);
            at_x(&out, last_indent);
            icon(&out, ICON_TRIANGLE);
            own(&out, TEXT_OWN_PACK_PACKS, "PACKS", NULL, NULL);
            continue;
        }
        for (p = from; *p != 0xFF && *p != 0xFE; p += plain_code(p)) {
            int k, length = plain_code(p);
            for (k = 0; k < length; k++) put(&out, p[k]);
        }
    }
    finish(&out);
    hint_ready = 1;
}

const unsigned char *PackShop_Text(int id)
{
    if (id != MESSAGE_ID && (id < TEXT_NAME_ID || id > TEXT_CONFIRM_ID)) return NULL;
    if (composing || !on_screen()) return NULL;
    if (id == MESSAGE_ID) {
        /* Composed again each time it is asked, so a language changed since
           shows; a box still showing it has the same bytes written over
           themselves. */
        if (rules()->password != PACK_SHOP_BOTH) return NULL;
        compose_hint();
        return hint_ready ? s.arena + text_at[TEXT_HINT] : NULL;
    }
    return s.arena + text_at[id == TEXT_NAME_ID ? TEXT_NAME : id == TEXT_MESSAGE_ID ? TEXT_MESSAGE : TEXT_CONFIRM];
}

unsigned char *PackShop_Retarget(const unsigned char *cursor)
{
    if (cursor >= s.arena && cursor < s.arena + ARENA_SIZE) {
        s.arena[0] = 0xFF;
        return s.arena;
    }
    return NULL;
}

/* --- the lines of the message box ------------------------------------------------------- */

/* Up to `max` lines of `text` wrapped at 20 letters, at spaces, into
 * `lines` (starts) and `lengths` (letters). */
static int wrap(const unsigned char *text, const unsigned char **lines, int *lengths, int max)
{
    int n = 0;
    while (*text != 0xFF && n < max) {
        const unsigned char *start = text, *cut = NULL, *p = text;
        int count = 0, cut_count = 0;
        while (*p != 0xFF && *p != 0xFE && count < BOX_LETTERS) {
            if (*p == 0) { cut = p; cut_count = count; }
            p += *p >= 0xF0 ? 2 : 1;
            count++;
        }
        if (*p != 0xFF && *p != 0xFE && cut && *p != 0) { p = cut; count = cut_count; }
        lines[n] = start;
        lengths[n++] = count;
        text = p;
        while (*text == 0 || *text == 0xFE) {
            int was_break = *text == 0xFE;
            text++;
            if (was_break) break;
        }
    }
    return n;
}

static void put_letters(Out *out, const unsigned char *text, int count)
{
    int i;
    for (i = 0; i < count && *text != 0xFF && *text != 0xFE; i++) {
        if (*text >= 0xF0) {
            glyph(out, ((text[0] - 0xF0) << 8) | text[1]);
            text += 2;
        } else {
            glyph(out, *text++);
        }
    }
}

/* What an unmet condition says, a line each (up to `max`). */
static int condition_lines(const PackUnlock *unlock, Out *out, int max, int first)
{
    int n = 0, k;
    const void *save = gDuel_awPlayerDeck;
    unsigned numbers[2];
    const unsigned char *names[1];
    static const unsigned char nameless[] = {0xFF};
    #define LINE() do { if (n || !first) newline(out); n++; } while (0)
    if (unlock->never && n < max) {
        LINE();
        own(out, TEXT_OWN_PACK_LOCKED, "LOCKED", NULL, NULL);
        return n;
    }
    if (unlock->story >= 0 && Campaign_TestStoryFlag(unlock->story) == 0 && n < max) {
        LINE();
        own(out, TEXT_OWN_PACK_STORY, "GO ON IN THE STORY", NULL, NULL);
    }
    if (unlock->beat[0] && !Duelists_ConditionsMet(save, unlock->beat, unlock->wins, -1, "", 0) && n < max) {
        int who = Duelists_Named(unlock->beat);
        unsigned char *encoded = encode(who >= 0 ? Duelists_Name(who) : "???");
        LINE();
        names[0] = encoded;
        numbers[0] = (unsigned)(unlock->wins > 1 ? unlock->wins : 1);
        if (unlock->wins > 1) own(out, TEXT_OWN_PACK_BEAT_TIMES, "BEAT %s %d TIMES", numbers, names);
        else own(out, TEXT_OWN_PACK_BEAT, "BEAT %s", numbers, names);
        free(encoded);
    } else if (!unlock->beat[0] && unlock->wins > 0 && !Duelists_ConditionsMet(save, "", unlock->wins, -1, "", 0) &&
               n < max) {
        LINE();
        numbers[0] = (unsigned)unlock->wins;
        own(out, TEXT_OWN_PACK_WINS, "WIN %d DUELS", numbers, NULL);
    }
    if (unlock->card[0] && !Duelists_ConditionsMet(save, "", 0, -1, unlock->card, unlock->copies) && n < max) {
        int card = Cards_Named(unlock->card);
        LINE();
        names[0] = card > 0 ? Cards_NameCodes(card) : nameless;
        numbers[0] = (unsigned)(unlock->copies > 1 ? unlock->copies : 1);
        if (unlock->copies > 1) own(out, TEXT_OWN_PACK_HOLD_COPIES, "HOLD %d %s", numbers, names);
        else own(out, TEXT_OWN_PACK_HOLD, "HOLD %s", numbers, names);
    }
    if (s.progress.starchips_spent < unlock->starchips_spent && n < max) {
        LINE();
        numbers[0] = unlock->starchips_spent - s.progress.starchips_spent;
        own(out, TEXT_OWN_PACK_SPEND, "SPEND %d MORE", numbers, NULL);
    }
    if (s.progress.packs_opened < unlock->packs_opened && n < max) {
        LINE();
        numbers[0] = unlock->packs_opened - s.progress.packs_opened;
        own(out, TEXT_OWN_PACK_OPEN_MORE, "OPEN %d MORE PACKS", numbers, NULL);
    }
    for (k = 0; k < unlock->opened_count && n < max; k++) {
        int pack = unlock->opened_pack[k];
        if (pack >= 0 && s.progress.packs[pack].opened >= unlock->opened_times[k]) continue;
        LINE();
        names[0] = pack >= 0 ? pack_name(pack) : nameless;
        numbers[0] = unlock->opened_times[k] - (pack >= 0 ? s.progress.packs[pack].opened : 0);
        own(out, TEXT_OWN_PACK_OPEN_PACK, "OPEN %s x%d", numbers, names);
    }
    #undef LINE
    return n;
}

/* A pack the player holds "max_copies" of every card of, which the shop
 * will not sell ("when_nothing_left": "refuse"). */
static int nothing_left(int pack)
{
    Packs_SetChestRoom(chest_room, NULL);
    return Packs_RefusesWhenNothingLeft(pack) && Packs_NothingLeft(pack, held, NULL);
}

/* ALL OWNED, grey, at the right of the price's line, NOTE_MARGIN clear of
 * the box's frame (which a full last letter would touch) and at least as
 * far from the price: why BUY is refused. The six languages' words (ten
 * letters at most) fit beside the longest price, 999999; a translation too
 * long for the room left loses its last letters, never the margin. */
static void nothing_left_note(Out *out)
{
    const int limit = out->limit;
    int width = own(NULL, TEXT_OWN_PACK_ALL_OWNED, "ALL OWNED", NULL, NULL), x = BOX_WIDTH - NOTE_MARGIN - width;
    if (x < out->x + NOTE_MARGIN) x = out->x + NOTE_MARGIN;
    at_x(out, x);
    out->limit = BOX_WIDTH - NOTE_MARGIN;
    colour(out, GREY);
    own(out, TEXT_OWN_PACK_ALL_OWNED, "ALL OWNED", NULL, NULL);
    colour(out, WHITE);
    out->limit = limit;
}

/* The pack as the list shows it: 1-2 lines (the shop, the description or
 * how many cards), the price, then what the buttons do: ✕BUY ○BACK □INFO on
 * one line, or, when a language's words do not fit the box's twenty letters
 * (ACHETER, RETOUR and INFO are 17 with their icons), ✕BUY ○BACK on one and
 * □INFO under it, the description one line shorter (□ shows it whole). */
static void compose_list(void)
{
    Out out = begin(TEXT_MESSAGE);
    const Pack *pack = Packs_At(s.pack);
    const int locked = !unlocked(s.pack);
    const int back_id = rules()->password == PACK_SHOP_PACKS_ONLY ? TEXT_OWN_PACK_END : TEXT_OWN_PACK_BACK;
    const char *back_english = rules()->password == PACK_SHOP_PACKS_ONLY ? "END" : "BACK";
    const int one_row = (locked ? 0 : hint_width(TEXT_OWN_PACK_BUY, "BUY") + 8) + hint_width(back_id, back_english) + 8 +
                        hint_width(TEXT_OWN_PACK_INFO, "INFO") <= BOX_WIDTH;
    const int top = one_row ? 3 : 2;   /* the lines above the buttons */
    int place = 0, total = listed_count(s.shop, &place, s.pack), lines_left = top - 1, stock;
    int written = 0;
    if (shops_open() > 1) {
        colour(&out, BLUE);
        codes(&out, shop_name(s.shop), BOX_LETTERS);
        colour(&out, WHITE);
        written++;
        lines_left--;
    }
    if (locked) {
        if (written) newline(&out);
        colour(&out, GREY);
        own(&out, TEXT_OWN_PACK_LOCKED, "LOCKED", NULL, NULL);
        colour(&out, WHITE);
        written++;
        lines_left--;
        /* Up to the line before the buttons (with two rows of them, the one
           line LOCKED leaves). */
        if (lines_left > 0 || (!one_row && lines_left == 0))
            written += condition_lines(&pack->unlock, &out, lines_left + 1, 0);
    } else {
        const unsigned char *starts[2];
        int lengths[2], n = wrap(pack_description(s.pack), starts, lengths, lines_left), i;
        for (i = 0; i < n; i++) {
            if (written++) newline(&out);
            put_letters(&out, starts[i], lengths[i]);
        }
        if (!n && lines_left > 0) {
            unsigned count = (unsigned)pack->count;
            if (written++) newline(&out);
            cards_words(&out, count);
        }
        while (written < top - 1) { newline(&out); written++; }
        newline(&out);
        written++;
        price_line(&out, s.pack);
        stock = Packs_StockLeft(s.pack, &s.progress);
        if (stock == 0 || (pack->once && s.progress.packs[s.pack].used)) {
            int width = own(NULL, TEXT_OWN_PACK_SOLD_OUT, "SOLD OUT", NULL, NULL);
            at_x(&out, BOX_WIDTH - width);
            colour(&out, GREY);
            own(&out, TEXT_OWN_PACK_SOLD_OUT, "SOLD OUT", NULL, NULL);
            colour(&out, WHITE);
        } else if (nothing_left(s.pack)) {
            nothing_left_note(&out);
        } else if (stock > 0) {
            unsigned left = (unsigned)stock;
            int width = own(NULL, TEXT_OWN_PACK_LEFT, "LEFT %d", &left, NULL);
            at_x(&out, BOX_WIDTH - width);
            own(&out, TEXT_OWN_PACK_LEFT, "LEFT %d", &left, NULL);
        } else if (total > 1) {
            count_at_right(&out, (unsigned)place + 1, (unsigned)total);
        }
    }
    while (written < top) { newline(&out); written++; }
    newline(&out);
    if (!locked) hint(&out, ICON_CROSS, TEXT_OWN_PACK_BUY, "BUY");
    hint(&out, ICON_CIRCLE, back_id, back_english);
    if (!one_row) newline(&out);
    hint(&out, ICON_SQUARE, TEXT_OWN_PACK_INFO, "INFO");
    finish(&out);
}

/* The pack's name in the digits' panel, centred between the arrows. */
static void compose_name(int pack, int hidden)
{
    Out out = begin(TEXT_NAME);
    int room = NAME_WIDTH / 8, length;
    const unsigned char *name = pack_name(pack);
    if (hidden) {
        int i;
        at_x(&out, (NAME_WIDTH - 6 * 8) / 2);
        for (i = 0; i < 6; i++) glyph(&out, Glyphs_Code('?'));
    } else {
        length = letters(name);
        if (length > room) length = room;
        at_x(&out, (NAME_WIDTH - 8 * length) / 2);
        codes(&out, name, room);
    }
    finish(&out);
}

/* Whether the save can buy pack `pack` now; its cards on the price met. */
static int affordable(int pack)
{
    const Pack *one = Packs_At(pack);
    int i;
    if (!one || !unlocked(pack)) return 0;
    if (Packs_StockLeft(pack, &s.progress) == 0) return 0;
    if (one->once && s.progress.packs[pack].used) return 0;
    if (nothing_left(pack)) return 0;
    /* As the password shop (shop.c, Duel_ChestFull): no price for a card
       the chest would not keep. */
    if (!Packs_FixedCardsFit(pack)) return 0;
    if (gLibrary_dwStarchips < one->price) return 0;
    for (i = 0; i < one->cost_cards; i++) {
        if (*Cards_ChestSlot(gDuel_awPlayerDeck, one->cost_card[i]) < one->cost_copies[i]) return 0;
    }
    return 1;
}

/* BUY / QUIT, as the game's EXCHANGE / QUIT (strings 227 and 228): the name
 * and the price above, BUY grey and skipped when it cannot be bought. */
static void compose_confirm(int pack)
{
    Out out = begin(TEXT_CONFIRM);
    const int can = affordable(pack);
    put(&out, 0xFB);
    if (can) {
        put(&out, 0x64);
    } else {
        put(&out, 0x6C);
        put(&out, 0x02);
    }
    codes(&out, pack_name(pack), BOX_LETTERS);
    newline(&out);
    price_line(&out, pack);
    if (nothing_left(pack)) nothing_left_note(&out);
    if (!can) colour(&out, GREY);
    newline(&out);
    space(&out);
    own(&out, TEXT_OWN_PACK_BUY, "BUY", NULL, NULL);
    newline(&out);
    colour(&out, WHITE);
    space(&out);
    own(&out, TEXT_OWN_PACK_QUIT, "QUIT", NULL, NULL);
    newline(&out);
    /* {choose 80 0 0}: the answer's jump, which lands on arena[0], an {end}
     * (PackShop_Retarget). */
    put(&out, 0xFB);
    put(&out, 0x80);
    put(&out, 0); put(&out, 0);
    put(&out, 0); put(&out, 0);
    finish(&out);
}

static const PackTier *tier_of(int slot)
{
    const Pack *pack = Packs_At(s.pack);
    int t = s.result.tiers[slot];
    return pack && t >= 0 && t < pack->tier_count ? &pack->tiers[t] : NULL;
}

static int reveal_style(int slot)
{
    const Pack *pack = Packs_At(s.pack);
    const PackTier *tier = tier_of(slot);
    if (tier && tier->reveal >= 0) return tier->reveal;
    return pack ? pack->reveal : PACK_REVEAL_FLIP;
}

/* The card just turned over: its name, its tier's label in its colour and
 * which of how many, NEW, and the buttons. */
static void compose_reveal(int slot)
{
    Out out = begin(TEXT_MESSAGE);
    const PackTier *tier = tier_of(slot);
    int shown = 0, at = 0, i;
    for (i = 0; i < s.result.count; i++) {
        if (!s.result.cards[i]) continue;
        shown++;
        if (i <= slot) at = shown;
    }
    codes(&out, Cards_NameCodes(s.result.cards[slot]), BOX_LETTERS);
    newline(&out);
    if (tier && tier->label[0]) {
        unsigned char *label = encode(tier->label);
        colour(&out, tier->color >= 0 ? tier->color : WHITE);
        codes(&out, label, BOX_LETTERS - 6);
        colour(&out, WHITE);
        free(label);
    }
    count_at_right(&out, (unsigned)at, (unsigned)shown);
    newline(&out);
    if (s.fresh[slot]) {
        colour(&out, GOLD);
        own(&out, TEXT_OWN_NEW, "NEW", NULL, NULL);
        colour(&out, WHITE);
    }
    newline(&out);
    if (reveal_style(slot) == PACK_REVEAL_FLIP) {
        hint(&out, ICON_CROSS, TEXT_OWN_PACK_NEXT, "NEXT");
        hint(&out, ICON_SQUARE, TEXT_OWN_PACK_SKIP, "SKIP");
    }
    finish(&out);
}

static int summary_rows(int *slots)
{
    int i, n = 0;
    for (i = 0; i < s.result.count; i++) if (s.result.cards[i]) slots[n++] = i;
    return n;
}

/* Three cards to a page, NEW at the right, then OK and the page. */
static void compose_summary(void)
{
    Out out = begin(TEXT_MESSAGE);
    int slots[PACK_COUNT_MAX], rows = summary_rows(slots), pages = (rows + LINES_A_PAGE - 1) / LINES_A_PAGE, r;
    int new_width = own(NULL, TEXT_OWN_NEW, "NEW", NULL, NULL);
    if (pages < 1) pages = 1;
    if (s.page >= pages) s.page = pages - 1;
    for (r = 0; r < LINES_A_PAGE; r++) {
        int row = s.page * LINES_A_PAGE + r;
        if (r) newline(&out);
        if (row >= rows) continue;
        codes(&out, Cards_NameCodes(s.result.cards[slots[row]]),
              s.fresh[slots[row]] ? (BOX_WIDTH - new_width - 8) / 8 : BOX_LETTERS);
        if (s.fresh[slots[row]]) {
            at_x(&out, BOX_WIDTH - new_width);
            colour(&out, GOLD);
            own(&out, TEXT_OWN_NEW, "NEW", NULL, NULL);
            colour(&out, WHITE);
        }
    }
    newline(&out);
    hint(&out, ICON_CROSS, TEXT_OWN_PACK_OK, "OK");
    if (pages > 1) count_at_right(&out, (unsigned)s.page + 1, (unsigned)pages);
    finish(&out);
}

/* --- the details page ---------------------------------------------------------------- */

#define INFO_LINES 64
#define INFO_LINE_BYTES 96
static unsigned char info[INFO_LINES][INFO_LINE_BYTES];
static int info_count;

static Out info_line(void)
{
    Out out;
    out.at = info[info_count];
    out.end = info[info_count] + INFO_LINE_BYTES - 8;
    out.x = 0;
    out.limit = BOX_WIDTH;
    return out;
}

static void info_done(Out *out)
{
    *out->at = 0xFF;
    if (info_count < INFO_LINES - 1) info_count++;
}

static void build_info(void)
{
    const Pack *pack = Packs_At(s.pack);
    const unsigned char *starts[INFO_LINES];
    int lengths[INFO_LINES], n, i, t;
    unsigned numbers[2];
    const unsigned char *names[1];
    Out out;
    info_count = 0;
    if (!pack) return;
    n = wrap(pack_description(s.pack), starts, lengths, INFO_LINES / 2);
    for (i = 0; i < n; i++) {
        out = info_line();
        put_letters(&out, starts[i], lengths[i]);
        info_done(&out);
    }
    out = info_line();
    cards_words(&out, (unsigned)pack->count);
    info_done(&out);
    out = info_line();
    icon(&out, ICON_STAR);
    words(&out, "x");
    number(&out, pack->price);
    info_done(&out);
    for (i = 0; i < pack->cost_cards; i++) {
        out = info_line();
        words(&out, "+");
        number(&out, pack->cost_copies[i]);
        words(&out, " ");
        codes(&out, Cards_NameCodes(pack->cost_card[i]), BOX_LETTERS - 5);
        info_done(&out);
    }
    /* The odds of a slot dealt by the tiers', rarest last. */
    for (t = 0; pack->tier_count > 1 && t < pack->tier_count; t++) {
        unsigned chance = Packs_TierChance(s.pack, t);
        char percent[24];
        unsigned char *label = encode(pack->tiers[t].label[0] ? pack->tiers[t].label : pack->tiers[t].name);
        out = info_line();
        if (pack->tiers[t].color >= 0) colour(&out, pack->tiers[t].color);
        codes(&out, label, BOX_LETTERS - 8);
        colour(&out, WHITE);
        snprintf(percent, sizeof(percent), "%u.%u%%", chance / 10000, chance / 1000 % 10);
        at_x(&out, BOX_WIDTH - 8 * (int)strlen(percent));
        words(&out, percent);
        info_done(&out);
        free(label);
    }
    for (t = 0; t < pack->tier_count; t++) {
        unsigned char *label;
        if (!pack->guarantee[t] && !pack->pity[t]) continue;
        label = encode(pack->tiers[t].label[0] ? pack->tiers[t].label : pack->tiers[t].name);
        names[0] = label;
        if (pack->guarantee[t]) {
            out = info_line();
            numbers[0] = (unsigned)pack->guarantee[t];
            own(&out, TEXT_OWN_PACK_AT_LEAST, "AT LEAST %d %s", numbers, names);
            info_done(&out);
        }
        if (pack->pity[t]) {
            out = info_line();
            numbers[0] = (unsigned)pack->pity[t];
            own(&out, TEXT_OWN_PACK_PITY, "%s IN %d PACKS", numbers, names);
            info_done(&out);
        }
        free(label);
    }
    if (pack->stock >= 0) {
        out = info_line();
        numbers[0] = (unsigned)Packs_StockLeft(s.pack, &s.progress);
        own(&out, TEXT_OWN_PACK_LEFT, "LEFT %d", numbers, NULL);
        info_done(&out);
    }
    if (nothing_left(s.pack)) {   /* why BUY is refused */
        out = info_line();
        colour(&out, GREY);
        own(&out, TEXT_OWN_PACK_ALL_OWNED, "ALL OWNED", NULL, NULL);
        colour(&out, WHITE);
        info_done(&out);
    }
    if (!unlocked(s.pack)) {
        out = info_line();
        colour(&out, GREY);
        own(&out, TEXT_OWN_PACK_LOCKED, "LOCKED", NULL, NULL);
        colour(&out, WHITE);
        info_done(&out);
        {   /* Every condition still unmet, a line each. */
            u8 buffer[INFO_LINE_BYTES * 8];
            Out all;
            const u8 *p;
            int line = 0;
            all.at = buffer;
            all.end = buffer + sizeof(buffer) - 8;
            all.x = 0;
            all.limit = BOX_WIDTH;
            n = condition_lines(&pack->unlock, &all, 8, 1);
            *all.at = 0xFF;
            for (p = buffer; line < n && *p != 0xFF; line++) {
                out = info_line();
                while (*p != 0xFF && *p != 0xFE) put(&out, *p++);
                if (*p == 0xFE) p++;
                info_done(&out);
            }
        }
    }
}

static void compose_info(void)
{
    Out out = begin(TEXT_MESSAGE);
    int pages = (info_count + LINES_A_PAGE - 1) / LINES_A_PAGE, r;
    if (pages < 1) pages = 1;
    if (s.page >= pages) s.page = pages - 1;
    for (r = 0; r < LINES_A_PAGE; r++) {
        int row = s.page * LINES_A_PAGE + r;
        const u8 *p;
        if (r) newline(&out);
        if (row >= info_count) continue;
        for (p = info[row]; *p != 0xFF; p++) put(&out, *p);
    }
    newline(&out);
    hint(&out, ICON_CIRCLE, TEXT_OWN_PACK_BACK, "BACK");
    if (pages > 1) count_at_right(&out, (unsigned)s.page + 1, (unsigned)pages);
    finish(&out);
}

/* --- the boxes ----------------------------------------------------------------------------- */

static void show_message(void)
{
    Password_CreateMessageBox(TEXT_MESSAGE_ID, 1);
}

void PackShop_NameBox(s32 id);   /* src/pc/game/pack_shop_box.c */

static void show_name(int pack, int hidden)
{
    compose_name(pack, hidden);
    PackShop_NameBox(TEXT_NAME_ID);
}

static void set_cursor_shown(int shown)
{
    PasswordCursorView *cursor = gPassword_pDigitCursorWidget;
    if (!cursor) return;
    if (shown) cursor->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
    else cursor->flags &= (u16)~DISPLAY_OBJECT_FLAG_RENDERABLE;
}

int PackShop_Decoration(unsigned char *object)
{
    PasswordCursorView *arrow = (PasswordCursorView *)object;
    int list, many, shops;
    if (!s.open || !PackShop_Available()) return 0;
    list = (D_8016D424 & 0x1F) == STATE_LIST;
    many = list && listed_count(s.shop, NULL, -1) > 1;
    shops = list && shops_open() > 1;
    arrow->flags &= (u16)~DISPLAY_OBJECT_FLAG_RENDERABLE;
    switch (arrow->kind) {
    case 0:   /* ► */
        if (many) arrow->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
        arrow->x = 0x129;
        arrow->y = 0x68;
        break;
    case 2:   /* ◄ */
        if (many) arrow->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
        arrow->x = 0xA9;
        arrow->y = 0x68;
        break;
    case 1:   /* ▼ */
        if (shops) arrow->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
        arrow->x = 0xE8;
        arrow->y = 0x78;
        break;
    case 3:   /* ▲ */
        if (shops) arrow->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
        arrow->x = 0xE8;
        arrow->y = 0x58;
        break;
    }
    return 1;
}

/* --- the big card ------------------------------------------------------------------------------ */

/* Turn the big card to show `card` (a card's face), or pack `pack`'s
 * picture when card is 0, or its back alone when both are negative. */
static void card_show(int card, int pack, int speed, int sound)
{
    s.card_goal = card;
    s.card_pack_art = card ? -1 : pack;
    s.card_speed = speed;
    s.card_sound = sound;
    s.card_phase = CARD_HIDING;
}

static int card_busy(void)
{
    PasswordCardPreviewView *widget = D_8016D4D8;
    switch (s.card_phase) {
    case CARD_HIDING:
        if (!widget) return 0;
        if ((widget->flags & DISPLAY_OBJECT_FLAG_CLIP_TEST) && widget->phase == 0x80) {
            s.card_phase = CARD_LOADING;
        } else {
            widget->flags |= DISPLAY_OBJECT_FLAG_CLIP_TEST;
            widget->phase = (u8)(widget->phase + s.card_speed);
            if ((s8)widget->phase < 0) {
                widget->phase = 0x80;
                s.card_phase = CARD_LOADING;
            }
            return 1;
        }
        /* fallthrough */
    case CARD_LOADING:
        if (s.card_goal < 0 || (!s.card_goal && s.card_pack_art < 0)) {
            s.card_phase = CARD_IDLE;   /* face down it stays */
            return 0;
        }
        if (s.card_goal) {
            s.card_cover = s.card_goal;
            func_80029164(0, s.card_goal);
        } else {
            const Pack *pack = Packs_At(s.card_pack_art);
            build_art();
            if (art_records[s.card_pack_art] && support_card()) {
                s.card_cover = support_card();
                Cards_OverrideArt(s.card_cover, art_records[s.card_pack_art], NULL);
            } else {
                s.card_cover = pack->cover;
                if (plates[s.card_pack_art]) Cards_OverrideArt(s.card_cover, NULL, plates[s.card_pack_art]);
            }
            func_80029164(0, s.card_cover);
        }
        s.card_phase = CARD_WAITING;
        return 1;
    case CARD_WAITING:
        if (((D_8009B0F4 & FILE_TRANSFER_REQUEST_BLOCKED_MASK) | D_8009B134) != 0) return 1;
        /* A pack is drawn in a Magic card's frame, which has no ATK or DEF:
           the art loaded is its cover's, or its own picture. */
        if (!s.card_goal && support_card()) D_800EA0E8[0].field_30 = (s16)support_card();
        Password_RecreateCardPreview(0);
        s.card_phase = CARD_SHOWING;
        return 1;
    case CARD_SHOWING:
        if (!widget) {
            s.card_phase = CARD_IDLE;
            return 0;
        }
        widget->phase = (u8)(widget->phase + s.card_speed);
        if (widget->phase == 0) {
            widget->flags &= ~DISPLAY_OBJECT_FLAG_CLIP_TEST;
            if (s.card_sound >= 0) SD_SEPlayFull((u32)s.card_sound);
            s.card_phase = CARD_IDLE;
            return 0;
        }
        return 1;
    }
    return 0;
}

/* --- the states ------------------------------------------------------------------------------------ */

static const Pack *shown_pack(void) { return Packs_At(s.pack); }

static int sound(int which)
{
    const Pack *pack = shown_pack();
    static const int defaults[PACK_SOUNDS] = {47, 48, 9, 12, 8};
    return pack ? pack->sounds[which] : defaults[which];
}

/* The list with pack `pack` (of shop `shop`) on it. */
static void list_show(int turn_card)
{
    const int locked = !unlocked(s.pack);
    D_8016D424 = STATE_LIST;
    s.step = 0;
    s.page = 0;
    show_name(s.pack, locked);
    compose_list();
    show_message();
    if (turn_card) card_show(locked ? -1 : 0, locked ? -1 : s.pack, FLIP_STEP, -1);
}

/* Into the list: from the digits (△), or as the screen opens. */
static int list_open(void)
{
    int shop = shop_open(s.shop) && listed_count(s.shop, NULL, -1) ? s.shop : next_shop(-1, 1);
    int pack;
    if (shop < 0) return 0;
    pack = listed(s.pack, shop) ? s.pack : next_listed(shop, -1, 1);
    if (pack < 0) return 0;
    own_progress();
    s.open = 1;
    s.shop = shop;
    s.pack = pack;
    s.from_password = 0;
    set_cursor_shown(0);
    list_show(1);
    return 1;
}

/* Back to the digits, as the game's state 4 goes back to them. */
static void leave_to_digits(void)
{
    D_8016D424 = STATE_LEAVE;
    s.step = 0;
    card_show(-1, -1, FLIP_STEP, -1);
}

static void quit_screen(void)
{
    Cards_OverrideArt(0, NULL, NULL);
    SD_SEPlayFull((u32)sound(PACK_SOUND_BACK));
    SD_BGMFadeOut();
    Fade_WaitOut();
    D_8009B26C = D_8009B269;
    s.open = 0;
}

static int game_random(void *context)
{
    (void)context;
    return Memories_Rand();
}

/* The purchase: dealt, paid in cards, awarded and counted, before the
 * starchips count down and before the first card turns over, so a state
 * saved at any point after never loses or doubles one. */
static void buy(void)
{
    const Pack *pack = shown_pack();
    unsigned seed;
    int i, k;
    Packs_SetChestRoom(chest_room, NULL);
    if (rules()->rng == PACK_RNG_SAVE) {
        seed = Packs_SaveSeed(running_code(), pack->identity, s.progress.packs[s.pack].opened);
        Packs_Deal(s.pack, &s.progress, held, NULL, Packs_LcgNext, &seed, &s.result);
    } else {
        Packs_Deal(s.pack, &s.progress, held, NULL, game_random, NULL, &s.result);
    }
    for (i = 0; i < pack->cost_cards; i++) {
        u8 *copies = Cards_ChestSlot(gDuel_awPlayerDeck, pack->cost_card[i]);
        *copies = (u8)(*copies >= pack->cost_copies[i] ? *copies - pack->cost_copies[i] : 0);
    }
    for (i = 0; i < s.result.count; i++) {
        int card = s.result.cards[i], earlier = 0;
        for (k = 0; k < i; k++) earlier |= s.result.cards[k] == card;
        s.fresh[i] = (u8)(card && !earlier && held(card, NULL) == 0);
    }
    for (i = 0; i < s.result.count; i++) {
        if (s.result.cards[i]) Duel_AwardCard(s.result.cards[i]);
    }
    /* Free spending takes nothing from the save: nothing counts as spent. */
    Packs_Record(s.pack, &s.result, Cheats_FreeSpending() ? 0 : pack->price, &s.progress);
    s.price_left = pack->price;
    for (i = 0, k = 0; i < s.result.count; i++) k += s.result.cards[i] != 0;
    LOG(LOG_MODS, "packs: %s bought for %u starchips, %d card%s of %d slots", pack->identity, pack->price, k,
        k == 1 ? "" : "s", s.result.count);
}

static void reveal_next(int from)
{
    int i;
    for (i = from; i < s.result.count; i++) {
        if (s.result.cards[i]) {
            const PackTier *tier;
            int style;
            s.reveal = i;
            tier = tier_of(i);
            style = reveal_style(i);
            D_8016D424 = STATE_REVEAL;
            s.step = 0;
            s.wait = 0;
            card_show(s.result.cards[i], -1, style == PACK_REVEAL_FLIP ? FLIP_STEP : QUICK_STEP,
                      tier && tier->sound >= 0 ? tier->sound : sound(PACK_SOUND_REVEAL));
            return;
        }
    }
    D_8016D424 = STATE_SUMMARY;
    s.step = 0;
    s.page = 0;
    compose_summary();
    show_message();
}

static void after_purchase(void)
{
    const Pack *pack = shown_pack();
    if (pack->reveal == PACK_REVEAL_LIST) {
        reveal_next(PACK_COUNT_MAX);   /* straight to the list of what came */
    } else {
        reveal_next(0);
    }
}

/* After a pack, or its question: the digits for a password's pack, else the
 * list, the card turned back to the pack when it shows another. */
static void back_from_pack(int turn_card)
{
    if (s.from_password) {
        leave_to_digits();
    } else {
        list_show(turn_card);
    }
}

static void update_list(void)
{
    const u16 pressed = gInput_wPad1Pressed, repeat = gInput_wPad1Repeat;
    if (card_busy()) return;
    if (repeat & PAD_DIRECTION_HORIZONTAL_MASK) {
        int next = next_listed(s.shop, s.pack, (repeat & PAD_DIRECTION_RIGHT) ? 1 : -1);
        if (next >= 0 && next != s.pack) {
            s.pack = next;
            SD_SEPlayFull((u32)sound(PACK_SOUND_MOVE));
            list_show(1);
        }
        return;
    }
    if (repeat & PAD_DIRECTION_VERTICAL_MASK) {
        int shop = shops_open() > 1 ? next_shop(s.shop, (repeat & PAD_DIRECTION_DOWN) ? 1 : -1) : -1;
        if (shop >= 0 && shop != s.shop) {
            s.shop = shop;
            s.pack = next_listed(shop, -1, 1);
            SD_SEPlayFull(7);
            list_show(1);
        }
        return;
    }
    if (pressed & PAD_BUTTON_CANCEL) {
        if (rules()->password == PACK_SHOP_PACKS_ONLY) {
            quit_screen();
        } else {
            SD_SEPlayFull((u32)sound(PACK_SOUND_BACK));
            leave_to_digits();
        }
        return;
    }
    if (pressed & PAD_BUTTON_SQUARE) {
        SD_SEPlayFull(7);
        build_info();
        D_8016D424 = STATE_INFO;
        s.page = 0;
        compose_info();
        show_message();
        return;
    }
    if (pressed & PAD_BUTTON_CROSS) {
        if (!unlocked(s.pack)) {
            SD_SEPlayFull((u32)sound(PACK_SOUND_REFUSE));
            return;
        }
        SD_SEPlayFull(48);
        compose_confirm(s.pack);
        Password_CreateMessageBox(TEXT_CONFIRM_ID, 0);
        D_8016D424 = STATE_CONFIRM;
        s.step = 0;
    }
}

/* CONFIRM by a password: the card turns first, then the question. */
static void confirm_after_card(void)
{
    if (card_busy()) return;
    Password_CreateMessageBox(TEXT_CONFIRM_ID, 0);
    s.step = 0;
}

static void update_confirm(void)
{
    if (s.step == 1) {
        confirm_after_card();
        return;
    }
    /* The box is done: the player answered (gDialog_bChoice, 0 for BUY). */
    if (gDialog_bChoice == 0 && affordable(s.pack)) {
        SD_SEPlayFull((u32)sound(PACK_SOUND_BUY));
        buy();
        D_8016D424 = STATE_PAY;
        return;
    }
    back_from_pack(0);
}

static void update_pay(void)
{
    u32 count = s.price_left, step = 1;
    if (!count) {
        after_purchase();
        return;
    }
    /* The game's own count (state 3): faster the more there is to pay. */
    if (count >= 10) step = count / 10;
    if (count >= 100) step = count / 20;
    if (count >= 1000) step = count / 30;
    if (count >= 10000) step = count / 40;
    if (step == 0) step = 1;
    if (step > count) step = count;
    s.price_left -= step;
    /* Game > Cheats > Free spending: the count runs, the balance stays. */
    if (!Cheats_FreeSpending()) gLibrary_dwStarchips = gLibrary_dwStarchips >= step ? gLibrary_dwStarchips - step : 0;
    Password_RefreshStarchipDisplay();
}

static void update_reveal(void)
{
    const u16 pressed = gInput_wPad1Pressed;
    if (card_busy()) return;
    if (s.step == 0) {
        compose_reveal(s.reveal);
        show_message();
        s.step = 1;
        s.wait = 0;
        return;
    }
    if (pressed & PAD_BUTTON_SQUARE) {
        SD_SEPlayFull(7);
        reveal_next(PACK_COUNT_MAX);
        return;
    }
    if (reveal_style(s.reveal) != PACK_REVEAL_FLIP && ++s.wait >= QUICK_WAIT) {
        reveal_next(s.reveal + 1);
        return;
    }
    if (pressed & PAD_BUTTON_CROSS) {
        SD_SEPlayFull(7);
        reveal_next(s.reveal + 1);
    }
}

static void update_pages(int info_page)
{
    const u16 pressed = gInput_wPad1Pressed, repeat = gInput_wPad1Repeat;
    int slots[PACK_COUNT_MAX];
    int rows = info_page ? info_count : summary_rows(slots);
    int pages = (rows + LINES_A_PAGE - 1) / LINES_A_PAGE;
    if (card_busy()) return;
    if ((repeat & PAD_DIRECTION_HORIZONTAL_MASK) && pages > 1) {
        s.page = (s.page + ((repeat & PAD_DIRECTION_RIGHT) ? 1 : pages - 1)) % pages;
        SD_SEPlayFull(47);
        if (info_page) compose_info();
        else compose_summary();
        show_message();
        return;
    }
    if (info_page && (pressed & (PAD_BUTTON_CANCEL | PAD_BUTTON_CROSS | PAD_BUTTON_SQUARE))) {
        SD_SEPlayFull((u32)sound(PACK_SOUND_BACK));
        if (s.from_password) leave_to_digits();
        else list_show(0);
        return;
    }
    if (!info_page && (pressed & (PAD_BUTTON_CROSS | PAD_BUTTON_CANCEL))) {
        SD_SEPlayFull((u32)sound(PACK_SOUND_BACK));
        back_from_pack(1);
    }
}

static void update_leave(void)
{
    int i;
    if (card_busy()) return;
    Cards_OverrideArt(0, NULL, NULL);
    s.open = 0;
    s.from_password = 0;
    set_cursor_shown(1);
    /* The digit cursor's arrows as the game left them: the game shows or
       hides ◄ and ► by the digit each frame, and never touches ▲ and ▼. */
    for (i = 0; i < 4; i++) {
        if (D_8016D440[i]) D_8016D440[i]->flags |= DISPLAY_OBJECT_FLAG_RENDERABLE;
    }
    Password_RefreshDigitDisplay();
    Password_CreateMessageBox(MESSAGE_ID, 0);
    D_8016D424 = 0;
}

void PackShop_Update(int state)
{
    if (!PackShop_Available() || !s.open) {
        D_8016D424 = 0;   /* a state of packs with none: back to the digits */
        return;
    }
    switch (state) {
    case STATE_LIST: update_list(); break;
    case STATE_CONFIRM: update_confirm(); break;
    case STATE_PAY: update_pay(); break;
    case STATE_REVEAL: update_reveal(); break;
    case STATE_SUMMARY: update_pages(0); break;
    case STATE_INFO: update_pages(1); break;
    case STATE_LEAVE: update_leave(); break;
    default: D_8016D424 = 0; break;
    }
}

int PackShop_Triangle(void)
{
    if (!PackShop_Available() || rules()->password != PACK_SHOP_BOTH) return 0;
    if (!(gInput_wPad1Pressed & PAD_BUTTON_TRIANGLE)) return 0;
    own_progress();
    if (!list_open()) {
        SD_SEPlayFull(9);   /* nothing on sale yet */
        return 1;
    }
    SD_SEPlayFull(48);
    return 1;
}

int PackShop_Password(void)
{
    unsigned password = 0;
    int i, pack;
    if (!PackShop_Available()) return 0;
    for (i = 0; i < 8; i++) password = (password << 4) | gPassword_abDigits[i];
    pack = Packs_WithPassword(password);
    if (pack < 0) return 0;
    own_progress();
    if (!unlocked(pack)) return 0;   /* a locked pack's password is no password yet */
    SD_SEPlayFull(48);
    s.open = 1;
    s.pack = pack;
    s.from_password = 1;
    set_cursor_shown(0);
    show_name(pack, 0);
    compose_confirm(pack);
    D_8016D424 = STATE_CONFIRM;
    s.step = 1;   /* the card turns first, then the question */
    card_show(0, pack, FLIP_STEP, 12);
    return 1;
}

/* A pack whose password a card has too: the card is what the digits sell
 * (Password_LookupCardID first). Said once, with the table loaded. */
static void check_passwords(void)
{
    static int checked;
    int i, id;
    if (checked) return;
    checked = 1;
    for (i = 0; i < Packs_Count(); i++) {
        const Pack *pack = Packs_At(i);
        if (!pack->has_password) continue;
        for (id = 1; id <= gCard_nCount; id++) {
            if (Cards_Password(id) == pack->password) {
                Mods_Note(pack->mod, "pack \"%s\": its password is card %d's too; the digits sell the card", pack->id,
                          id);
                break;
            }
        }
    }
}

void PackShop_Enter(void)
{
    if (!PackShop_Available()) return;
    /* A pack's picture armed for a load the screen never saw (a state
       loaded, a jump to the title) is not the next Magic card's. */
    Cards_OverrideArt(0, NULL, NULL);
    s.open = 0;
    s.card_phase = CARD_IDLE;
    own_progress();
    build_art();
    check_passwords();
    if (rules()->music != 29520) SD_BGMPlay((u32)rules()->music);
    if (rules()->password == PACK_SHOP_PACKS_ONLY) list_open();
}

/* --- beside the save --------------------------------------------------------------------------------- */

static int progress_path(char *out, size_t size, unsigned token)
{
    char relative[64];
    snprintf(relative, sizeof(relative), "packs/%08X.txt", token);
    return Paths_User(out, size, relative);
}

/* Whether `file` is a progress file, "XXXXXXXX.txt" (progress_path), and
 * its token. */
static int token_named(const char *file, unsigned *token, char tail[8])
{
    return strlen(file) == 12 && sscanf(file, "%8x%7s", token, tail) == 2 && !strcmp(tail, ".txt") &&
           strspn(file, "0123456789ABCDEF") == 8;
}

static unsigned slot_token(void)
{
    int slot = SaveMenu_CurrentSlot();
    return slot >= 0 ? SaveSlots_Token(slot) : 0;
}

void PackShop_SaveLoaded(const void *state)
{
    char path[1024];
    unsigned token;
    FILE *file;
    if (!PackShop_Available()) return;
    Packs_ForgetProgress(&s.progress);
    memcpy(&s.owner, (const u8 *)state + SAVE_DUELIST_CODE, sizeof(s.owner));
    s.owner_set = 1;
    s.token = token = slot_token();
    if (!token || progress_path(path, sizeof(path), token)) return;
    file = fopen(path, "r");
    if (!file) return;
    Packs_ReadProgress(file, &s.progress);
    fclose(file);
    LOG(LOG_MODS, "packs: progress read from %s (%u opened)", path, s.progress.packs_opened);
}

/* The progress files no slot's save goes with any more: the one the slot
 * just written had before, and that of a slot saved over from another's
 * load. None while a slot cannot be read (its token is not known). */
static void forget_progress(unsigned written)
{
    unsigned live[SAVE_SLOT_COUNT], token;
    char directory[1024], path[1024], *slash;
    struct dirent *item;
    DIR *folder;
    int slot, i;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        if (SaveSlots_ReadToken(slot, &live[slot])) return;
    }
    if (progress_path(directory, sizeof(directory), 1)) return;
    slash = strrchr(directory, '/');
    if (!slash) return;
    *slash = '\0';
    if (!(folder = opendir(directory))) return;
    while ((item = readdir(folder)) != NULL) {
        char tail[8];
        int still = token_named(item->d_name, &token, tail) ? token == written : 1;
        for (i = 0; i < SAVE_SLOT_COUNT && !still; i++) still = live[i] == token;
        if (!still && !progress_path(path, sizeof(path), token)) remove(path);
    }
    closedir(folder);
}

void PackShop_SaveWritten(const void *state)
{
    char path[1024], temporary[1040], directory[1024], *slash;
    unsigned token = slot_token();
    u32 code;
    FILE *file;
    if (!PackShop_Available() || !token) return;
    memcpy(&code, (const u8 *)state + SAVE_DUELIST_CODE, sizeof(code));
    if (!s.owner_set || s.owner != code) {
        Packs_ForgetProgress(&s.progress);
        s.owner = code;
        s.owner_set = 1;
    }
    s.token = token;
    if (Packs_ProgressEmpty(&s.progress)) goto forget_old;   /* nothing bought: no file */
    if (progress_path(path, sizeof(path), token)) return;
    snprintf(directory, sizeof(directory), "%s", path);
    slash = strrchr(directory, '/');
    if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "w");
    if (!file) {
        fprintf(stderr, "memories-pc: cannot write %s\n", temporary);
        return;
    }
    Packs_WriteProgress(file, &s.progress);
    {
        int failed = ferror(file);
        if (fclose(file)) failed = 1;
        if (failed || rename(temporary, path)) {
            remove(temporary);
            fprintf(stderr, "memories-pc: cannot replace %s\n", path);
            return;
        }
    }
forget_old:
    forget_progress(token);
}

/* --- save states -------------------------------------------------------------------------------------- */

/* The progress lines of packs not here this run (Packs_ForeignLines) go
 * with the state's progress, not stay those of the save loaded since: else
 * a save written after the load would be given another save's lines. Their
 * size first, then the lines. */
static void foreign_state(MemoriesState *state)
{
    size_t size;
    const char *lines = Packs_ForeignLines(&size);
    uint32_t length = (uint32_t)size;
    MemoriesStateField count = {&length, sizeof(length)};
    if (!Memories_StateLoading(state)) {
        MemoriesStateField text = {(void *)lines, size};
        Memories_StateChunk(state, "pack-foreign-n", &count, 1);
        Memories_StateChunk(state, "pack-foreign", &text, 1);
        return;
    }
    if (Memories_StateChunk(state, "pack-foreign-n", &count, 1)) {
        char *read = malloc(length + 1u);
        MemoriesStateField text = {read, length};
        if (read && Memories_StateChunk(state, "pack-foreign", &text, 1)) {
            Packs_SetForeignLines(read, length);
            free(read);
            return;
        }
        free(read);
    }
    Packs_SetForeignLines(NULL, 0);
}

void PackShop_State(MemoriesState *state)
{
    uint32_t base = (uint32_t)(uintptr_t)s.arena, size = ARENA_SIZE;
    MemoriesStateField fields[] = {{&s, sizeof(s)}, {&base, sizeof(base)}, {&size, sizeof(size)}};
    s.version = STATE_VERSION;
    if (!Packs_Count()) return;
    if (Memories_StateLoading(state)) {
        Screen before = s;
        Cards_OverrideArt(0, NULL, NULL);   /* this session's, not the state's */
        s.open = 0;   /* a state without the chunk: the packs closed */
        if (Memories_StateChunk(state, "pack-shop", fields, 3)) {
            if (s.version != STATE_VERSION || size != ARENA_SIZE) {
                s = before;
                s.open = 0;
                return;
            }
            /* Saved while a pack's picture was loading: the load goes on
               after this one, and is the pack's again. */
            if (s.open && s.card_phase == CARD_WAITING && !s.card_goal && s.card_pack_art >= 0 &&
                s.card_pack_art < Packs_Count()) {
                build_art();
                if (art_records[s.card_pack_art]) Cards_OverrideArt(s.card_cover, art_records[s.card_pack_art], NULL);
                else if (plates[s.card_pack_art]) Cards_OverrideArt(s.card_cover, NULL, plates[s.card_pack_art]);
            }
            if (base != (uint32_t)(uintptr_t)s.arena) {
                LOG(LOG_MODS, "packs: the screen's text moved from %08X to %08X since the state was saved",
                    (unsigned)base, (unsigned)(uintptr_t)s.arena);
                Memories_StateRemapRange(state, base, (uint32_t)(uintptr_t)s.arena, ARENA_SIZE);
            }
            hint_ready = 0;
            foreign_state(state);
        }
        return;
    }
    Memories_StateChunk(state, "pack-shop", fields, 3);
    foreign_state(state);
}
