/* Custom artwork for the cards mods add (cards.h, notes/more-cards.md).
 *
 * A card's art record on the disc (7 sectors in WA_MRG.MRG, read by
 * func_80029164) is, in its first 0x3060 bytes:
 *
 *   +0x0000  102 x 96 art, one byte per pixel      (indices 1-255)
 *   +0x2640  its CLUT, 256 BGR555 entries          (entry 0 unused, 0x8000)
 *   +0x2840  the title plate, 96 x 14 at 4 bits    (0 clear, 1 darkest ink to 7 faintest)
 *   +0x2AE0  40 x 32 thumbnail, one byte per pixel (indices 1-63)
 *   +0x2FE0  its CLUT, 64 BGR555 entries
 *
 * and the thumbnail block (+0x2AE0, 0x580 bytes) is also the card's own sector
 * the duel reads for the hand and field. This file makes those bytes from a
 * PNG: cropped to the shape, averaged down to the size, and reduced to the
 * colors by median cut. The plate is the card's name set in Times (the
 * system's; the retail plates' own face is not available as a font), or a
 * PNG the mod gives.
 *
 * The plate is drawn subtractively over the card's gold frame through a
 * fixed 16-entry CLUT (index 1 a light grey, 7 a dark one), so index 1 takes
 * the most away: it is the darkest ink, and 7 barely shows. The retail
 * plates are authored that way, stems at 1 with faint 6 and 7 fringes. The
 * measurements, and the settings below that make a legible plate (Times
 * regular at 13 pixels, baseline under row 11, whole-pixel advances), are the
 * YuGiOhForbiddenMemoriesRecomp project's (src/psx_card_packs.c,
 * render_title), found against window captures; each texel takes the ink of
 * the nearest tone (ink_of). Besides a mod's own cards, a retail card a
 * translation renames gets its plate from here (cards.c, translated_plate). */
#include "pc/compat/fs.h"
#include "cards.h"
#include "art.h"
#include "pc/text/glyphs.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#else
#ifdef __APPLE__
#include "pc/platform/macos_fonts.h"
#else
#include <fontconfig/fontconfig.h>
#endif
#endif

typedef struct { unsigned char r, g, b; } Rgb;

/* --- images ---------------------------------------------------------- */

/* The PNG as RGB over black or, with `ink`, as ink coverage in all three
 * channels: dark and opaque is full ink, so a strip drawn in black on white
 * and one drawn on a transparent background read the same. */
static Rgb *load_png_as(const char *path, int *width, int *height, int ink)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *out;
    size_t i, count;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return NULL; }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        return NULL;
    }
    fclose(file);
    count = (size_t)image.width * image.height;
    out = malloc(count * sizeof(*out));
    for (i = 0; out && i < count; i++) {
        unsigned a = rgba[i * 4 + 3];
        if (ink) {
            unsigned luma = (rgba[i * 4] * 3u + rgba[i * 4 + 1] * 6u + rgba[i * 4 + 2]) / 10u;
            out[i].r = out[i].g = out[i].b = (unsigned char)((255 - luma) * a / 255);
            continue;
        }
        out[i].r = (unsigned char)(rgba[i * 4] * a / 255);
        out[i].g = (unsigned char)(rgba[i * 4 + 1] * a / 255);
        out[i].b = (unsigned char)(rgba[i * 4 + 2] * a / 255);
    }
    *width = (int)image.width;
    *height = (int)image.height;
    free(rgba);
    png_image_free(&image);
    return out;
}

static Rgb *load_png(const char *path, int *width, int *height)
{
    return load_png_as(path, width, height, 0);
}

/* `w` x `h` from the middle of the image at that shape, each pixel the
 * average of the source pixels under it. */
static void resample(const Rgb *source, int sw, int sh, Rgb *out, int w, int h)
{
    double cw = sw, ch = sh, x0, y0;
    int x, y;
    if (cw * h > ch * w) cw = ch * w / h; else ch = cw * h / w;
    x0 = (sw - cw) / 2;
    y0 = (sh - ch) / 2;
    for (y = 0; y < h; y++) {
        int top = (int)(y0 + ch * y / h), bottom = (int)(y0 + ch * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)(x0 + cw * x / w), right = (int)(x0 + cw * (x + 1) / w), sx, sy;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    const Rgb *p = &source[(size_t)sy * sw + sx];
                    r += p->r; g += p->g; b += p->b; n++;
                }
            }
            if (!n) n = 1;
            out[y * w + x].r = (unsigned char)(r / n);
            out[y * w + x].g = (unsigned char)(g / n);
            out[y * w + x].b = (unsigned char)(b / n);
        }
    }
}

/* --- colors --------------------------------------------------------- */

/* A color and how many pixels have it. */
typedef struct {
    unsigned char r, g, b;
    int count;
} Weighted;

/* A box of median cut: its colors, how many pixels in all, and its range on
 * each channel. */
typedef struct {
    Weighted *colors;
    int n, count;
    int low[3], high[3];
} Box;

static unsigned short to555(int r, int g, int b)
{
    unsigned short c = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    return c ? c : 0x8000;   /* 0 is the transparent color; this is black */
}

