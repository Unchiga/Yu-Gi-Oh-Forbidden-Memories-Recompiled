/* A duelist's bust (busts.h, notes/more-duelists.md, Busts): read from a
 * duelists entry's "bust", and made, once every mod is read, into the kind
 * of record the campaign's large characters are on the disc, so the
 * game's own code brings it in, makes it talk and blink, changes its face
 * and takes it out.
 *
 * A disc record (WA sector 15182 + id * 50, kept by func_8003A560 in a
 * cache slot) is a 384 x 256 sheet at 8 bits, two rows of 256 colors and a
 * parts bank of 0x800 bytes: a tree of u16 offsets, expression -> piece ->
 * variant -> a stream of (frames, u16 frame) entries (0xFF stops it, which
 * zeroes the object's countdown; a 0 holds the frame), and each frame a
 * header (count, flags, page, palette) and its parts, six bytes each:
 * signed x and y from the anchor (ten bits with flag 0x10), the sheet's
 * cell (8-texel units) and the size (DisplayObject_RenderSpriteSheet). The
 * disc's characters use flag 0x10 and the first 256 columns only, and so
 * does this. Of the three pieces DisplayEffect_BuildResourceObjects makes:
 *
 *   0  the body: variant 0 held;
 *   1  the eyes (DisplayEffect_UpdateMenuRecord): variant 0 held, variant
 *      1 played after a random wait -- a blink -- which must stop, since
 *      the eyes go back to 0 only once its countdown is 0;
 *   2  the mouth: the variant the letter being typed asks for (its glyph's
 *      kind, 0-7; 0-2 held, 3 played through on Simon's sheet), and 0 once
 *      the words stop.
 *
 * So a bust's expression is its picture as the body, its blink frame's
 * changes as the eyes' variant 1 and its talking frames' changes as the
 * mouth's variants: drawn over the body where they differ from it, as the
 * disc's eyes and mouths are. Without a blink frame the eyes are no piece
 * at all (the game then never blinks it), without a talking frame the
 * mouth is none (it never speaks), so a frame left out is the picture
 * alone, never a blank.
 *
 * Room: the first expression of a record is its picture whole at the
 * sheet's top left. Another expression is the first's picture where they
 * are the same and its own parts where they differ, cut in squares from
 * 128 texels down to 8 (a quadtree, so the parts do not overlap and a few
 * cover a face that changed); a talking or blink frame, the squares where
 * it differs from its expression. Squares the same as one cut before are
 * cut once. The parts are in the 256 x 256 the disc's use, and the bank
 * holds 0x800 bytes: what does not fit goes in the bust's next record. */
#include "busts.h"
#include "duelists.h"
#include "pc/cards/art.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/platform/loading.h"
#include "pc/tower/tower.h"
#include "pc/debug/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHARACTERS 0x41              /* the campaign's large characters, 1-0x40 */
/* The side's anchor (func_80039F44: y 0xB2, just under the box's top): a
 * picture stands on it, and one taller than the room above it has its top
 * at the screen's and runs on under the box. */
#define ANCHOR_Y 0xB2
#define BLINK_FRAMES 8               /* how long the eyes stay shut */
#define TALK_FRAMES 3                /* a step of the mouth played through (variant 3) */
#define MOUTH_VARIANTS 8             /* a glyph's kind is three bits */
#define FRAME_PARTS 255              /* a frame's count is a byte */
#define CELLS (BUST_SIDE / 8)

typedef struct {
    char name[BUST_NAME];
    char image[BUST_PATH];
    char talk[BUST_TALK_FRAMES][BUST_PATH];
    int talk_count;
    char blink[BUST_PATH];           /* "": none */
    int record, slot;                /* where it was made; record -1 not */
} Expression;

typedef struct {
    char mod[64];
    char where[64];
    int count, chosen;               /* expressions; the default */
    Expression *expressions;
    int order[BUST_EXPRESSIONS];     /* the default, then the list's */
    int made;                        /* 1 made (or tried) */
    int character;                   /* the campaign character it replaces, 0 none */
    int duelist;                     /* the disc duelist it replaces that for, 0 none */
    int first;                       /* its first record, -1 none */
} Bust;

typedef struct {
    unsigned char *slot;             /* the sheet, then the two rows of colors */
    unsigned char *bank;
} Record;

static Bust *busts;
static int bust_count, bust_room;
static Record records[BUST_RECORDS];
static int record_count;
static int replacing[CHARACTERS];    /* 1 + the record, 0 none */

static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return !*a && !*b;
}

static int is_png(const char *text)
{
    size_t length = text ? strlen(text) : 0;
    return length > 4 && same_word(text + length - 4, ".png");
}

/* --- reading -------------------------------------------------------------- */

