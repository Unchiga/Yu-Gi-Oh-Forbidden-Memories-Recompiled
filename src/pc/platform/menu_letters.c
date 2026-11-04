/* A menu button of words in the game's own letters (menu_letters.h).
 *
 * The title's entries are sprite sheets (src/overlays/main_menu/frontend.c:
 * entry i is resource variant 2i with the cursor on it, 2i + 1 without):
 * a frame, and the word on it, sometimes one sprite, sometimes several.
 * With the main menu's package loaded, each entry's sheet is worked out
 * for both variants as the game does (DisplayObject_UpdateCommandStream),
 * its sprites taken as the game would sort them and read out of VRAM
 * through their palettes, back to front.
 *
 * The letters are then found column by column between the frame's two
 * lines: without the cursor they are light grey with clean gaps between
 * them (a gap inside a letter, where a stroke is thin, is the narrowest and
 * merged away until the word has its count); with the cursor they are bold
 * green and touch, so their places come from the plain look's, scaled to
 * the bold word's width, each cut moved to the column with the least ink
 * near it. A word is set from those letters, each with its share of the
 * space round it, on the frame of the entry the button stands in for (or
 * SAVE's, the shortest, for a button of a mod's own), the frame's empty
 * margin repeated where the word is longer. A letter no entry has makes
 * this give up, and the label is drawn as menu_label.c draws one. */
#include "menu_letters.h"
#include "menu_label.h"
#include "paths.h"
#include "types.h"
#include "game/display_object.h"
#include "game/display_object_helpers.h"
#include "game/display_object_config.h"
#include "game/display_object_render_sprite_sheet.h"
#include "game/display_object_update_command_stream.h"
#include "game/ordering_tables.h"
#include "pc/cards/duel_ui.h"
#include "pc/render/soft_gpu.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "overlays/main_menu/frontend.h"
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/fs.h"

enum { ENTRIES = 11, MAX_W = 256, MAX_H = 48, MAX_LETTERS = 16, MAX_RUNS = 48 };

/* The retail entries' words (English), as their sprites have them. */
static const char *const words[ENTRIES] = {"NEW GAME", "LOAD", "2P DUEL", "TRADE", "OPTION",
                                           "CAMPAIGN", "FREE DUEL", "BUILD DECK", "LIBRARY", "PASSWORD", "SAVE"};

typedef struct {
    int w, h;
    uint32_t px[MAX_W * MAX_H];     /* R, G, B, A bytes, as png wants them */
    int top, bottom;                /* the rows between the frame's two lines */
    int letters, cut[MAX_LETTERS][2]; /* each letter's columns with its share of the space round it */
    int good[MAX_LETTERS];          /* the letter's word could be cut into its letters */
    int spacing;                    /* how far apart its letters stand: the roomier, the cleaner the cut */
    int first, last;                /* the word's first column and the one past its last */
    int ink[MAX_LETTERS][2];        /* each letter's own inked columns, inside its cut */
} Entry;

static Entry entries[2][ENTRIES]; /* [selected][entry] */
/* How light a plain letter's pixel is at least (R + G + B): raised, word
 * by word, until letters that touch at their thin strokes come apart. */
static int threshold = 330;
static int waiting, tries;

int MenuLetters_Waiting(void)
{
    return waiting;
}

static uint32_t texel(int mode, int tpage, int cx, int cy, int u, int v)
{
    const uint16_t *vram = SoftGpu_Vram();
    int tx = (tpage & 0xF) * 64, ty = ((tpage >> 4) & 1) * 256;
    uint16_t c;
    if (mode == 0) {
        int index = (vram[((ty + v) & 511) * SOFT_GPU_WIDTH + ((tx + u / 4) & 1023)] >> ((u & 3) * 4)) & 0xF;
        c = vram[(cy & 511) * SOFT_GPU_WIDTH + ((cx + index) & 1023)];
    } else if (mode == 1) {
        int index = (vram[((ty + v) & 511) * SOFT_GPU_WIDTH + ((tx + u / 2) & 1023)] >> ((u & 1) * 8)) & 0xFF;
        c = vram[(cy & 511) * SOFT_GPU_WIDTH + ((cx + index) & 1023)];
    } else {
        c = vram[((ty + v) & 511) * SOFT_GPU_WIDTH + ((tx + u) & 1023)];
    }
    if (!c) return 0;
    /* R, G, B, A bytes */
    return 0xFF000000u | (uint32_t)((c >> 10 & 31) * 255 / 31) << 16 | (uint32_t)((c >> 5 & 31) * 255 / 31) << 8 |
           (uint32_t)((c & 31) * 255 / 31);
}