static int channel(const Rgb *c, int axis) { return axis == 0 ? c->r : axis == 1 ? c->g : c->b; }
static int weighted_channel(const Weighted *c, int axis) { return axis == 0 ? c->r : axis == 1 ? c->g : c->b; }

static void measure(Box *box)
{
    int a, k;
    for (a = 0; a < 3; a++) {
        box->low[a] = 255;
        box->high[a] = 0;
    }
    for (k = 0; k < box->n; k++) {
        for (a = 0; a < 3; a++) {
            int v = weighted_channel(&box->colors[k], a);
            if (v < box->low[a]) box->low[a] = v;
            if (v > box->high[a]) box->high[a] = v;
        }
    }
}

/* The pixels in order by red, green, blue: three stable counting passes,
 * the least of the keys first, through `spare`; the result in `spare`. */
static void sort_pixels(Rgb *pixels, int count, Rgb *spare)
{
    int pass, k;
    Rgb *from = pixels, *to = spare, *swap;
    for (pass = 0; pass < 3; pass++) {
        int at[256] = {0}, key = 2 - pass, sum = 0, v;
        for (k = 0; k < count; k++) at[channel(&from[k], key)]++;
        for (v = 0; v < 256; v++) {
            int n = at[v];
            at[v] = sum;
            sum += n;
        }
        for (k = 0; k < count; k++) to[at[channel(&from[k], key)]++] = from[k];
        swap = from;
        from = to;
        to = swap;
    }
    /* Three passes: the sorted pixels are in `spare`. */
}

/* A box's colors in order by channel `axis`, then red, green and blue (no
 * color is there twice, so it is a whole order), through `spare`. */
static void sort_colors(Weighted *colors, int n, int axis, Weighted *spare)
{
    static const int keys[3] = {2, 1, 0};     /* blue, green, red, then the axis */
    int pass, k;
    Weighted *from = colors, *to = spare, *swap;
    for (pass = 0; pass < 4; pass++) {
        int at[256] = {0}, key = pass < 3 ? keys[pass] : axis, sum = 0, v;
        for (k = 0; k < n; k++) at[weighted_channel(&from[k], key)]++;
        for (v = 0; v < 256; v++) {
            int c = at[v];
            at[v] = sum;
            sum += c;
        }
        for (k = 0; k < n; k++) to[at[weighted_channel(&from[k], key)]++] = from[k];
        swap = from;
        from = to;
        to = swap;
    }
    /* Four passes: back in `colors`. */
}

/* The palette entry nearest a color (the weights the eye gives red, green
 * and blue), the lowest of those as near. `by_green` is the entries in
 * order of green and `hint` an entry likely near (the last color's): the
 * search starts from that one's distance and goes out from the entries of
 * about the color's green no further than green alone puts an entry past
 * the nearest found -- each entry as near as the hint or nearer is looked
 * at, so the answer is the plain search's. */
static int nearest(const Weighted *c, const Rgb *palette, const int *by_green, int entries, int hint)
{
    int lo = 0, hi = entries, best = hint, k, step;
    long dr = palette[hint].r - c->r, dg = palette[hint].g - c->g, db = palette[hint].b - c->b;
    long best_distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (palette[by_green[mid]].g < c->g) lo = mid + 1;
        else hi = mid;
    }
    for (step = 0; step < 2; step++) {
        for (k = step ? lo - 1 : lo; step ? k >= 0 : k < entries; k += step ? -1 : 1) {
            const Rgb *p = &palette[by_green[k]];
            long distance;
            dr = p->r - c->r;
            dg = p->g - c->g;
            db = p->b - c->b;
            if (dg * dg * 4 > best_distance) break;
            distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (distance < best_distance || (distance == best_distance && by_green[k] < best)) {
                best_distance = distance;
                best = by_green[k];
            }
        }
    }
    return best;
}

/* The memory one quantize works in. The cuts used to take a block of their
 * own each (a malloc per box, a realloc per split), a few hundred allocations
 * for every picture; where the game's allocator registers each one (the
 * translated build, whose registry is searched in a line) that was nearly all
 * of the time a card's picture took. Here the memory comes from a few chunks,
 * handed out in order and all freed at once. A chunk is the first one's size,
 * or a request's when that is bigger, so the memory stays near what the cuts
 * use (a bust's hundred pictures are millions of pixels): a picture of
 * all-different colors takes four or five chunks of the sixteen. */
#define ARENA_CHUNKS 16

typedef struct {
    unsigned char *chunks[ARENA_CHUNKS];
    int count;
    size_t used, size;      /* of the last chunk; before the first, the size it is to have */
} Arena;

static void *arena_get(Arena *arena, size_t bytes)
{
    bytes = (bytes + 15u) & ~(size_t)15u;
    if (!arena->count || bytes > arena->size - arena->used) {
        size_t size = arena->size;
        if (size < bytes) size = bytes;
        if (arena->count == ARENA_CHUNKS) return NULL;
        arena->chunks[arena->count] = malloc(size);
        if (!arena->chunks[arena->count]) return NULL;
        arena->count++;
        arena->used = 0;
        arena->size = size;
    }
    arena->used += bytes;
    return arena->chunks[arena->count - 1] + arena->used - bytes;
}