/* A PNG the bust names: its path inside `directory` into `out`, 1; 0 noted. */
static int picture(const char *mod, const char *directory, const char *where, const JsonValue *value,
                   char *out, size_t size)
{
    const char *file = Json_String(value, NULL);
    int w, h;
    if (!file || !is_png(file)) {
        Mods_Note(mod, "%s: a picture is a PNG of the mod's, a path inside it", where);
        return 0;
    }
    if (!directory) {
        Mods_Note(mod, "%s: %s: the mod has no folder to find it in", where, file);
        return 0;
    }
    if (!Paths_Contained(file)) {
        Mods_Note(mod, "%s: %s is not a path inside the mod", where, file);
        return 0;
    }
    if (snprintf(out, size, "%s/%s", directory, file) >= (int)size) {
        Mods_Note(mod, "%s: the path of %s is too long", where, file);
        return 0;
    }
    if (!CardArt_ImageSize(out, &w, &h) || w < 1 || h < 1) {
        Mods_Note(mod, "%s: %s is not a PNG it could read", where, file);
        return 0;
    }
    return 1;
}

static void check_keys(const char *mod, const char *where, const JsonValue *object, const char *const *known,
                       size_t count)
{
    const JsonValue *member;
    for (member = Json_At(object, 0); member; member = Json_Next(member)) {
        size_t k;
        for (k = 0; k < count && strcmp(Json_Name(member), known[k]); k++) {}
        if (k == count) Mods_Note(mod, "%s: unknown key \"%s\"", where, Json_Name(member));
    }
}

/* One of "expressions": {"name", "image", "talk", "blink"}. 1 when it has
 * a picture. */
static int read_expression(const char *mod, const char *directory, const char *where, const JsonValue *value,
                           Expression *out)
{
    static const char *const known[] = {"name", "image", "talk", "blink"};
    const char *name = Json_String(Json_Member(value, "name"), NULL);
    const JsonValue *talk = Json_Member(value, "talk"), *blink = Json_Member(value, "blink");
    char at[160];
    memset(out, 0, sizeof(*out));
    out->record = -1;
    if (Json_TypeOf(value) != JSON_OBJECT) {
        Mods_Note(mod, "%s: an expression is {\"name\": ..., \"image\": ...}", where);
        return 0;
    }
    check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
    if (!name || !name[0]) {
        Mods_Note(mod, "%s: an expression has a \"name\"", where);
        return 0;
    }
    if (strlen(name) >= BUST_NAME) {
        Mods_Note(mod, "%s: an expression's name is at most %d bytes", where, BUST_NAME - 1);
        return 0;
    }
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(at, sizeof(at), "%s \"%s\"", where, name);
    if (!Json_Member(value, "image")) {
        Mods_Note(mod, "%s: an expression has an \"image\"", at);
        return 0;
    }
    if (!picture(mod, directory, at, Json_Member(value, "image"), out->image, sizeof(out->image))) return 0;
    if (Json_TypeOf(talk) == JSON_ARRAY) {
        const JsonValue *item;
        for (item = Json_At(talk, 0); item; item = Json_Next(item)) {
            if (out->talk_count == BUST_TALK_FRAMES) {
                Mods_Note(mod, "%s: \"talk\" has at most %d frames; the rest are left out", at, BUST_TALK_FRAMES);
                break;
            }
            if (picture(mod, directory, at, item, out->talk[out->talk_count], BUST_PATH)) out->talk_count++;
        }
    } else if (talk && Json_TypeOf(talk) != JSON_NULL) {
        if (picture(mod, directory, at, talk, out->talk[0], BUST_PATH)) out->talk_count = 1;
    }
    if (blink && Json_TypeOf(blink) != JSON_NULL && !picture(mod, directory, at, blink, out->blink, BUST_PATH))
        out->blink[0] = 0;
    return 1;
}