#define RED(p) ((int)((p) & 0xFF))
#define GREEN(p) ((int)((p) >> 8 & 0xFF))
#define BLUE(p) ((int)((p) >> 16 & 0xFF))

/* A letter's pixel: light grey without the cursor, the bright green core
 * with it. */
static int inked(uint32_t p, int selected)
{
    if (!(p >> 24)) return 0;
    if (selected) return GREEN(p) > 200 && GREEN(p) > RED(p) + 30;
    return RED(p) + GREEN(p) + BLUE(p) > threshold && abs(RED(p) - BLUE(p)) < 40;
}

/* A line's pixel: the grey lines without the cursor, the white ones with. */
static int lined(uint32_t p, int selected)
{
    if (!(p >> 24)) return 0;
    if (selected) return RED(p) > 200 && GREEN(p) > 200 && BLUE(p) > 200;
    return RED(p) + GREEN(p) + BLUE(p) > 300 && abs(RED(p) - BLUE(p)) < 40;
}

/* Entry `i` as variant `selected`: its sprites, read out of VRAM. */
static int capture(int i, int selected, Entry *out)
{
    DisplayObject *object = (DisplayObject *)gMain_apMenuEntries[i];
    DisplayObjectCapture caught;
    DisplayObject saved;
    int n, x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20), x, y;
    memset(out, 0, sizeof(*out));
    if (!object || !object->field_54) return 0;
    /* The variant's sheet worked out from the object's resource as the game
     * does when it is set, drawn into the capture, and the object put back
     * as it was, every byte. */
    memcpy(&saved, object, sizeof(saved));
    DisplayObject_SetResourceVariant((DisplayObjectConfig *)object, i * 2 + !selected);
    DisplayObject_UpdateCommandStream(object, 0, 0);
    object->field_30.h.field_30 = 160;
    object->field_30.h.field_32 = 120;
    caught.count = 0;
    if (object->field_4C) {
        DisplayObject_Capture = &caught;
        DisplayObject_RenderSpriteSheet(object, (s32)D_800E9D90[1], D_8009AF74[1]);
        DisplayObject_Capture = NULL;
    }
    memcpy(object, &saved, sizeof(saved));
    if (!caught.count) return 0;
    for (n = 0; n < caught.count; n++) {
        const GsSPRITE *s = (const GsSPRITE *)&caught.sprite[n];
        if (s->x < x0) x0 = s->x;
        if (s->y < y0) y0 = s->y;
        if (s->x + s->w > x1) x1 = s->x + s->w;
        if (s->y + s->h > y1) y1 = s->y + s->h;
    }
    out->w = x1 - x0;
    out->h = y1 - y0;
    if (out->w <= 0 || out->h <= 0 || out->w > MAX_W || out->h > MAX_H) {
        out->w = 0;
        return 0;
    }
    /* back to front: the sheet lists the word before the frame under it */
    for (n = caught.count - 1; n >= 0; n--) {
        const GsSPRITE *s = (const GsSPRITE *)&caught.sprite[n];
        int mode = (int)(s->attribute >> 24) & 3;
        int shade = (s->attribute & 0x40) || (s->r == 0x80 && s->g == 0x80 && s->b == 0x80);
        for (y = 0; y < s->h; y++) {
            for (x = 0; x < s->w; x++) {
                uint32_t p = texel(mode, s->tpage, s->cx, s->cy, s->u + x, s->v + y);
                if (!p) continue;
                if (!shade) {
                    int r = RED(p) * s->r / 0x80, g = GREEN(p) * s->g / 0x80, b = BLUE(p) * s->b / 0x80;
                    p = 0xFF000000u | (uint32_t)(b > 255 ? 255 : b) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 |
                        (uint32_t)(r > 255 ? 255 : r);
                }
                out->px[(s->y - y0 + y) * MAX_W + (s->x - x0 + x)] = p;
            }
        }
    }
    /* the frame's two lines: the rows lined most of the way across */
    out->top = -1;
    out->bottom = -1;
    {
        int best_top = out->w / 2, best_bottom = out->w / 2;
        for (y = 0; y < out->h; y++) {
            int count = 0;
            for (x = 0; x < out->w; x++) count += lined(out->px[y * MAX_W + x], selected);
            if (y < out->h / 3 && count > best_top) {
                best_top = count;
                out->top = y;
            } else if (y >= out->h * 2 / 3 && count > best_bottom) {
                best_bottom = count;
                out->bottom = y;
            }
        }
    }
    if (out->top < 0 || out->bottom < 0 || out->bottom - out->top < 8) {
        out->w = 0;
        return 0;
    }
    return 1;
}