static void arena_free(Arena *arena)
{
    while (arena->count) free(arena->chunks[--arena->count]);
}

/* Median cut of the pixels to `colors` entries, written to `clut` from
 * entry 1 on, and each pixel's entry to `indices`: the widest box (on one
 * channel) split in two at its median pixel in order of that channel, then
 * red, green and blue, until there are `colors` or none is worth it; an
 * entry the average of its box, a pixel the entry nearest it.
 *
 * Worked on the colors and how many pixels have each, not pixel by pixel:
 * a box holds the same pixels either way (a split may part one color's
 * pixels between the two), so the palette and the entries are the plain
 * way's, in a moment where that took seconds for a bust's hundred pictures
 * (busts.c). */
static void quantize(const Rgb *pixels, int count, int colors, unsigned short *clut, unsigned char *indices)
{
    Arena arena = {{NULL}, 0, 0, 0};
    Rgb *sorted, *spare, palette[256];
    Weighted *unique = NULL, *scratch = NULL;
    unsigned char *entry = NULL, *table = NULL;
    Box boxes[256];
    int box_count = 1, i, k, n = 0, by_green[256];
    boxes[0].colors = NULL;
    if (count < 1) return;
    /* A chunk holds the two copies of the pixels and some of what follows. */
    arena.size = (size_t)count * 16;
    sorted = arena_get(&arena, (size_t)count * sizeof(*sorted));
    spare = arena_get(&arena, (size_t)count * sizeof(*spare));
    if (!sorted || !spare) goto done;
    /* The colors, in order, and how many of each. */
    memcpy(sorted, pixels, (size_t)count * sizeof(*sorted));
    sort_pixels(sorted, count, spare);
    for (k = 0; k < count; k++) {
        if (!k || spare[k].r != spare[k - 1].r || spare[k].g != spare[k - 1].g || spare[k].b != spare[k - 1].b) n++;
    }
    unique = arena_get(&arena, (size_t)n * sizeof(*unique));
    scratch = arena_get(&arena, (size_t)n * sizeof(*scratch));
    if (!unique || !scratch) goto done;
    for (k = 0, n = 0; k < count; k++) {
        const Rgb *c = &spare[k];
        if (n && unique[n - 1].r == c->r && unique[n - 1].g == c->g && unique[n - 1].b == c->b) {
            unique[n - 1].count++;
            continue;
        }
        unique[n].r = c->r;
        unique[n].g = c->g;
        unique[n].b = c->b;
        unique[n].count = 1;
        n++;
    }
    boxes[0].colors = arena_get(&arena, (size_t)n * sizeof(Weighted));
    if (!boxes[0].colors) goto done;
    memcpy(boxes[0].colors, unique, (size_t)n * sizeof(Weighted));
    boxes[0].n = n;
    boxes[0].count = count;
    measure(&boxes[0]);
    while (box_count < colors) {
        int best = -1, best_range = 0, axis = 0, a, half, before, j, part;
        Box *box, *next;
        for (i = 0; i < box_count; i++) {
            if (boxes[i].count < 2) continue;
            for (a = 0; a < 3; a++) {
                if (boxes[i].high[a] - boxes[i].low[a] > best_range) {
                    best_range = boxes[i].high[a] - boxes[i].low[a];
                    best = i;
                    axis = a;
                }
            }
        }
        if (best < 0 || best_range < 4) break;   /* no box worth splitting */
        box = &boxes[best];
        next = &boxes[box_count];
        sort_colors(box->colors, box->n, axis, scratch);
        /* Its first count / 2 pixels in that order stay; the rest are the
         * new box, a color across the line parted between the two. */
        half = box->count / 2;
        for (j = 0, before = 0; j < box->n && before + box->colors[j].count <= half; j++) before += box->colors[j].count;
        part = half - before;                 /* color j's pixels in the first */
        next->n = box->n - j;
        next->count = box->count - half;
        next->colors = arena_get(&arena, (size_t)next->n * sizeof(Weighted));
        if (!next->colors) break;
        memcpy(next->colors, box->colors + j, (size_t)next->n * sizeof(Weighted));
        if (part) {
            next->colors[0].count -= part;
            box->colors[j].count = part;
            box->n = j + 1;
        } else {
            box->n = j;
        }
        box->count = half;
        measure(box);
        measure(next);
        box_count++;
    }
    for (i = 0; i < box_count; i++) {
        unsigned long r = 0, g = 0, b = 0, w = (unsigned long)boxes[i].count;
        for (k = 0; k < boxes[i].n; k++) {
            const Weighted *c = &boxes[i].colors[k];
            r += (unsigned long)c->r * (unsigned long)c->count;
            g += (unsigned long)c->g * (unsigned long)c->count;
            b += (unsigned long)c->b * (unsigned long)c->count;
        }
        if (!w) w = 1;
        palette[i].r = (unsigned char)(r / w);
        palette[i].g = (unsigned char)(g / w);
        palette[i].b = (unsigned char)(b / w);
        clut[i + 1] = to555(palette[i].r, palette[i].g, palette[i].b);
    }
    for (; i < colors; i++) clut[i + 1] = 0x8000;
    clut[0] = 0x8000;
    /* Each color's entry once (in order, the last one's a good start for
     * the next), then each pixel's by its color: through a table of every
     * color when there are many, else found among them in order. */
    for (i = 0; i < box_count; i++) {
        int v = i;
        for (k = i; k > 0 && palette[by_green[k - 1]].g > palette[v].g; k--) by_green[k] = by_green[k - 1];
        by_green[k] = v;
    }
    entry = arena_get(&arena, (size_t)n);
    if (!entry) goto done;
    for (k = 0; k < n; k++)
        entry[k] = (unsigned char)(nearest(&unique[k], palette, by_green, box_count, k ? entry[k - 1] - 1 : 0) + 1);
    if (n > 0x4000) table = malloc((size_t)1 << 24);
    if (table) {
        for (k = 0; k < n; k++) table[(size_t)unique[k].r << 16 | (size_t)unique[k].g << 8 | unique[k].b] = entry[k];
        for (k = 0; k < count; k++)
            indices[k] = table[(size_t)pixels[k].r << 16 | (size_t)pixels[k].g << 8 | pixels[k].b];
    } else {
        for (k = 0; k < count; k++) {
            unsigned key = (unsigned)pixels[k].r << 16 | (unsigned)pixels[k].g << 8 | pixels[k].b;
            int lo = 0, hi = n - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                unsigned at = (unsigned)unique[mid].r << 16 | (unsigned)unique[mid].g << 8 | unique[mid].b;
                if (at < key) lo = mid + 1;
                else hi = mid;
            }
            indices[k] = entry[lo];
        }
    }