int Busts_Read(const char *mod, const char *directory, const JsonValue *value, const char *where)
{
    static const char *const known[] = {"default", "expressions"};
    Bust one;
    char at[160];
    const char *chosen;
    int i, k;
    memset(&one, 0, sizeof(one));
    one.first = -1;
    snprintf(one.mod, sizeof(one.mod), "%s", mod ? mod : "");
    snprintf(one.where, sizeof(one.where), "%s", where);
    one.expressions = calloc(BUST_EXPRESSIONS, sizeof(Expression));
    if (!one.expressions) return -1;
    if (Json_TypeOf(value) == JSON_STRING) {
        /* One picture: one expression, named "default". */
        snprintf(at, sizeof(at), "%s bust", where);
        snprintf(one.expressions[0].name, BUST_NAME, "default");
        one.expressions[0].record = -1;
        if (picture(mod, directory, at, value, one.expressions[0].image, BUST_PATH)) one.count = 1;
    } else if (Json_TypeOf(value) == JSON_OBJECT) {
        const JsonValue *list = Json_Member(value, "expressions"), *item;
        check_keys(mod, where, value, known, sizeof(known) / sizeof(known[0]));
        if (Json_TypeOf(list) != JSON_ARRAY) {
            Mods_Note(mod, "%s: \"bust\" has a list of \"expressions\"", where);
        } else {
            for (item = Json_At(list, 0), i = 0; item; item = Json_Next(item), i++) {
                snprintf(at, sizeof(at), "%s bust expressions[%d]", where, i);
                if (one.count == BUST_EXPRESSIONS) {
                    Mods_Note(mod, "%s: a bust has at most %d expressions; the rest are left out", at,
                              BUST_EXPRESSIONS);
                    break;
                }
                if (!read_expression(mod, directory, at, item, &one.expressions[one.count])) continue;
                for (k = 0; k < one.count && !same_word(one.expressions[k].name, one.expressions[one.count].name); k++) {}
                if (k < one.count) {
                    Mods_Note(mod, "%s: there is an expression \"%s\" already; this one is left out", at,
                              one.expressions[k].name);
                    continue;
                }
                one.count++;
            }
        }
    } else {
        Mods_Note(mod, "%s: \"bust\" is {\"default\": ..., \"expressions\": [...]}, or a PNG", where);
    }
    if (!one.count) {
        if (Json_TypeOf(value) == JSON_OBJECT || Json_TypeOf(value) == JSON_STRING)
            Mods_Note(mod, "%s: the bust has no expression with a picture; the duelist keeps the one it had", where);
        free(one.expressions);
        return -1;
    }
    chosen = Json_TypeOf(value) == JSON_OBJECT ? Json_String(Json_Member(value, "default"), NULL) : NULL;
    one.chosen = 0;
    if (Json_TypeOf(value) == JSON_OBJECT && !chosen) {
        Mods_Note(mod, "%s: a bust names its \"default\" expression; \"%s\", the first, is taken", where,
                  one.expressions[0].name);
    } else if (chosen) {
        for (k = 0; k < one.count && !same_word(one.expressions[k].name, chosen); k++) {}
        if (k == one.count)
            Mods_Note(mod, "%s: the bust has no expression \"%s\"; \"%s\", the first, is its default", where, chosen,
                      one.expressions[0].name);
        else
            one.chosen = k;
    }
    one.order[0] = one.chosen;
    for (i = 0, k = 1; i < one.count; i++) {
        if (i != one.chosen) one.order[k++] = i;
    }
    if (bust_count == bust_room) {
        int room = bust_room ? bust_room * 2 : 8;
        Bust *grown = realloc(busts, (size_t)room * sizeof(*grown));
        if (!grown) {
            free(one.expressions);
            return -1;
        }
        busts = grown;
        bust_room = room;
    }
    busts[bust_count] = one;
    return bust_count++;
}

void Busts_Clear(void)
{
    int i;
    for (i = 0; i < bust_count; i++) free(busts[i].expressions);
    free(busts);
    busts = NULL;
    bust_count = bust_room = 0;
    for (i = 0; i < record_count; i++) {
        free(records[i].slot);
        free(records[i].bank);
    }
    memset(records, 0, sizeof(records));
    record_count = 0;
    memset(replacing, 0, sizeof(replacing));
}

/* --- what the game asks ----------------------------------------------------- */

static const Bust *bust_at(int bust)
{
    return bust >= 0 && bust < bust_count ? &busts[bust] : NULL;
}

int Busts_Count(int bust)
{
    const Bust *one = bust_at(bust);
    return one ? one->count : 0;
}

const char *Busts_Name(int bust, int expression)
{
    const Bust *one = bust_at(bust);
    return one && expression >= 0 && expression < one->count ? one->expressions[expression].name : NULL;
}

int Busts_Default(int bust)
{
    const Bust *one = bust_at(bust);
    return one ? one->chosen : -1;
}

int Busts_Named(int bust, const char *name)
{
    const Bust *one = bust_at(bust);
    int k;
    if (!one || !name) return -1;
    for (k = 0; k < one->count; k++) {
        if (same_word(one->expressions[k].name, name)) return k;
    }
    return -1;
}

int Busts_Ordered(int bust, int n)
{
    const Bust *one = bust_at(bust);
    if (!one) return -1;
    return n >= 0 && n < one->count ? one->order[n] : one->chosen;
}

int Busts_Place(int bust, int expression, int *record, int *slot)
{
    const Bust *one = bust_at(bust);
    if (!one || expression < 0 || expression >= one->count || one->expressions[expression].record < 0) return 0;
    *record = one->expressions[expression].record;
    *slot = one->expressions[expression].slot;
    return 1;
}

int Busts_Character(int bust)
{
    const Bust *one = bust_at(bust);
    return one ? one->character : 0;
}