/* How much ink column `x` has between the lines. */
static int ink_at(const Entry *e, int x, int selected)
{
    int y, n = 0;
    if (x < 0 || x >= e->w) return 0;
    for (y = e->top + 2; y < e->bottom - 1; y++) n += inked(e->px[y * MAX_W + x], selected);
    return n;
}

static int letters_in(const char *word)
{
    int n = 0;
    for (; *word; word++) n += *word != ' ';
    return n;
}

/* The plain look's letters: runs of columns with ink. The word's spaces
 * are its widest gaps; in each word the narrowest gaps (inside a letter,
 * where a stroke is thin) are merged away until it has its letters. A word
 * with fewer runs than letters (letters that touch) is left out. */
static int cut_plain_at(Entry *e, const char *word)
{
    int runs[MAX_RUNS][2], n = 0, x, k, want = letters_in(word), spaces = 0, gap_sum = 0, gaps = 0, half, from, letter;
    int space_at[8], used = 0;
    for (x = 0; x <= e->w; x++) {
        int ink = x < e->w && ink_at(e, x, 0) > 0;
        if (ink && (!n || runs[n - 1][1] >= 0)) {
            if (n >= MAX_RUNS) return 0;
            runs[n][0] = x;
            runs[n++][1] = -1;
        } else if (!ink && n && runs[n - 1][1] < 0) {
            runs[n - 1][1] = x;
        }
    }
    for (x = 0; word[x]; x++) spaces += word[x] == ' ';
    if (!n || want > MAX_LETTERS || !want || spaces > 7 || n < spaces + 1) return 0;
    /* the spaces: the widest gaps, in order */
    for (k = 0; k < spaces; k++) {
        int best = -1, j, q;
        for (j = 1; j < n; j++) {
            int taken = 0;
            for (q = 0; q < used; q++) taken |= space_at[q] == j;
            if (!taken && (best < 0 || runs[j][0] - runs[j - 1][1] > runs[best][0] - runs[best - 1][1])) best = j;
        }
        space_at[used++] = best;
    }
    for (k = 0; k < used; k++) { /* sorted */
        int j;
        for (j = k + 1; j < used; j++) {
            if (space_at[j] < space_at[k]) {
                int t = space_at[j];
                space_at[j] = space_at[k];
                space_at[k] = t;
            }
        }
    }
    /* each word: its runs merged down to its letters */
    e->letters = 0;
    from = 0;
    letter = 0;
    for (k = 0; k <= used; k++) {
        int to = k < used ? space_at[k] : n, count = to - from, letters = 0, w0 = letter, j;
        int word_runs[MAX_RUNS][2];
        while (word[letter] == ' ') letter++;
        while (word[letter] && word[letter] != ' ') {
            letters++;
            letter++;
        }
        memcpy(word_runs, &runs[from], sizeof(runs[0]) * (size_t)count);
        while (count > letters) {
            int best = 1;
            for (j = 2; j < count; j++)
                if (word_runs[j][0] - word_runs[j - 1][1] < word_runs[best][0] - word_runs[best - 1][1]) best = j;
            word_runs[best - 1][1] = word_runs[best][1];
            memmove(&word_runs[best], &word_runs[best + 1], sizeof(runs[0]) * (size_t)(count - best - 1));
            count--;
        }
        for (j = 0; j < letters; j++) {
            int ok = count == letters;
            if (e->letters >= MAX_LETTERS) return 0;
            /* a word that would not cut: its letters spread evenly, unused */
            e->cut[e->letters][0] = ok ? word_runs[j][0] : runs[from][0] + j;
            e->cut[e->letters][1] = ok ? word_runs[j][1] : runs[from][0] + j + 1;
            e->good[e->letters] = ok;
            if (ok && j) {
                gap_sum += word_runs[j][0] - word_runs[j - 1][1];
                gaps++;
            }
            e->letters++;
        }
        (void)w0;
        from = to;
    }
    if (e->letters != want) return 0;
    /* each letter keeps a quarter of the gap round it either side: the
     * roomy words' letters set closer, as the others' are */
    half = gaps ? (gap_sum + gaps) / (4 * gaps) : 1;
    half = half < 1 ? 1 : half;
    e->spacing = gaps ? gap_sum * 16 / gaps : 0;
    e->first = runs[0][0];
    e->last = runs[n - 1][1];
    for (k = 0; k < e->letters; k++) {
        e->cut[k][0] -= half;
        e->cut[k][1] += half;
    }
    return 1;
}