done:
    arena_free(&arena);
    free(table);
}

static void put_clut(unsigned char *out, const unsigned short *clut, int entries)
{
    int i;
    for (i = 0; i < entries; i++) {
        out[i * 2] = (unsigned char)(clut[i] & 0xFF);
        out[i * 2 + 1] = (unsigned char)(clut[i] >> 8);
    }
}

static int image_into(const char *path, unsigned char *record, int thumbnail_only, char *why, size_t why_size)
{
    int width, height;
    Rgb *source = load_png(path, &width, &height), *art;
    unsigned short clut[256];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT * sizeof(*art));
    if (!art) { free(source); return 0; }
    if (!thumbnail_only) {
        resample(source, width, height, art, CARD_ART_WIDTH, CARD_ART_HEIGHT);
        quantize(art, CARD_ART_WIDTH * CARD_ART_HEIGHT, 255, clut, record + CARD_ART_PIXELS);
        put_clut(record + CARD_ART_CLUT, clut, 256);
    }
    resample(source, width, height, art, CARD_THUMB_WIDTH, CARD_THUMB_HEIGHT);
    quantize(art, CARD_THUMB_WIDTH * CARD_THUMB_HEIGHT, 63, clut, record + CARD_THUMB_PIXELS);
    put_clut(record + CARD_THUMB_CLUT, clut, 64);
    free(art);
    free(source);
    return 1;
}

int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 0, why, why_size);
}

int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 1, why, why_size);
}

/* Like load_png, but keeps the PNG's own alpha instead of flattening it
 * away: field_art's cutouts (below) need to know what to leave out; every
 * other user of a card's art draws a full rectangle, so nothing else does.
 * *alpha is the same length as the returned pixels, NULL on failure along
 * with the return. */
static Rgb *load_png_rgba(const char *path, int *width, int *height, unsigned char **alpha)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *out;
    size_t i, count;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return NULL; }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        return NULL;
    }
    fclose(file);
    count = (size_t)image.width * image.height;
    out = malloc(count * sizeof(*out));
    *alpha = malloc(count);
    if (!out || !*alpha) {
        free(out);
        free(*alpha);
        *alpha = NULL;
        free(rgba);
        png_image_free(&image);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        out[i].r = rgba[i * 4];
        out[i].g = rgba[i * 4 + 1];
        out[i].b = rgba[i * 4 + 2];
        (*alpha)[i] = rgba[i * 4 + 3];
    }
    *width = (int)image.width;
    *height = (int)image.height;
    free(rgba);
    png_image_free(&image);
    return out;
}

/* resample's own crop-and-average, for a single channel: called with the
 * same source and target sizes resample itself is, so the same crop window
 * (a deterministic function of those sizes alone) lines back up with it,
 * texel for texel. */
static void resample_alpha(const unsigned char *source, int sw, int sh, unsigned char *out, int w, int h)
{
    double cw = sw, ch = sh, x0, y0;
    int x, y;
    if (cw * h > ch * w) cw = ch * w / h; else ch = cw * h / w;
    x0 = (sw - cw) / 2;
    y0 = (sh - ch) / 2;
    for (y = 0; y < h; y++) {
        int top = (int)(y0 + ch * y / h), bottom = (int)(y0 + ch * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)(x0 + cw * x / w), right = (int)(x0 + cw * (x + 1) / w), sx, sy;
            unsigned long a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    a += source[(size_t)sy * sw + sx];
                    n++;
                }
            }
            if (!n) n = 1;
            out[y * w + x] = (unsigned char)(a / n);
        }
    }
}