const unsigned char *Busts_Record(int record, const unsigned short **clut, const unsigned char **bank)
{
    if (record < 0 || record >= record_count || !records[record].slot) return NULL;
    *clut = (const unsigned short *)(records[record].slot + BUST_SHEET_W * BUST_SHEET_H);
    *bank = records[record].bank;
    return records[record].slot;
}

int Busts_Replacing(int character)
{
    return character > 0 && character < CHARACTERS ? replacing[character] - 1 : -1;
}

/* --- the record maker ----------------------------------------------------- */

typedef struct {
    int dx, dy, u, v, w, h;
} Part;

typedef struct {
    int u, v, w, h;
} Cut;

typedef struct {
    unsigned char *sheet, *bank;
    int w, h, least;                  /* the pictures' size; the smallest square */
    unsigned char used[CELLS][CELLS]; /* the sheet's 8 x 8 cells taken, [row][column] */
    int at;                           /* the bank's next byte */
    int full;                         /* something did not fit */
    Cut cuts[1024];                   /* the squares cut, for one the same */
    int cut_count;
} Packer;

static int same_area(const unsigned char *a, int a_pitch, const unsigned char *b, int b_pitch, int w, int h)
{
    int y;
    for (y = 0; y < h; y++) {
        if (memcmp(a + y * a_pitch, b + y * b_pitch, (size_t)w)) return 0;
    }
    return 1;
}

static int clear_area(const unsigned char *a, int pitch, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (a[y * pitch + x]) return 0;
        }
    }
    return 1;
}

/* Room on the sheet for a w x h square of `picture` (at x, y of it): the
 * same one cut before, or the first free place; 0 when there is none. */
static int cut(Packer *p, const unsigned char *picture, int x, int y, int w, int h, int *u, int *v)
{
    const unsigned char *from = picture + y * p->w + x;
    int cw = w / 8, ch = h / 8, step_x = cw < 4 ? cw : 4, step_y = ch < 4 ? ch : 4, cx, cy, i, j, k;
    for (k = 0; k < p->cut_count; k++) {
        const Cut *c = &p->cuts[k];
        if (c->w == w && c->h == h && same_area(p->sheet + c->v * BUST_SHEET_W + c->u, BUST_SHEET_W, from, p->w, w, h)) {
            *u = c->u;
            *v = c->v;
            return 1;
        }
    }
    if (p->cut_count == (int)(sizeof(p->cuts) / sizeof(p->cuts[0]))) return 0;
    for (cy = 0; cy + ch <= CELLS; cy += step_y) {
        for (cx = 0; cx + cw <= CELLS; cx += step_x) {
            int free_here = 1;
            for (j = 0; j < ch && free_here; j++) {
                for (i = 0; i < cw && free_here; i++) free_here = !p->used[cy + j][cx + i];
            }
            if (!free_here) continue;
            for (j = 0; j < ch; j++) {
                for (i = 0; i < cw; i++) p->used[cy + j][cx + i] = 1;
            }
            for (j = 0; j < h; j++) memcpy(p->sheet + (cy * 8 + j) * BUST_SHEET_W + cx * 8, from + j * p->w, (size_t)w);
            p->cuts[p->cut_count].u = *u = cx * 8;
            p->cuts[p->cut_count].v = *v = cy * 8;
            p->cuts[p->cut_count].w = w;
            p->cuts[p->cut_count].h = h;
            p->cut_count++;
            return 1;
        }
    }
    return 0;
}

static void add_part(Packer *p, Part *parts, int *count, int x, int y, int u, int v, int w, int h)
{
    if (*count == FRAME_PARTS) {
        p->full = 1;
        return;
    }
    parts[*count].dx = x - p->w / 2;
    parts[*count].dy = y - (p->h < ANCHOR_Y ? p->h : ANCHOR_Y);
    parts[*count].u = u;
    parts[*count].v = v;
    parts[*count].w = w;
    parts[*count].h = h;
    (*count)++;
}

/* The square at x, y of side `side` of `picture`: against the first
 * expression's (`body`: the same is the first's own part, on the sheet at
 * the same place), or against its expression's picture (`under`: the same
 * is nothing to draw). A clear square is nothing either way: a part drawn
 * over the body cannot clear it. */
static void cover(Packer *p, const unsigned char *picture, const unsigned char *under, int body, int x, int y,
                  int side, Part *parts, int *count)
{
    int w = p->w - x < side ? p->w - x : side, h = p->h - y < side ? p->h - y : side, u, v;
    const unsigned char *here = picture + y * p->w + x, *there = under + y * p->w + x;
    if (w <= 0 || h <= 0 || p->full) return;
    if (clear_area(here, p->w, w, h)) return;
    if (same_area(here, p->w, there, p->w, w, h)) {
        if (body) add_part(p, parts, count, x, y, x, y, w, h);
        return;
    }
    if (side > p->least) {
        int half = side / 2;
        cover(p, picture, under, body, x, y, half, parts, count);
        cover(p, picture, under, body, x + half, y, half, parts, count);
        cover(p, picture, under, body, x, y + half, half, parts, count);
        cover(p, picture, under, body, x + half, y + half, half, parts, count);
        return;
    }
    if (!cut(p, picture, x, y, w, h, &u, &v)) {
        p->full = 1;
        return;
    }
    add_part(p, parts, count, x, y, u, v, w, h);
}