/* The plain look cut at the lowest threshold that cuts every word of it;
 * failing that, at the one that cuts the most letters. */
static int cut_plain(Entry *e, const char *word)
{
    int t, best = -1, best_good = -1, k;
    for (t = 330; t <= 505; t += 25) {
        int good = 0;
        threshold = t;
        if (!cut_plain_at(e, word)) continue;
        for (k = 0; k < e->letters; k++) good += e->good[k];
        if (good > best_good) {
            best_good = good;
            best = t;
        }
        if (good == e->letters) break;
    }
    threshold = best < 0 ? 330 : best;
    k = best >= 0 && cut_plain_at(e, word);
    threshold = 330;
    return k;
}

/* The cursor's look: the plain look's places, scaled to the bold word's
 * width, each cut moved to the least inked column within two of it. */
static int cut_bold(Entry *e, const Entry *plain)
{
    int x, k, first = -1, last = -1, span, plain_span;
    for (x = 0; x < e->w; x++) {
        if (ink_at(e, x, 1)) {
            if (first < 0) first = x;
            last = x + 1;
        }
    }
    if (first < 0 || !plain->letters) return 0;
    span = last - first;
    plain_span = plain->last - plain->first;
    if (plain_span <= 0) return 0;
    e->first = first;
    e->last = last;
    e->letters = plain->letters;
    memcpy(e->good, plain->good, sizeof(e->good));
    e->spacing = plain->spacing;
    for (k = 0; k < plain->letters; k++) {
        int side;
        for (side = 0; side < 2; side++) {
            int at = first + (plain->cut[k][side] - plain->first) * span / plain_span, best = at, d;
            for (d = -2; d <= 2; d++)
                if (ink_at(e, at + d, 1) < ink_at(e, best, 1)) best = at + d;
            e->cut[k][side] = best;
        }
        if (e->cut[k][1] <= e->cut[k][0]) return 0;
    }
    return 1;
}

/* Letters the cut cannot find, measured on the English disc's sprites
 * (2026-10-20): 2P DUEL's U, the only U on a word that does not run its
 * letters together elsewhere. Used only when the entry is that size. */
static const struct {
    int entry, letter, plain_w, bold_w, plain[2], bold[2];
} measured[] = {
    {2, 3, 64, 72, {33, 40}, {36, 44}}, /* 2P DUEL: U */
};

static void apply_measured(void)
{
    int k, j;
    for (k = 0; k < (int)(sizeof(measured) / sizeof(measured[0])); k++) {
        Entry *plain = &entries[0][measured[k].entry], *bold = &entries[1][measured[k].entry];
        int letters = letters_in(words[measured[k].entry]);
        if (plain->w != measured[k].plain_w || bold->w != measured[k].bold_w || plain->top < 0 || bold->top < 0)
            continue;
        if (plain->letters != letters || bold->letters != letters) {
            /* the word did not cut at all: only the measured letter, spread */
            plain->letters = bold->letters = letters;
            plain->spacing = bold->spacing = 0;
            for (j = 0; j < letters; j++) plain->good[j] = bold->good[j] = 0;
        }
        plain->cut[measured[k].letter][0] = measured[k].plain[0];
        plain->cut[measured[k].letter][1] = measured[k].plain[1];
        bold->cut[measured[k].letter][0] = measured[k].bold[0];
        bold->cut[measured[k].letter][1] = measured[k].bold[1];
        plain->good[measured[k].letter] = bold->good[measured[k].letter] = 1;
        plain->spacing = bold->spacing = 1 << 20; /* measured: before any cut of its letter */
    }
}

/* Each letter's own columns: the inked ones inside its cut (all of the
 * cut when it has none, or when letters touch and the cut is inked to its
 * edges). Set from these, letters stand as close as the word they are set
 * on has its own, wherever they were taken from. */