#define FIELD_ART_ALPHA_CUTOFF 128

int CardArt_FieldArtFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    int width, height, i;
    unsigned char *alpha;
    Rgb *source = load_png_rgba(path, &width, &height, &alpha), *art;
    unsigned char *art_alpha;
    unsigned short clut[256];

    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT * sizeof(*art));
    art_alpha = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT);
    if (!art || !art_alpha) {
        free(source);
        free(alpha);
        free(art);
        free(art_alpha);
        return 0;
    }
    resample(source, width, height, art, CARD_ART_WIDTH, CARD_ART_HEIGHT);
    resample_alpha(alpha, width, height, art_alpha, CARD_ART_WIDTH, CARD_ART_HEIGHT);
    quantize(art, CARD_ART_WIDTH * CARD_ART_HEIGHT, 255, clut, record + CARD_ART_PIXELS);
    /* A texel this transparent is written as index 0 (never quantize's own
     * output: every real pixel is 1-255), and its CLUT entry set to the
     * PS1's own transparent color, 0x0000 (to555's own comment) -- unlike
     * quantize's own default for it, 0x8000, opaque black, since index 0
     * never reaches a real pixel anywhere else a card's art is drawn. */
    clut[0] = 0;
    for (i = 0; i < CARD_ART_WIDTH * CARD_ART_HEIGHT; i++) {
        if (art_alpha[i] < FIELD_ART_ALPHA_CUTOFF) record[CARD_ART_PIXELS + i] = 0;
    }
    put_clut(record + CARD_ART_CLUT, clut, 256);

    free(source);
    free(alpha);
    free(art);
    free(art_alpha);
    return 1;
}

/* A Free Duel portrait record: the 48x48 image at 8 bits a pixel, then its
 * 64-entry palette, which is what the screen uploads and the disc holds forty
 * of (notes/more-duelists.md). The same shape as a card's, at another size. */
int CardArt_PortraitFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    int width, height;
    Rgb *source = load_png(path, &width, &height), *art;
    unsigned short clut[256];

    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc((size_t)PORTRAIT_SIDE * PORTRAIT_SIDE * sizeof(*art));
    if (!art) { free(source); return 0; }
    resample(source, width, height, art, PORTRAIT_SIDE, PORTRAIT_SIDE);
    quantize(art, PORTRAIT_SIDE * PORTRAIT_SIDE, 63, clut, record);
    put_clut(record + PORTRAIT_PIXELS, clut, 64);
    free(art);
    free(source);
    return 1;
}

/* A guardian star's icon (stars.h): 16x16 at 4 bits a pixel, index 0 where
 * the PNG is less than half covered. With `palette` (16 colors, 0 the
 * transparent one) each pixel takes the nearest of its entries 1-15, as the
 * disc's stars are drawn; without, the PNG's own 15 colors go to `clut`.
 * The image is taken whole, squeezed to a square if it is not one. */
int CardArt_IconFromImage(const char *path, const unsigned short *palette, unsigned char *pixels,
                          unsigned short *clut, char *why, size_t why_size)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb cells[CARD_ICON_SIDE * CARD_ICON_SIDE], opaque[CARD_ICON_SIDE * CARD_ICON_SIDE];
    unsigned char covered[CARD_ICON_SIDE * CARD_ICON_SIDE], indices[CARD_ICON_SIDE * CARD_ICON_SIDE];
    int x, y, count = 0, i;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!(file = fopen(path, "rb")) || !png_image_begin_read_from_stdio(&image, file)) {
        if (file) fclose(file);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    fclose(file);
    /* Each cell the average of the pixels under it, weighted by their
     * alpha, and how much of it they cover. */
    for (y = 0; y < CARD_ICON_SIDE; y++) {
        int top = (int)((long)image.height * y / CARD_ICON_SIDE);
        int bottom = (int)((long)image.height * (y + 1) / CARD_ICON_SIDE);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < CARD_ICON_SIDE; x++) {
            int left = (int)((long)image.width * x / CARD_ICON_SIDE);
            int right = (int)((long)image.width * (x + 1) / CARD_ICON_SIDE), sx, sy;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < (int)image.height; sy++) {
                for (sx = left; sx < right && sx < (int)image.width; sx++) {
                    const unsigned char *p = &rgba[((size_t)sy * image.width + sx) * 4];
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            }
            i = y * CARD_ICON_SIDE + x;
            covered[i] = n && a * 2 >= n * 255;
            if (a) {
                cells[i].r = (unsigned char)(r / a);
                cells[i].g = (unsigned char)(g / a);
                cells[i].b = (unsigned char)(b / a);
            } else {
                cells[i].r = cells[i].g = cells[i].b = 0;
            }
            if (covered[i]) opaque[count++] = cells[i];
        }
    }
    free(rgba);
    png_image_free(&image);
    memset(indices, 0, sizeof(indices));
    if (palette) {
        for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE; i++) {
            long best_distance = -1;
            int entry;
            if (!covered[i]) continue;
            for (entry = 1; entry < 16; entry++) {
                unsigned short c = palette[entry];
                long dr, dg, db, distance;
                if (!(c & 0x7FFF) && !(c & 0x8000)) continue;   /* transparent */
                dr = cells[i].r - (long)((c & 0x1F) << 3);
                dg = cells[i].g - (long)(((c >> 5) & 0x1F) << 3);
                db = cells[i].b - (long)(((c >> 10) & 0x1F) << 3);
                distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (best_distance < 0 || distance < best_distance) { best_distance = distance; indices[i] = (unsigned char)entry; }
            }
        }
        if (clut) memcpy(clut, palette, 16 * sizeof(*clut));
    } else {
        unsigned short own[256];
        unsigned char chosen[CARD_ICON_SIDE * CARD_ICON_SIDE];
        int k = 0;
        memset(own, 0, sizeof(own));
        if (count) quantize(opaque, count, 15, own, chosen);
        for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE; i++) {
            if (covered[i]) indices[i] = chosen[k++];
        }
        own[0] = 0;   /* index 0 stays transparent */
        if (clut) memcpy(clut, own, 16 * sizeof(*clut));
    }
    for (i = 0; i < CARD_ICON_SIDE * CARD_ICON_SIDE / 2; i++) {
        pixels[i] = (unsigned char)(indices[i * 2] | (indices[i * 2 + 1] << 4));
    }
    return 1;
}