static int put8(Packer *p, int value)
{
    if (p->at >= BUST_BANK) {
        p->full = 1;
        return 0;
    }
    p->bank[p->at++] = (unsigned char)value;
    return 1;
}

static void put16_at(Packer *p, int at, int value)
{
    if (at < 0 || at + 1 >= BUST_BANK) {
        p->full = 1;
        return;
    }
    p->bank[at] = (unsigned char)value;
    p->bank[at + 1] = (unsigned char)(value >> 8);
}

static int put_frame(Packer *p, const Part *parts, int count)
{
    int start = p->at, k;
    put8(p, count);
    put8(p, 0x10);                   /* ten-bit offsets, as the disc's characters */
    put8(p, 0);
    put8(p, 0);
    for (k = 0; k < count; k++) {
        const Part *part = &parts[k];
        unsigned cell = (unsigned)(part->u / 8) | (unsigned)(part->v / 8) << 5 | ((unsigned)(part->dx >> 8) & 3) << 14;
        unsigned size = (unsigned)(part->w / 8 - 1) << 5 | (unsigned)(part->h / 8 - 1) << 9 |
                        ((unsigned)(part->dy >> 8) & 3) << 14;
        put8(p, part->dx & 0xFF);
        put8(p, part->dy & 0xFF);
        put8(p, (int)(cell & 0xFF));
        put8(p, (int)(cell >> 8));
        put8(p, (int)(size & 0xFF));
        put8(p, (int)(size >> 8));
    }
    return start;
}

/* A stream of (frames, frame) steps, then the stop. */
static int put_stream(Packer *p, const int *steps, int count)
{
    int start = p->at, k;
    for (k = 0; k < count; k++) {
        put8(p, steps[k * 2]);
        put8(p, steps[k * 2 + 1] & 0xFF);
        put8(p, steps[k * 2 + 1] >> 8);
    }
    put8(p, 0xFF);
    return start;
}

static int pack(Packer *p, const BustFrames *frames, int count)
{
    static Part parts[FRAME_PARTS];
    int level2 = 8, level3 = level2 + count * 6, e, x, y, n, empty;
    memset(p->used, 0, sizeof(p->used));
    memset(p->bank, 0, BUST_BANK);
    p->cut_count = 0;
    p->full = 0;
    /* The first expression's picture whole, at the top left. */
    for (y = 0; y < p->h; y++) memcpy(p->sheet + y * BUST_SHEET_W, frames[0].base + y * p->w, (size_t)p->w);
    for (y = 0; y < p->h / 8; y++) {
        for (x = 0; x < p->w / 8; x++) p->used[y][x] = 1;
    }
    p->at = level3 + count * 22;
    empty = put_frame(p, parts, 0);
    for (e = 0; e < BUST_IN_PLACE; e++) put16_at(p, e * 2, level2 + (e < count ? e : 0) * 6);
    for (e = 0; e < count && !p->full; e++) {
        const BustFrames *f = &frames[e];
        int table = level3 + e * 22, body, k, held[3], talk[BUST_TALK_FRAMES], talks = 0, steps[8];
        /* The body: the first's squares where they are the same. */
        n = 0;
        for (y = 0; y < p->h; y += 128) {
            for (x = 0; x < p->w; x += 128) cover(p, f->base, frames[0].base, 1, x, y, 128, parts, &n);
        }
        body = put_frame(p, parts, n);
        steps[0] = 0;
        steps[1] = body;
        put16_at(p, level2 + e * 6, table);
        put16_at(p, table, put_stream(p, steps, 1));
        /* The eyes: shut by the blink frame's changes. */
        if (f->blink) {
            n = 0;
            for (y = 0; y < p->h; y += 128) {
                for (x = 0; x < p->w; x += 128) cover(p, f->blink, f->base, 0, x, y, 128, parts, &n);
            }
            if (n) {
                int shut = put_frame(p, parts, n);
                steps[0] = 0;
                steps[1] = empty;
                put16_at(p, level2 + e * 6 + 2, table + 2);
                put16_at(p, table + 2, put_stream(p, steps, 1));
                steps[0] = BLINK_FRAMES;
                steps[1] = shut;
                put16_at(p, table + 4, put_stream(p, steps, 1));
            }
        }
        /* The mouth: a talking frame's changes for each shape the words
         * ask for. */
        for (k = 0; k < f->talk_count; k++) {
            n = 0;
            for (y = 0; y < p->h; y += 128) {
                for (x = 0; x < p->w; x += 128) cover(p, f->talk[k], f->base, 0, x, y, 128, parts, &n);
            }
            if (n) talk[talks++] = put_frame(p, parts, n);
        }
        if (talks) {
            int closed, open[2];
            steps[0] = 0;
            steps[1] = empty;
            closed = put_stream(p, steps, 1);
            for (k = 0; k < 2; k++) {
                steps[1] = talk[k % talks];
                open[k] = put_stream(p, steps, 1);
            }
            held[0] = closed;
            held[1] = open[0];
            held[2] = open[1];
            put16_at(p, level2 + e * 6 + 4, table + 6);
            for (k = 0; k < MOUTH_VARIANTS; k++) put16_at(p, table + 6 + k * 2, k < 3 ? held[k] : closed);
            {   /* variant 3: through the frames and closed again */
                int played[2 * (BUST_TALK_FRAMES + 2)], m = 0;
                for (k = 0; k < (talks < 2 ? 2 : talks); k++) {
                    played[m * 2] = TALK_FRAMES;
                    played[m * 2 + 1] = talk[k % talks];
                    m++;
                }
                played[m * 2] = TALK_FRAMES;
                played[m * 2 + 1] = empty;
                m++;
                put16_at(p, table + 6 + 3 * 2, put_stream(p, played, m));
            }
        }
    }
    return !p->full;
}