static void letter_ink(Entry *e, int selected)
{
    int k, x;
    for (k = 0; k < e->letters; k++) {
        int from = -1, to = -1;
        for (x = e->cut[k][0]; x < e->cut[k][1]; x++) {
            if (ink_at(e, x, selected)) {
                if (from < 0) from = x;
                to = x + 1;
            }
        }
        /* a column more either side for the letter's dark edge */
        e->ink[k][0] = from < 0 ? e->cut[k][0] : from - 1 > e->cut[k][0] ? from - 1 : e->cut[k][0];
        e->ink[k][1] = from < 0 ? e->cut[k][1] : to + 1 < e->cut[k][1] ? to + 1 : e->cut[k][1];
    }
}

/* How far apart the letters of `word` stand on entry `e`, ink to ink: the
 * middle one of its gaps inside a word (letters that touch give 0 or
 * less); `fallback` when it has none. */
static int letter_gap(const Entry *e, const char *word, int fallback)
{
    int gaps[MAX_LETTERS], n = 0, k, j, letter = 0;
    for (k = 0; word[k] && letter < e->letters; k++) {
        if (word[k] == ' ') continue;
        if (k > 0 && word[k - 1] != ' ' && letter > 0 && e->good[letter] && e->good[letter - 1])
            gaps[n++] = e->ink[letter][0] - e->ink[letter - 1][1];
        letter++;
    }
    if (!n) return fallback;
    for (k = 0; k < n; k++) { /* sorted */
        for (j = k + 1; j < n; j++) {
            if (gaps[j] < gaps[k]) {
                int t = gaps[j];
                gaps[j] = gaps[k];
                gaps[k] = t;
            }
        }
    }
    return gaps[n / 2];
}

/* Where the letter `c` is: of the entries cut into their words' letters
 * in both looks, the one whose letters stand furthest apart (the cleanest
 * cut), and the letter's place in it. */
static int find(char c, int *entry, int *at)
{
    int i, k, j, best = -1;
    for (i = 0; i < ENTRIES; i++) {
        const char *w = words[i];
        if (!entries[0][i].letters || !entries[1][i].letters) continue;
        for (k = 0, j = 0; w[k]; k++) {
            if (w[k] == ' ') continue;
            if (w[k] == c && entries[0][i].good[j] && (best < 0 || entries[0][i].spacing > entries[0][best].spacing)) {
                best = i;
                *entry = i;
                *at = j;
                break;
            }
            j++;
        }
    }
    return best >= 0;
}

static uint32_t pixel(const Entry *e, int x, int y)
{
    return x >= 0 && x < e->w && y >= 0 && y < e->h ? e->px[y * MAX_W + x] : 0;
}