/* Where resample takes a `w` by `h` picture from: the middle of the image
 * at that shape, in whole pixels, and the image's size. Only the PNG's
 * header is read. */
int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw, int *ch, int *width, int *height)
{
    png_image image;
    FILE *file;
    double sw, sh, fw, fh;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return 0; }
    sw = fw = image.width;
    sh = fh = image.height;
    png_image_free(&image);
    fclose(file);
    if (fw * h > fh * w) fw = fh * w / h; else fh = fw * h / w;
    *cw = (int)(fw + 0.5);
    *ch = (int)(fh + 0.5);
    if (*cw < 1) *cw = 1;
    if (*ch < 1) *ch = 1;
    *x = (int)((sw - *cw) / 2 + 0.5);
    *y = (int)((sh - *ch) / 2 + 0.5);
    *width = (int)sw;
    *height = (int)sh;
    return 1;
}

/* A PNG stretched to `w` x `h`, each texel the average of the pixels under
 * it: a texel under half covered is clear (0 in `indices`), the others 1,
 * their colors appended to `opaque` from `*count` on. */
static int sample_indexed(const char *path, int w, int h, unsigned char *indices, Rgb *opaque, int *count,
                          char *why, size_t why_size)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    int x, y, sw, sh;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file || !png_image_begin_read_from_stdio(&image, file)) {
        if (file) fclose(file);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    fclose(file);
    sw = (int)image.width;
    sh = (int)image.height;
    png_image_free(&image);
    for (y = 0; y < h; y++) {
        int top = (int)((long)sh * y / h), bottom = (int)((long)sh * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)((long)sw * x / w), right = (int)((long)sw * (x + 1) / w), sx, sy;
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    const unsigned char *p = rgba + ((size_t)sy * sw + sx) * 4;
                    r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
                }
            }
            if (!n || a * 2 < n * 255) {
                indices[y * w + x] = 0;
                continue;
            }
            opaque[*count].r = (unsigned char)(r / a);
            opaque[*count].g = (unsigned char)(g / a);
            opaque[*count].b = (unsigned char)(b / a);
            indices[y * w + x] = 1;   /* an opaque texel, numbered below */
            (*count)++;
        }
    }
    free(rgba);
    return 1;
}

/* A PNG with see-through parts as 8-bit texels (title_images.c): stretched
 * to `w` x `h`, each texel the average of the pixels under it, a texel under
 * half covered clear (entry 0, the PS1's transparent 0x0000) and the rest
 * reduced to 255 colors by median cut from entry 1. */
int CardArt_IndexedImage(const char *path, int w, int h, unsigned char *indices, unsigned short *clut, char *why,
                         size_t why_size)
{
    return CardArt_IndexedImages(&path, 1, w, h, &indices, clut, why, why_size);
}

int CardArt_IndexedImages(const char *const *paths, int count, int w, int h, unsigned char *const *indices,
                          unsigned short *clut, char *why, size_t why_size)
{
    Rgb *opaque;
    unsigned char *opaque_indices;
    int x, i, opaque_count = 0, k;
    opaque = malloc((size_t)w * h * (size_t)count * sizeof(*opaque));
    opaque_indices = malloc((size_t)w * h * (size_t)count);
    if (!opaque || !opaque_indices) {
        free(opaque);
        free(opaque_indices);
        snprintf(why, why_size, "out of memory for %s", count ? paths[0] : "a picture");
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!sample_indexed(paths[i], w, h, indices[i], opaque, &opaque_count, why, why_size)) {
            free(opaque);
            free(opaque_indices);
            return 0;
        }
    }
    memset(clut, 0, 256 * sizeof(*clut));
    if (opaque_count) quantize(opaque, opaque_count, 255, clut, opaque_indices);
    clut[0] = 0x0000;
    for (k = 0, i = 0; i < count; i++) {
        for (x = 0; x < w * h; x++) {
            if (indices[i][x]) indices[i][x] = opaque_indices[k++];
        }
    }
    free(opaque);
    free(opaque_indices);
    return 1;
}