static Packer packer;

int Busts_Pack(int w, int h, const BustFrames *frames, int count, unsigned char *sheet, unsigned char *bank)
{
    static const int least[] = {8, 16, 32, 64, 128};
    size_t k;
    if (count < 1 || count > BUST_IN_PLACE || w < 8 || h < 8 || w > BUST_SIDE || h > BUST_SIDE || (w & 7) || (h & 7))
        return 0;
    packer.sheet = sheet;
    packer.bank = bank;
    packer.w = w;
    packer.h = h;
    for (k = 0; k < sizeof(least) / sizeof(least[0]); k++) {
        memset(sheet, 0, BUST_SHEET_W * BUST_SHEET_H);
        packer.least = least[k];
        if (pack(&packer, frames, count)) return 1;
    }
    return 0;
}

/* --- making a bust -------------------------------------------------------- */

/* Expression `e`'s frames, from the bust's pictures. */
static BustFrames frames_of(const Bust *one, int e, unsigned char *const *pictures, const int *first, int talks,
                            int blink)
{
    BustFrames f;
    const Expression *x = &one->expressions[e];
    int k, at = first[e];
    memset(&f, 0, sizeof(f));
    f.base = pictures[at++];
    for (k = 0; k < x->talk_count; k++, at++) {
        if (k < talks) f.talk[f.talk_count++] = pictures[at];
    }
    if (x->blink[0] && blink) f.blink = pictures[at];
    return f;
}

static int keep_record(const unsigned char *sheet, const unsigned char *bank, const unsigned short *clut)
{
    unsigned short *rows;
    int k;
    if (record_count == BUST_RECORDS) return -1;
    records[record_count].slot = malloc(BUST_SLOT);
    records[record_count].bank = malloc(BUST_BANK);
    if (!records[record_count].slot || !records[record_count].bank) {
        free(records[record_count].slot);
        free(records[record_count].bank);
        records[record_count].slot = records[record_count].bank = NULL;
        return -1;
    }
    memcpy(records[record_count].slot, sheet, BUST_SHEET_W * BUST_SHEET_H);
    memcpy(records[record_count].bank, bank, BUST_BANK);
    /* The two rows of colors after the texels: as is, then
     * semi-transparent, as every disc record has them. */
    rows = (unsigned short *)(records[record_count].slot + BUST_SHEET_W * BUST_SHEET_H);
    for (k = 0; k < 256; k++) {
        rows[k] = clut[k];
        rows[256 + k] = clut[k] ? (unsigned short)(clut[k] | 0x8000) : 0;
    }
    return record_count++;
}

/* What an expression lost to fit a record alone: said exactly, with the
 * room its picture leaves (notes/more-duelists.md, the picture's size). */