int MenuLetters_Make(const char *text, int selected, int stand_in, int factor, char *path, size_t size, int *width,
                     int *height, char *why, size_t why_size)
{
    const Entry *frame;
    int i, k, n, left, right, w, h, dx, x, y, total, space, margin_from, gap;
    float middle[64], ratio;
    const Entry *plain;
    uint32_t *rgba, *big;
    char relative[128];
    png_image image;
    FILE *file;
    waiting = 0;
    for (i = 0, n = 0; i < ENTRIES; i++) {
        capture(i, 0, &entries[0][i]);
        capture(i, 1, &entries[1][i]);
        if (entries[0][i].w && entries[1][i].w && cut_plain(&entries[0][i], words[i]))
            cut_bold(&entries[1][i], &entries[0][i]);
        if (!entries[1][i].letters) entries[0][i].letters = 0;
        if (i == ENTRIES - 1) apply_measured();
        n += entries[0][i].w > 0;
    }
    for (i = 0; i < ENTRIES; i++) {
        letter_ink(&entries[0][i], 0);
        letter_ink(&entries[1][i], 1);
    }
    if (!n) {
        waiting = ++tries < 8;
        snprintf(why, why_size, "the game's entries are not drawn yet");
        return 0;
    }
    tries = 0;
    for (k = 0; text[k]; k++) {
        int e, at;
        if (text[k] != ' ' && !find(text[k], &e, &at)) {
            snprintf(why, why_size, "the game's entries have no letter '%c'", text[k]);
            return 0;
        }
    }
    if (stand_in < 0 || stand_in >= ENTRIES || !entries[0][stand_in].letters) stand_in = 10; /* SAVE */
    if (!entries[0][stand_in].letters) {
        for (stand_in = 0; stand_in < ENTRIES && !entries[0][stand_in].letters; stand_in++) {
        }
    }
    frame = &entries[selected][stand_in];
    /* the frame's word, with a column of room either side */
    left = frame->first - 1;
    right = frame->last + 1;
    /* The plain word: its letters as close as the frame's own word has
     * its letters, ink to ink, and a space two thirds of the frame word's
     * first letter. The cursor's bold word is the plain one's layout made
     * as much wider as the frame's bold word is than its plain one, each
     * bold letter on its plain letter's middle (bold letters touch, so
     * their own columns cannot tell how close they stand). */
    plain = &entries[0][stand_in];
    gap = letter_gap(plain, words[stand_in], 1);
    space = (plain->cut[0][1] - plain->cut[0][0]) * 2 / 3;
    total = 0;
    for (k = 0, n = 0; text[k] && n < 64; k++) {
        int e, at;
        if (text[k] == ' ') {
            total += space;
            continue;
        }
        find(text[k], &e, &at);
        middle[n] = total + (entries[0][e].ink[at][1] - entries[0][e].ink[at][0]) / 2.0f;
        total += entries[0][e].ink[at][1] - entries[0][e].ink[at][0];
        n++;
        if (text[k + 1] && text[k + 1] != ' ') total += gap;
    }
    ratio = 1;
    if (selected && plain->last > plain->first && frame->last > frame->first)
        ratio = (float)(frame->last - frame->first) / (float)(plain->last - plain->first);
    total = (int)(total * ratio + 0.5f);
    w = left + total + (frame->w - right);
    /* never narrower than the frame it stands in: a short word in the middle */
    if (w < frame->w) w = frame->w;
    w = (w + 1) & ~1;
    h = frame->h;
    if (w > MAX_W) {
        snprintf(why, why_size, "too long for the game's letters");
        return 0;
    }
    rgba = calloc((size_t)w * h, 4);
    if (!rgba) {
        snprintf(why, why_size, "out of memory");
        return 0;
    }
    /* the frame: its left, its empty margin (the two columns left of the
     * word) repeated under the whole word, its right */
    margin_from = left - 2;
    for (x = 0; x < w; x++) {
        int sx = x < left ? x : x >= w - (frame->w - right) ? right + x - (w - (frame->w - right))
                                                               : margin_from + (x - left) % 2;
        for (y = 0; y < h; y++) rgba[y * w + x] = pixel(frame, sx, y);
    }
    /* the letters, each its columns of its own entry, between the lines */
    dx = left + (w - (frame->w - right) - left - total) / 2;
    for (k = 0, n = 0; text[k] && n < 64; k++) {
        int e, at, sx, at_x;
        const Entry *from;
        if (text[k] == ' ') continue;
        find(text[k], &e, &at);
        from = &entries[selected][e];
        at_x = dx + (int)(middle[n++] * ratio - (from->ink[at][1] - from->ink[at][0]) / 2.0f + 0.5f);
        for (sx = from->ink[at][0]; sx < from->ink[at][1]; sx++, at_x++) {
            if (at_x < 0 || at_x >= w) continue;
            for (y = frame->top + 1; y < frame->bottom; y++) {
                /* the letter's own rows, lined up by the lines */
                uint32_t p = pixel(from, sx, from->top + (y - frame->top));
                if (p >> 24) rgba[y * w + at_x] = p;
            }
        }
    }
    *width = w;
    *height = h;
    {
        char tail[48];
        int t = 0;
        for (k = 0; text[k] && t < 40; k++) {
            char c = text[k];
            tail[t++] = (char)((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_');
        }
        tail[t] = 0;
        snprintf(relative, sizeof(relative), "cache/menu-labels/game-%s-%d-x%d-%s.png", selected ? "on" : "off",
                 stand_in, factor, tail);
    }
    if (Paths_User(path, size, relative) != 0) {
        free(rgba);
        snprintf(why, why_size, "the user directory's path is too long");
        return 0;
    }
    factor = factor < 1 ? 1 : factor;
    big = malloc((size_t)w * factor * h * factor * 4);
    if (!big) {
        free(rgba);
        snprintf(why, why_size, "out of memory");
        return 0;
    }
    for (y = 0; y < h * factor; y++)
        for (x = 0; x < w * factor; x++) big[(size_t)y * w * factor + x] = rgba[(y / factor) * w + x / factor];
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = (png_uint_32)(w * factor);
    image.height = (png_uint_32)(h * factor);
    image.format = PNG_FORMAT_RGBA;
    file = fopen(path, "wb");
    if (!file || !png_image_write_to_stdio(&image, file, 0, big, 0, NULL)) {
        snprintf(why, why_size, "cannot write %s", path);
        if (file) fclose(file);
        free(rgba);
        free(big);
        return 0;
    }
    fclose(file);
    free(rgba);
    free(big);
    return 1;
}