/* The PNG's width and height, from its header. */
int CardArt_ImageSize(const char *path, int *width, int *height)
{
    int x, y, cw, ch;
    return CardArt_Crop(path, 1, 1, &x, &y, &cw, &ch, width, height);
}

/* --- the title plate --------------------------------------------------- */

static void put_ink(unsigned char *plate, int x, int y, int ink)
{
    unsigned char *byte;
    if (x < 0 || x >= CARD_TITLE_WIDTH || y < 0 || y >= CARD_TITLE_HEIGHT) return;
    byte = plate + y * (CARD_TITLE_WIDTH / 2) + x / 2;
    if (x & 1) *byte = (unsigned char)((*byte & 0x0F) | (ink << 4));
    else *byte = (unsigned char)((*byte & 0xF0) | ink);
}

/* Coverage (0-255) to the plate ink of the nearest tone. What each ink
 * takes from the gold under it, as a share of what 1 takes, measured on a
 * retail plate drawn in the game (Dancing Elf's, at 1x): 1 all of it, then
 * about .93, .8, .6, .5, .35 and .18 for 7; 0 none. The thresholds are the
 * midpoints between those, so an edge is drawn in the ink that darkens it as
 * much as its coverage says, stems at 1 and fringes at 6 and 7 as the retail
 * plates have them. */
static int ink_of(int coverage)
{
    static const unsigned char from[7] = {247, 221, 179, 140, 109, 69, 23};
    int ink;
    for (ink = 0; ink < 7; ink++) {
        if (coverage >= from[ink]) return ink + 1;
    }
    return 0;
}

int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why, size_t why_size)
{
    int width, height, x, y;
    Rgb *source = load_png_as(path, &width, &height, 1), small[CARD_TITLE_WIDTH * CARD_TITLE_HEIGHT];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    resample(source, width, height, small, CARD_TITLE_WIDTH, CARD_TITLE_HEIGHT);
    memset(plate, 0, CARD_TITLE_BYTES);
    for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
        for (x = 0; x < CARD_TITLE_WIDTH; x++) {
            put_ink(plate, x, y, ink_of(small[y * CARD_TITLE_WIDTH + x].r));
        }
    }
    free(source);
    return 1;
}

static FT_Library library;
static FT_Face face;
static int face_tried;