static void note_dropped(const Bust *one, const Expression *x, int talks, int blink, int cw, int ch)
{
    char lost[96], does[64];
    int had_blink = x->blink[0] != 0, cut_talks = x->talk_count - talks, free_cells = CELLS * CELLS - cw / 8 * (ch / 8);
    lost[0] = does[0] = 0;
    if (had_blink && !blink) snprintf(lost, sizeof(lost), "its blink frame");
    if (cut_talks == 1 && x->talk_count == 1)
        snprintf(lost + strlen(lost), sizeof(lost) - strlen(lost), "%sits talking frame", lost[0] ? " and " : "");
    else if (cut_talks && !talks)
        snprintf(lost + strlen(lost), sizeof(lost) - strlen(lost), "%sall %d talking frames", lost[0] ? " and " : "",
                 x->talk_count);
    else if (cut_talks == 1)
        snprintf(lost + strlen(lost), sizeof(lost) - strlen(lost), "%stalking frame %d", lost[0] ? " and " : "",
                 x->talk_count);
    else if (cut_talks)
        snprintf(lost + strlen(lost), sizeof(lost) - strlen(lost), "%stalking frames %d-%d", lost[0] ? " and " : "",
                 talks + 1, x->talk_count);
    if (had_blink && !blink) snprintf(does, sizeof(does), "never blinks");
    if (x->talk_count && !talks)
        snprintf(does + strlen(does), sizeof(does) - strlen(does), "%snever talks", does[0] ? " and " : "");
    else if (cut_talks)
        snprintf(does + strlen(does), sizeof(does) - strlen(does), "%stalks with %d frame%s", does[0] ? " and " : "",
                 talks, talks == 1 ? "" : "s");
    Mods_Note(one->mod, "%s: expression \"%s\" has too much to change to fit the game's record: it is made without "
              "%s, so it %s (its %d x %d picture leaves %d of the page's %d 8 x 8 squares for its frames' changes)",
              one->where, x->name, lost, does, cw, ch, free_cells, CELLS * CELLS);
}

static void make(Bust *one)
{
    static unsigned char sheet[BUST_SHEET_W * BUST_SHEET_H], bank[BUST_BANK], best_sheet[BUST_SHEET_W * BUST_SHEET_H],
        best_bank[BUST_BANK];
    const char *paths[BUST_EXPRESSIONS * (BUST_TALK_FRAMES + 2)];
    unsigned char *pictures[BUST_EXPRESSIONS * (BUST_TALK_FRAMES + 2)] = {0}, *loaded = NULL, *canvas = NULL;
    unsigned short clut[256];
    int first[BUST_EXPRESSIONS], n = 0, e, k, pw, ph, w, h, cw, ch, done;
    char why[256];
    one->made = 1;
    for (e = 0; e < one->count; e++) {
        const Expression *x = &one->expressions[e];
        first[e] = n;
        paths[n++] = x->image;
        for (k = 0; k < x->talk_count; k++) paths[n++] = x->talk[k];
        if (x->blink[0]) paths[n++] = x->blink;
    }
    /* Every picture at the default's size, shrunk to fit the page. */
    if (!CardArt_ImageSize(one->expressions[one->chosen].image, &pw, &ph) || pw < 1 || ph < 1) return;
    w = pw;
    h = ph;
    if (w > BUST_SIDE || h > BUST_SIDE) {
        if (w >= h) {
            h = (int)((long)h * BUST_SIDE / w);
            w = BUST_SIDE;
        } else {
            w = (int)((long)w * BUST_SIDE / h);
            h = BUST_SIDE;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    /* Never otherwise; said so GCC 16 -O3 sees the sizes below are small. */
    if (w < 1 || h < 1 || w > BUST_SIDE || h > BUST_SIDE || n < 1) return;
    cw = (w + 7) & ~7;
    ch = (h + 7) & ~7;
    for (k = 0; k < n; k++) {
        int fw, fh;
        if (CardArt_ImageSize(paths[k], &fw, &fh) && (fw != pw || fh != ph))
            Mods_Note(one->mod, "%s: %s is %d x %d, the default expression's picture %d x %d; it is stretched to it",
                      one->where, paths[k], fw, fh, pw, ph);
    }
    loaded = malloc((size_t)n * w * h);
    canvas = calloc((size_t)n, (size_t)cw * ch);
    if (!loaded || !canvas) {
        free(loaded);
        free(canvas);
        return;
    }
    for (k = 0; k < n; k++) pictures[k] = loaded + (size_t)k * w * h;
    if (!CardArt_IndexedImages(paths, n, w, h, pictures, clut, why, sizeof(why))) {
        Mods_Note(one->mod, "%s: %s", one->where, why);
        free(loaded);
        free(canvas);
        return;
    }
    for (k = 0; k < n; k++) {
        int y;
        for (y = 0; y < h; y++) memcpy(canvas + (size_t)k * cw * ch + (size_t)y * cw, pictures[k] + (size_t)y * w, (size_t)w);
        pictures[k] = canvas + (size_t)k * cw * ch;
    }
    /* Into records, in the order the codes number them: as many to a
     * record as fit, up to four. */
    for (done = 0; done < one->count;) {
        BustFrames group[BUST_IN_PLACE];
        int fitted = 0, talks = BUST_TALK_FRAMES, blink = 1, record;
        while (fitted < BUST_IN_PLACE && done + fitted < one->count) {
            group[fitted] = frames_of(one, one->order[done + fitted], pictures, first, talks, blink);
            if (!Busts_Pack(cw, ch, group, fitted + 1, sheet, bank)) break;
            memcpy(best_sheet, sheet, sizeof(sheet));
            memcpy(best_bank, bank, sizeof(bank));
            fitted++;
        }
        if (!fitted) {
            /* Alone it does not fit: without its blink, then with fewer
             * talking frames, the last first. */
            const Expression *x = &one->expressions[one->order[done]];
            blink = x->blink[0] != 0;
            talks = x->talk_count;
            while (!fitted && (blink || talks)) {
                if (blink) blink = 0;
                else talks--;
                group[0] = frames_of(one, one->order[done], pictures, first, talks, blink);
                if (Busts_Pack(cw, ch, group, 1, sheet, bank)) {
                    memcpy(best_sheet, sheet, sizeof(sheet));
                    memcpy(best_bank, bank, sizeof(bank));
                    fitted = 1;
                    note_dropped(one, x, talks, blink, cw, ch);
                }
            }
        }
        if (!fitted) {
            Mods_Note(one->mod, "%s: expression \"%s\" does not fit the game's record; it is left out", one->where,
                      one->expressions[one->order[done]].name);
            done++;
            continue;
        }
        record = keep_record(best_sheet, best_bank, clut);
        if (record < 0) {
            Mods_Note(one->mod, "%s: the mods' busts have %d records at most; the rest of this one is left out",
                      one->where, BUST_RECORDS);
            break;
        }
        if (one->first < 0) one->first = record;
        for (k = 0; k < fitted; k++) {
            one->expressions[one->order[done + k]].record = record;
            one->expressions[one->order[done + k]].slot = k;
        }
        LOG(LOG_MODS, "busts: %s %s: record %d holds %d expressions (%d x %d)", one->mod, one->where, record, fitted,
            cw, ch);
        done += fitted;
    }
    free(loaded);
    free(canvas);
}

void Busts_Make(void)
{
    int duelist, bust, count = Duelists_Count(), made = 0;
    clock_t start = clock();
    for (duelist = 1; duelist < count; duelist++) {
        bust = Duelists_Bust(duelist);
        Loading_Step("Duelist portraits", (unsigned)duelist, (unsigned)count);
        if (bust >= 0 && bust < bust_count && !busts[bust].made) {
            make(&busts[bust]);
            made++;
        }
    }
    /* What it cost the start (MEMORIES_TRACE=mods). */
    if (made)
        LOG(LOG_MODS, "busts: %d made into %d records in %ld ms", made, record_count,
            (long)((clock() - start) * 1000 / CLOCKS_PER_SEC));
    /* The campaign's characters a disc duelist's "replace_bust" takes over:
     * the character its own dialogue brings in (Tower_DuelistCharacter),
     * whose record the bust's first is from then on, wherever the campaign
     * or a story shows it. The busts go in the order they were read -- the
     * mods' load order, then each mod's list -- so of two replacing one
     * character (Seto and Seto 3rd are both character 4) the later mod's
     * has it, as a later mod's replacement of anything does. */
    for (bust = 0; bust < bust_count; bust++) {
        Bust *one = &busts[bust];
        const char *name;
        int character, k;
        for (duelist = 1; duelist < DUELISTS_RETAIL_COUNT &&
                          !(Duelists_Bust(duelist) == bust && Duelists_ReplacesBust(duelist)); duelist++) {}
        if (duelist == DUELISTS_RETAIL_COUNT) continue;
        name = Duelists_Name(duelist) ? Duelists_Name(duelist) : "the duelist";
        character = Tower_DuelistCharacter(duelist);
        if (!character) {
            Mods_Note(one->mod, "%s: replace_bust: the campaign shows %s as no character, so nothing changes there",
                      one->where, name);
            continue;
        }
        if (one->first < 0) continue;
        for (k = 0; k < bust; k++) {
            Bust *other = &busts[k];
            const char *loser;
            if (other->character != character) continue;
            loser = Duelists_Name(other->duelist) ? Duelists_Name(other->duelist) : "a duelist";
            Mods_Note(other->mod, "%s: replace_bust: campaign character %d is %s's bust from mod %s instead (later "
                      "in load order); %s's bust here is not shown in the campaign", other->where, character, name,
                      one->mod, loser);
            Mods_Note(one->mod, "%s: replace_bust: campaign character %d was %s's bust from mod %s (earlier in load "
                      "order); %s's bust here has it instead", one->where, character, loser, other->mod, name);
            other->character = 0;
        }
        replacing[character] = one->first + 1;
        one->character = character;
        one->duelist = duelist;
        LOG(LOG_MODS, "busts: campaign character %d is %s's bust (record %d)", character, name, one->first);
    }
}