static const char *serif_file(void)
{
#ifdef _WIN32
    return Win32_SerifFontPath();
#elif defined(__APPLE__)
    return MacOS_FontPath(MEMORIES_FONT_SERIF);
#else
    static char path[1024];
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
    if (!FcInit()) return NULL;
    pattern = FcNameParse((const FcChar8 *)"Times:regular");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    path[0] = '\0';
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        snprintf(path, sizeof(path), "%s", (const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return path[0] ? path : NULL;
#endif
}

void *CardArt_SerifFace(void)
{
    if (!face_tried) {
        const char *file = serif_file();
        face_tried = 1;
        if (!file || FT_Init_FreeType(&library) || FT_New_Face(library, file, 0, &face)) face = NULL;
    }
    return face;
}

/* The name as the retail plates set theirs: Times at 13 pixels, the
 * baseline under row 11, from column 3, each glyph on a whole pixel so stems
 * fill whole columns; a name wider than 90 pixels is squeezed into columns
 * 3 to 93, as the long retail names are. At `factor` pixels per texel all of
 * that is `factor` times as big. `cover` gets each pixel's coverage, 0-255,
 * CARD_TITLE_WIDTH * factor across and CARD_TITLE_HEIGHT * factor down.
 * 0 when there is no serif font. */
static int set_name(const char *name, int factor, unsigned char *cover)
{
    const int wide = 512 * factor, baseline = 11 * factor, left = 3 * factor, room = 90 * factor;
    const int width = CARD_TITLE_WIDTH * factor, height = CARD_TITLE_HEIGHT * factor, above = 8 * factor;
    unsigned char *line, *drawn;
    int pen = left, x, y, ink_low = wide, ink_high = -1, previous = 0, first = above;
    const char *c = name;
    CardArt_SerifFace();
    if (!face || FT_Set_Pixel_Sizes(face, 0, 13 * factor)) return 0;
    /* Drawn with `above` rows over the plate, for marks that reach past
     * its top (a Vietnamese capital's two). */
    drawn = calloc((size_t)(above + height) * wide, 1);
    if (!drawn) return 0;
    line = drawn + (size_t)above * wide;
    while (*c) {
        /* A character at a time, as the name's glyphs are (cards.c,
         * encode_name): an accented letter is one glyph, not two. */
        FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)Glyphs_NextCharacter(&c));
        FT_Bitmap *bitmap;
        if (!index) continue; /* a letter the face lacks is left out, not drawn as a box */
        if (previous && index && FT_HAS_KERNING(face)) {
            FT_Vector kern;
            if (!FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kern)) pen += (int)((kern.x + 32) >> 6);
        }
        previous = index;
        if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) continue;
        bitmap = &face->glyph->bitmap;
        for (y = 0; y < (int)bitmap->rows; y++) {
            int ty = baseline - face->glyph->bitmap_top + y;
            if (ty < -above || ty >= height) continue;
            for (x = 0; x < (int)bitmap->width; x++) {
                int tx = pen + face->glyph->bitmap_left + x;
                unsigned char v = bitmap->buffer[y * bitmap->pitch + x];
                if (tx < 0 || tx >= wide || !v) continue;
                if (v > line[ty * wide + tx]) line[ty * wide + tx] = v;
                if (ty < first) first = ty;
                if (tx < ink_low) ink_low = tx;
                if (tx > ink_high) ink_high = tx;
            }
        }
        pen += (int)((face->glyph->advance.x + 32) >> 6);
        if (pen >= wide) break;
    }
    if (first < 0) {
        /* Ink above the plate: what is above the baseline is fitted into
         * the rows above it (each row the most of the ones it takes), so
         * the marks stay on; the baseline and what is below stay. A name
         * that fits (any ASCII one) is left as it is. */
        unsigned char *fitted = calloc((size_t)baseline * wide, 1);
        if (!fitted) {
            free(drawn);
            return 0;
        }
        for (y = 0; y < baseline; y++) {
            int from = first + y * (baseline - first) / baseline, to = first + (y + 1) * (baseline - first) / baseline, k;
            unsigned char *row = fitted + (size_t)y * wide;
            if (to <= from) to = from + 1;
            for (k = from; k < to; k++) {
                for (x = 0; x < wide; x++) {
                    if (line[(ptrdiff_t)k * wide + x] > row[x]) row[x] = line[(ptrdiff_t)k * wide + x];
                }
            }
        }
        memcpy(line, fitted, (size_t)baseline * wide);
        free(fitted);
    }
    memset(cover, 0, (size_t)width * height);
    if (ink_high < ink_low) {
    } else if (ink_high - ink_low + 1 <= room) {
        for (y = 0; y < height; y++) memcpy(cover + (size_t)y * width, line + (size_t)y * wide, (size_t)width);
    } else {
        /* Squeezed with a linear filter, then brought back up to full ink:
         * averaging thins every stem, and thin stems are faint ones. */
        float *squeezed = malloc(sizeof(float) * (size_t)height * room);
        float step = (float)(ink_high - ink_low + 1) / room, peak = 1.0f;
        if (!squeezed) {
            free(drawn);
            return 0;
        }
        for (y = 0; y < height; y++) {
            for (x = 0; x < room; x++) {
                float at = ink_low + (x + 0.5f) * step - 0.5f, t;
                int i0, i1;
                if (at < 0) at = 0;
                i0 = (int)at;
                t = at - i0;
                i1 = i0 + 1 < wide ? i0 + 1 : i0;
                squeezed[y * room + x] = line[y * wide + i0] * (1.0f - t) + line[y * wide + i1] * t;
                if (squeezed[y * room + x] > peak) peak = squeezed[y * room + x];
            }
        }
        for (y = 0; y < height; y++) {
            for (x = 0; x < room; x++) {
                cover[(size_t)y * width + left + x] = (unsigned char)(int)(squeezed[y * room + x] * 255.0f / peak + 0.5f);
            }
        }
        free(squeezed);
    }
    free(drawn);
    return 1;
}

int CardArt_TitleFromName(const char *name, unsigned char *plate)
{
    unsigned char cover[CARD_TITLE_WIDTH * CARD_TITLE_HEIGHT];
    int x, y;
    if (!set_name(name, 1, cover)) return 0;
    memset(plate, 0, CARD_TITLE_BYTES);
    for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
        for (x = 0; x < CARD_TITLE_WIDTH; x++) put_ink(plate, x, y, ink_of(cover[y * CARD_TITLE_WIDTH + x]));
    }
    return 1;
}

int CardArt_TitlePicture(const char *name, int factor, unsigned char *indices, int pitch)
{
    const int width = CARD_TITLE_WIDTH * factor, height = CARD_TITLE_HEIGHT * factor;
    unsigned char *cover = malloc((size_t)width * height);
    int x, y;
    if (!cover) return 0;
    if (!set_name(name, factor, cover)) {
        free(cover);
        return 0;
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            /* The plate's inks run from 1 (full, the darkest) to 7 (the
             * faintest the retail plates use); at this size the whole run. */
            int c = cover[(size_t)y * width + x], ink = 0;
            if (c >= 24) ink = 7 - (int)((c >= 200 ? 176 : c - 24) * 6 / 176.0 + 0.5);
            indices[(size_t)y * pitch + x] = (unsigned char)ink;
        }
    }
    free(cover);
    return 1;
}
