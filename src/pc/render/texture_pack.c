#include "pc/compat/fs.h"
#include "texture_pack.h"
#include "texture_dump.h"
#include "soft_gpu.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/compat/signal.h"
#include "pc/sdk/disc.h"
#include <png.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef MEMORIES_TRANSLATED
#include "pc/guest/translated_runtime.h"
#endif

typedef struct Entry {
    uint32_t offset, clut_offset, stride; /* disc bytes once resolved; stride in words, 0 with row_offsets */
    char archive[32];                     /* the archive the offsets are relative to until then */
    int32_t *row_offsets;
    int words, rows, bpp, crop_left, crop_width, clut_entries;
    char *file;
    volatile int wanted; /* an upload needs this image: TexturePack_Service reads it */
    uint16_t *pixels; /* resampled to words*per_word x rows, shadow cells (texture_dump.h); NULL until first use */
    unsigned char *image; /* the PNG itself, RGBA, for the scaled picture */
    int image_width, image_height;
    int failed;
    int absolute; /* offset and clut_offset are the disc's (resolve) */
    unsigned rank; /* the pack's place in the mods' load order: a later pack's reading of the same words wins */
    int position;  /* and the entry's in its manifest, so the order never rests on qsort's */
    int made;      /* the port made the words (TexturePack_AddMade): the image is this rectangle of the PNG */
    int see_through; /* a made image that keeps the PNG's alpha (TexturePack_AddMadeSeeThrough) */
    int source_x, source_y, source_w, source_h;
} Entry;

/* Named replacements use the extractor's layouts, compiled into the game. */
typedef struct AssetSpec {
    const char *name, *archive;
    unsigned offset;
    int words, rows, bpp;
    unsigned clut_offset;
    int clut_entries, stride, crop_left, width;
} AssetSpec;
#include "asset_catalog.inc"
#define ASSET_COUNT ((int)(sizeof(asset_specs) / sizeof(asset_specs[0])))

static Entry *entries;
static int entry_count, resolved; /* offsets are absolute on the disc, entries sorted */
/* An upload can arrive from the interrupt tick (a LoadImage in the disc
 * callback), where reading a PNG or the disc's directory is not safe: paint
 * only notes what it needs, and TexturePack_Service does it between frames. */
static volatile int wanted_resolve, wanted_images, wanted_rediscover;
static uint16_t *entry_of; /* per VRAM word: entry index + 1 painted there, 0 none */
static uint32_t *place_of; /* per VRAM word: row << 16 | word within that entry */
/* The uploads that asked for an image not read yet: once it is, only they
 * are painted again. Painting all of VRAM takes longer than a frame, and
 * the frame it lands in is shown late (a card's art is read as the card is
 * turned over). Past the last slot, all of VRAM. */
#define WAITING_AREAS 32
typedef struct Area {
    int x, y, w, h;
} Area;
static Area waiting[WAITING_AREAS];
static volatile int waiting_count;
static unsigned generation, map_generation; /* of the entries, of the maps (texture_pack.h) */
/* prepare's pick for the primitive: the head entry (index) whose words it
 * samples and the sibling read with its depth and palette. */
static int chosen_head = -1, chosen = -1;

static int per_word(int bpp) { return bpp == 4 ? 4 : bpp == 8 ? 2 : 1; }

/* Images the port makes (TexturePack_AddMade): the entries, kept apart so
 * that every reload of the packs gets them back, and the blocks of bytes
 * their uploads are known by, each at its own place from TEXTURE_MADE_BASE
 * up. Identical blocks are one block. */
typedef struct Block {
    uint32_t offset, bytes;
    unsigned char *data;
} Block;
static Entry *made;
static int made_count;
static Block *blocks;
static int block_count;
static uint32_t made_next = TEXTURE_MADE_BASE;

typedef struct BankSprite {
    int bank, page_x, page_y, depth, u, v, w, h;
    int source_x, source_y, source_w, source_h;
    char *file;
    unsigned char *image;
    int image_width, image_height, failed;
} BankSprite;
static BankSprite *bank_sprites;
static int bank_sprite_count;

static int load_bank_sprite(BankSprite *sprite)
{
    png_image image = {0};
    FILE *file;
    if (sprite->image || sprite->failed) return sprite->image != NULL;
    image.version = PNG_IMAGE_VERSION;
    file = fopen(sprite->file, "rb");
    if (!file || !png_image_begin_read_from_stdio(&image, file)) {
        fprintf(stderr, "memories-pc: texture bank image: %s cannot be read: %s\n", sprite->file, image.message);
        if (file) fclose(file);
        sprite->failed = 1;
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    sprite->image = malloc(PNG_IMAGE_SIZE(image));
    if (!sprite->image || !png_image_finish_read(&image, NULL, sprite->image, 0, NULL)) {
        fprintf(stderr, "memories-pc: texture bank image: %s cannot be read: %s\n", sprite->file,
                sprite->image ? image.message : "out of memory");
        free(sprite->image); sprite->image = NULL;
        fclose(file); png_image_free(&image); sprite->failed = 1;
        return 0;
    }
    fclose(file);
    sprite->image_width = (int)image.width;
    sprite->image_height = (int)image.height;
    png_image_free(&image);
    return 1;
}

int TexturePack_AddBankSpriteCrop(int bank, int page_x, int page_y, int depth, int u, int v, int w, int h,
                                  const char *file, int source_x, int source_y, int source_w, int source_h)
{
    BankSprite *more;
    int i;
    if (!file || bank <= 0 || w <= 0 || h <= 0 || u < 0 || v < 0 || u + w > 256 || v + h > 256 ||
        source_x < 0 || source_y < 0 || (source_w == 0) != (source_h == 0) || source_w < 0 || source_h < 0) return 0;
    for (i = 0; i < bank_sprite_count; i++) {
        BankSprite *at = &bank_sprites[i];
        if (at->bank == bank && at->page_x == page_x && at->page_y == page_y && at->depth == depth &&
            at->u == u && at->v == v && at->w == w && at->h == h) {
            if (!strcmp(at->file, file) && at->source_x == source_x && at->source_y == source_y &&
                at->source_w == source_w && at->source_h == source_h) return 1;
            free(at->file); free(at->image); memset(at, 0, sizeof(*at));
            at->bank = bank; at->page_x = page_x; at->page_y = page_y; at->depth = depth;
            at->u = u; at->v = v; at->w = w; at->h = h; at->file = strdup(file);
            at->source_x = source_x; at->source_y = source_y; at->source_w = source_w; at->source_h = source_h;
            generation++;
            return at->file != NULL;
        }
    }
    more = realloc(bank_sprites, (size_t)(bank_sprite_count + 1) * sizeof(*more));
    if (!more) return 0;
    bank_sprites = more;
    memset(&bank_sprites[bank_sprite_count], 0, sizeof(*bank_sprites));
    bank_sprites[bank_sprite_count] = (BankSprite){bank, page_x, page_y, depth, u, v, w, h,
                                                     source_x, source_y, source_w, source_h,
                                                     strdup(file), NULL, 0, 0, 0};
    if (!bank_sprites[bank_sprite_count].file) return 0;
    bank_sprite_count++;
    generation++;
    return 1;
}

int TexturePack_AddBankSprite(int bank, int page_x, int page_y, int depth, int u, int v, int w, int h,
                              const char *file)
{
    return TexturePack_AddBankSpriteCrop(bank, page_x, page_y, depth, u, v, w, h, file, 0, 0, 0, 0);
}

int TexturePack_BankEntryFor(int bank, int page_x, int page_y, int depth, int u, int v)
{
    int i;
    for (i = 0; i < bank_sprite_count; i++) {
        BankSprite *at = &bank_sprites[i];
        if (at->bank == bank && at->page_x == page_x && at->page_y == page_y && at->depth == depth &&
            u >= at->u && u < at->u + at->w && v >= at->v && v < at->v + at->h && load_bank_sprite(at)) return -i - 1;
    }
    return 0;
}

int TexturePack_BankSample(int bank, int page_x, int page_y, int depth, int u, int v, uint32_t *rgb)
{
    int entry = TexturePack_BankEntryFor(bank, page_x, page_y, depth, u >> 16, v >> 16), x, y;
    BankSprite *at;
    const unsigned char *p;
    if (!entry) return 0;
    at = &bank_sprites[-entry - 1];
    x = at->source_x + (int)(((int64_t)(u - (at->u << 16)) * (at->source_w ? at->source_w : at->image_width)) /
                       ((int64_t)at->w << 16));
    y = at->source_y + (int)(((int64_t)(v - (at->v << 16)) * (at->source_h ? at->source_h : at->image_height)) /
                       ((int64_t)at->h << 16));
    x = x < 0 ? 0 : x >= at->image_width ? at->image_width - 1 : x;
    y = y < 0 ? 0 : y >= at->image_height ? at->image_height - 1 : y;
    p = at->image + ((size_t)y * at->image_width + x) * 4;
    if (p[3] < PACK_ALPHA_CLEAR) return 2;
    *rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2] | ((uint32_t)(255 - p[3]) >> 1 << 24);
    return 1;
}

int TexturePack_BankEntryRect(int entry, int *u, int *v, int *w, int *h)
{
    BankSprite *at;
    if (entry >= 0 || -entry > bank_sprite_count) return 0;
    at = &bank_sprites[-entry - 1];
    *u = at->u; *v = at->v; *w = at->w; *h = at->h;
    return 1;
}

int TexturePack_BankEntrySource(int entry, int *x, int *y, int *w, int *h)
{
    BankSprite *at;
    if (entry >= 0 || -entry > bank_sprite_count || !(at = &bank_sprites[-entry - 1])->image) return 0;
    *x = at->source_x; *y = at->source_y;
    *w = at->source_w ? at->source_w : at->image_width;
    *h = at->source_h ? at->source_h : at->image_height;
    return 1;
}

/* Readings of the same words: entries of one geometry, differing in depth
 * or palette (a sheet the game draws with several palettes). Sorted by
 * offset they are adjacent; the first is the head, the one the maps name. */
static int sibling(const Entry *a, const Entry *b)
{
    return a->offset == b->offset && a->words == b->words && a->rows == b->rows && a->stride == b->stride &&
           !a->row_offsets && !b->row_offsets;
}

static int head_of(int index)
{
    while (index > 0 && sibling(&entries[index - 1], &entries[index])) index--;
    return index;
}

/* The disc byte offset of an archive named as the extractor names it
 * ("WA_MRG.MRG"): -1 if the disc has no such file, -2 while there is no
 * disc to ask yet (mods are applied before it is opened). */
static long archive_start(const char *name)
{
    static struct { char name[32]; long start; } known[8];
    static int count;
    char path[64];
    int i, lba, found;
    unsigned size;
    for (i = 0; i < count; i++) {
        if (strcmp(known[i].name, name) == 0) return known[i].start;
    }
    snprintf(path, sizeof(path), "\\DATA\\%s;1", name);
    found = TextureDump_DiscFile(path, &lba, &size);
    if (found == -2) return -2;
    if (found != 0 || lba < 0) return -1;
    if (count < 8 && strlen(name) < sizeof(known[0].name)) {
        strcpy(known[count].name, name);
        known[count].start = (long)lba * 2048;
        count++;
    }
    return (long)lba * 2048;
}

/* By offset, then geometry, depth and palette: readings of the same words
 * end up adjacent, whatever packs they came from and in whatever order.
 * The same reading from two packs goes the later pack first, since the first
 * of a run is the one drawn (head_of, prepare); last, the manifest's own
 * order. No two entries compare equal, so glibc's qsort and the Windows
 * CRT's put them in the same order. */
static int compare(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
    if (x->words != y->words) return x->words < y->words ? -1 : 1;
    if (x->rows != y->rows) return x->rows < y->rows ? -1 : 1;
    if (x->stride != y->stride) return x->stride < y->stride ? -1 : 1;
    if (x->bpp != y->bpp) return x->bpp < y->bpp ? -1 : 1;
    if (x->clut_offset != y->clut_offset) return x->clut_offset < y->clut_offset ? -1 : 1;
    if (x->rank != y->rank) return x->rank > y->rank ? -1 : 1;
    return x->position < y->position ? -1 : x->position > y->position;
}

/* The PNG, as the texture's own grid of 15-bit colors: each texel takes
 * the average of the image pixels that fall on it (a pack image is any
 * size), alpha below half is the transparent color. */
static int load_pixels(Entry *entry)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    char path[1200];
    int width = entry->words * per_word(entry->bpp), height = entry->rows, x, y;
    unsigned image_width, image_height;
    if (entry->pixels || entry->failed) return entry->pixels != NULL;
    entry->wanted = 0;
    snprintf(path, sizeof(path), "%s", entry->file);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file || !png_image_begin_read_from_stdio(&image, file)) {
        fprintf(stderr, "memories-pc: texture pack: %s cannot be read: %s\n", path, image.message);
        if (file) fclose(file);
        entry->failed = 1;
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        fprintf(stderr, "memories-pc: texture pack: %s cannot be read: %s\n", path, rgba ? image.message : "out of memory");
        free(rgba);
        fclose(file);
        png_image_free(&image);
        entry->failed = 1;
        return 0;
    }
    fclose(file);
    image_width = image.width;
    image_height = image.height;
    png_image_free(&image);
    if (entry->made) {
        /* The part of the mod's picture its card shows, opaque over black
         * as the card's own art is made (art.c); or, for a picture with
         * see-through parts (the title's), as it is. */
        unsigned char *cut = NULL;
        size_t i, j;
        if (entry->source_x + entry->source_w <= (int)image_width && entry->source_y + entry->source_h <= (int)image_height)
            cut = malloc((size_t)entry->source_w * entry->source_h * 4);
        if (!cut) {
            fprintf(stderr, "memories-pc: texture pack: %s changed since the card was made, or out of memory\n", path);
            free(rgba);
            entry->failed = 1;
            return 0;
        }
        for (j = 0; j < (size_t)entry->source_h; j++) {
            for (i = 0; i < (size_t)entry->source_w; i++) {
                const unsigned char *p = rgba + (((entry->source_y + j) * image_width) + entry->source_x + i) * 4;
                unsigned char *q = cut + (j * entry->source_w + i) * 4;
                if (entry->see_through) {
                    memcpy(q, p, 4);
                    continue;
                }
                q[0] = (unsigned char)(p[0] * p[3] / 255);
                q[1] = (unsigned char)(p[1] * p[3] / 255);
                q[2] = (unsigned char)(p[2] * p[3] / 255);
                q[3] = 255;
            }
        }
        free(rgba);
        rgba = cut;
        image_width = (unsigned)entry->source_w;
        image_height = (unsigned)entry->source_h;
    }
    entry->pixels = calloc((size_t)width * height, sizeof(uint16_t));
    if (!entry->pixels) {
        fprintf(stderr, "memories-pc: texture pack: %s: out of memory\n", path);
        free(rgba);
        entry->failed = 1;
        return 0;
    }
    for (y = 0; y < height; y++) {
        int sy0 = (int)((long long)y * image_height / height), sy1 = (int)((long long)(y + 1) * image_height / height);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = entry->crop_left; x < entry->crop_left + entry->crop_width && x < width; x++) {
            int px = x - entry->crop_left;
            int sx0 = (int)((long long)px * image_width / entry->crop_width);
            int sx1 = (int)((long long)(px + 1) * image_width / entry->crop_width);
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            int sx, sy;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1 && sy < (int)image_height; sy++) {
                for (sx = sx0; sx < sx1 && sx < (int)image_width; sx++) {
                    const unsigned char *p = rgba + ((size_t)sy * image_width + sx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            }
            if (n && a / n >= 128) {
                uint16_t color = (uint16_t)(((r / n) >> 3) | (((g / n) >> 3) << 5) | (((b / n) >> 3) << 10));
                /* Black stays opaque: 0x8000 alone is transparent. */
                entry->pixels[y * width + x] = color ? (uint16_t)(color | 0x8000) : TEXTURE_SHADOW_BLACK;
            } else {
                entry->pixels[y * width + x] = 0x8000; /* painted transparent: replaced, by nothing */
            }
        }
    }
    entry->image = rgba;
    entry->image_width = (int)image_width;
    entry->image_height = (int)image_height;
    return 1;
}

/* The pack's image at its own resolution for the scaled picture: u and v
 * are 16.16 texels within the page. 0 not replaced, 1 a color, 2 painted
 * transparent. A color's pixel may be partly transparent (a letter's
 * smoothed edge): bits 24-30 say how much, 0 opaque to 127 all but clear,
 * and the picture mixes it over what lies beneath. */
static int locate_texel(uint32_t offset, int texel, int *row, int *word);
static int head_of(int index);
static int sibling(const Entry *a, const Entry *b);

/* The entry whose crop covers one texel of a word its own entry does not,
 * read the way prepare picked: the same depth and the same palette. Two
 * images sharing a word (card_frames/digit_0 ends in the word digit_1
 * begins in) each own part of it. NULL when none does. */
static const Entry *shared_owner(size_t at, int texel, int *row, int *word)
{
    uint32_t tag;
    int i, head;
    if (!TextureDump_Tags || chosen < 0) return NULL;   /* no tags: nothing to trace */
    tag = TextureDump_Tags[at];
    if (!tag) return NULL;
    i = locate_texel(tag - 1, texel, row, word);
    if (i < 0) return NULL;
    head = head_of(i);
    for (i = head; i < entry_count && (i == head || sibling(&entries[head], &entries[i])); i++) {
        const Entry *entry = &entries[i];
        if (entry->bpp == entries[chosen].bpp && entry->clut_offset == entries[chosen].clut_offset
            && entry->image) {
            return entry;
        }
    }
    return NULL;
}

static int sample(int page_x, int page_y, int depth, int u, int v, uint32_t *rgb)
{
    int per = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    int tu = (u >> 16) & 0xff, tv = (v >> 16) & 0xff;
    int vx = (page_x + tu / per) & (SOFT_GPU_WIDTH - 1), vy = (page_y + tv) & (SOFT_GPU_HEIGHT - 1);
    size_t at = (size_t)vy * SOFT_GPU_WIDTH + vx;
    uint16_t index = entry_of[at];
    const Entry *entry = NULL;
    const unsigned char *p;
    int64_t px, py;
    int row = 0, word = 0, texel_x;
    if (!index) return 0;
    if (!*TextureDump_Cell(vx, vy, (tu % per) * (4 / per))) return 0; /* drawn over since */
    if (index - 1 == chosen_head && chosen >= 0) {   /* prepare's pick: its words, its reading */
        entry = &entries[chosen];
        row = (int)(place_of[at] >> 16);
        word = (int)(place_of[at] & 0xffff);
        if (!entry->image || per != per_word(entry->bpp) ||
            word * per + (tu % per) - entry->crop_left < 0 ||
            word * per + (tu % per) - entry->crop_left >= entry->crop_width) {
            entry = NULL;       /* its crop stops short of this texel */
        }
    }
    if (!entry) {
        /* Two images can share a word, so the neighbour's crop may cover what
         * this one's does not; the picture is still read the way prepare
         * picked, so the primitive's palette decides. */
        entry = shared_owner(at, tu % per, &row, &word);
        if (!entry || per != per_word(entry->bpp)) return 0;
    }
    /* The texel within the image, with the fraction the picture carries. */
    texel_x = word * per + (tu % per) - entry->crop_left;
    if (texel_x < 0 || texel_x >= entry->crop_width) return 0;
    px = ((int64_t)texel_x * 65536 + (u & 0xffff)) * entry->image_width / ((int64_t)entry->crop_width * 65536);
    py = ((int64_t)row * 65536 + (v & 0xffff)) * entry->image_height / ((int64_t)entry->rows * 65536);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= entry->image_width) px = entry->image_width - 1;
    if (py >= entry->image_height) py = entry->image_height - 1;
    p = entry->image + ((size_t)py * entry->image_width + (size_t)px) * 4;
    if (p[3] < PACK_ALPHA_CLEAR) return 2;
    *rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2] | ((uint32_t)(255 - p[3]) >> 1 << 24);
    return 1;
}

/* The entry holding disc byte `offset`, and where in it: the row and the
 * word within the row. -1 if none. */
static int locate(uint32_t offset, int *row, int *word)
{
    int low = 0, high = entry_count, i;
    while (low < high) {
        int middle = (low + high) / 2;
        if (entries[middle].offset <= offset) low = middle + 1;
        else high = middle;
    }
    /* Entries overlap (the same texture drawn as sub-rectangles), so look
     * back through those starting at or before the offset. */
    for (i = low - 1; i >= 0 && i >= low - 64; i--) {
        const Entry *entry = &entries[i];
        uint32_t delta = offset - entry->offset;
        if (entry->row_offsets) {
            int r;
            for (r = 0; r < entry->rows; r++) {
                int32_t start = entry->row_offsets[r];
                if ((int32_t)delta >= start && (int32_t)delta < start + entry->words * 2) {
                    *row = r;
                    *word = (int)(((int32_t)delta - start) / 2);
                    return i;
                }
            }
        } else if (entry->stride) {
            uint32_t r = delta / (entry->stride * 2), c = (delta % (entry->stride * 2)) / 2;
            if (r < (uint32_t)entry->rows && c < (uint32_t)entry->words) {
                *row = (int)r;
                *word = (int)c;
                return i;
            }
        }
    }
    return -1;
}

/* The entry whose replacement covers texel `texel` of the word at disc byte
 * `offset`, and where that texel sits in it. A word holds up to four texels
 * and two images can share it -- card_frames/digit_0 ends in the word
 * digit_1 begins in -- so the owner is asked for per texel, not per word.
 * -1 when no entry's crop covers it. */
static int locate_texel(uint32_t offset, int texel, int *row, int *word)
{
    int low = 0, high = entry_count, i;
    while (low < high) {
        int middle = (low + high) / 2;
        if (entries[middle].offset <= offset) low = middle + 1;
        else high = middle;
    }
    for (i = low - 1; i >= 0 && i >= low - 64; i--) {
        const Entry *entry = &entries[i];
        uint32_t delta = offset - entry->offset;
        int r = -1, c = 0, x;
        if (entry->row_offsets) {
            int at;
            for (at = 0; at < entry->rows; at++) {
                int32_t start = entry->row_offsets[at];
                if ((int32_t)delta >= start && (int32_t)delta < start + entry->words * 2) {
                    r = at;
                    c = (int)(((int32_t)delta - start) / 2);
                    break;
                }
            }
        } else if (entry->stride) {
            uint32_t rows = delta / (entry->stride * 2), words = (delta % (entry->stride * 2)) / 2;
            if (rows < (uint32_t)entry->rows && words < (uint32_t)entry->words) {
                r = (int)rows;
                c = (int)words;
            }
        }
        if (r < 0) continue;
        x = c * per_word(entry->bpp) + texel;
        if (x < entry->crop_left || x >= entry->crop_left + entry->crop_width) continue;
        *row = r;
        *word = c;
        return i;
    }
    return -1;
}

/* Recall: the first bytes on the disc of every image and palette the pack
 * replaces, read once the entries are resolved and sorted by them. The
 * disc layer traces an upload through its ring of recent reads
 * (texture_dump.c); an upload it cannot trace by address is known by these:
 * the duel's card thumbnails come from a table filled when the duel
 * starts, whose sectors have left the ring after a long duel (3D models
 * read since) or were never in it (a state loaded). Only images whose
 * rows follow each other on the disc: an upload is one block. Verify the
 * whole block, even for a unique head: copied or modified data may share
 * its first bytes. This also lets uploads prefer the pack's copy over an
 * identical thumbnail in a recently read full-art record. */
#define RECALL_BYTES 32
typedef struct {
    unsigned char head[RECALL_BYTES]; /* first: compare_recalled */
    uint32_t disc;
    uint32_t bytes;  /* the block's size */
    uint32_t hash;   /* of the whole block */
    const unsigned char *data; /* a made block's bytes (not on the disc), else NULL */
} Recalled;
static Recalled *recalled;
static int recalled_count;

static int compare_recalled(const void *a, const void *b) { return memcmp(a, b, RECALL_BYTES); }

/* 1 with the bytes at a disc offset, 0 when unreadable or one value
 * throughout (a fill would be found anywhere). */
static int read_head(uint32_t offset, unsigned char *out)
{
    unsigned char sectors[2 * 2048];
    int at = (int)(offset % 2048), count = at + RECALL_BYTES > 2048 ? 2 : 1, i;
    if (Memories_DiscReadSectors((int)(offset / 2048), count, sectors) != count) return 0;
    memcpy(out, sectors + at, RECALL_BYTES);
    for (i = 1; i < RECALL_BYTES && out[i] == out[0]; i++) {}
    return i < RECALL_BYTES;
}

static uint32_t hash_bytes(const void *data, size_t size)
{
    const unsigned char *at = data;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *at++) * 16777619u;
    return hash;
}

/* The hash of a disc block, 0 unreadable. */
static uint32_t hash_disc(uint32_t offset, uint32_t bytes)
{
    size_t first = offset % 2048;
    int sectors = (int)((first + bytes + 2047) / 2048);
    uint32_t hash = 0;
    unsigned char *data = malloc((size_t)sectors * 2048);
    if (!data) return 0;
    if (Memories_DiscReadSectors((int)(offset / 2048), sectors, data) == sectors) hash = hash_bytes(data + first, bytes);
    free(data);
    return hash;
}

static void recall_heads(void)
{
    int i, n = 0;
    free(recalled);
    recalled_count = 0;
    recalled = malloc(sizeof(*recalled) * ((size_t)entry_count * 2 + (size_t)block_count + 1));
    if (!recalled) return;
    for (i = 0; i < entry_count; i++) {
        const Entry *entry = &entries[i];
        int whole;
        if (entry->made) continue; /* its blocks, below */
        whole = !entry->row_offsets && (entry->stride == (uint32_t)entry->words || entry->rows == 1);
        if (whole && entry->words * entry->rows * 2 >= RECALL_BYTES && (i == 0 || entry->offset != entries[i - 1].offset) &&
            read_head(entry->offset, recalled[n].head)) {
            recalled[n].disc = entry->offset;
            recalled[n++].bytes = (uint32_t)(entry->words * entry->rows * 2);
        }
        if (entry->clut_entries * 2 >= RECALL_BYTES && read_head(entry->clut_offset, recalled[n].head)) {
            recalled[n].disc = entry->clut_offset;
            recalled[n++].bytes = (uint32_t)(entry->clut_entries * 2);
        }
    }
    for (i = 0; i < n; i++) recalled[i].data = NULL;
    for (i = 0; i < block_count; i++) {
        const Block *block = &blocks[i];
        uint32_t k;
        /* A head of one value is fine (a picture's plain sky): recall
         * checks the whole block. One value throughout would be a fill. */
        for (k = 1; k < block->bytes && block->data[k] == block->data[0]; k++) {}
        if (block->bytes < RECALL_BYTES || k == block->bytes) continue;
        memcpy(recalled[n].head, block->data, RECALL_BYTES);
        recalled[n].disc = block->offset;
        recalled[n].bytes = block->bytes;
        recalled[n].hash = hash_bytes(block->data, block->bytes);
        recalled[n++].data = block->data;
    }
    qsort(recalled, (size_t)n, sizeof(*recalled), compare_recalled);
    for (i = 0; i < n; i++)
        if (!recalled[i].data) recalled[i].hash = hash_disc(recalled[i].disc, recalled[i].bytes);
    recalled_count = n;
}

/* The first recalled head an upload starts with, -1 none. */
static int recalled_first(const uint16_t *pixels)
{
    int low = 0, high = recalled_count;
    while (low < high) {
        int middle = (low + high) / 2;
        if (memcmp(pixels, recalled[middle].head, RECALL_BYTES) > 0) low = middle + 1;
        else high = middle;
    }
    return low < recalled_count && !memcmp(pixels, recalled[low].head, RECALL_BYTES) ? low : -1;
}

/* The disc offset of an upload's first byte, 0 unknown. Any context. */
static uint32_t recall(const uint16_t *pixels, size_t words)
{
    uint32_t found = 0;
    int i;
    if (words * 2 < RECALL_BYTES || (i = recalled_first(pixels)) < 0) return 0;
    /* Match the entire upload, not just a prefix: a larger palette or
     * transfer can start with a small recalled block but differ after it.
     * Two places with the same bytes throughout would each be a guess. */
    for (; i < recalled_count && !memcmp(pixels, recalled[i].head, RECALL_BYTES); i++) {
        if (recalled[i].bytes != words * 2 || !recalled[i].hash ||
            hash_bytes(pixels, recalled[i].bytes) != recalled[i].hash || recalled[i].disc == found)
            continue;
        if (found) return 0;
        found = recalled[i].disc;
    }
    return found;
}

/* The disc is open after the mods are applied, so the archives' places are
 * looked up on first use; an archive the disc lacks drops its images.
 * 1 resolved, 0 nothing left to resolve, -1 no disc yet. */
static int resolve(void)
{
    int i, kept = 0;
    if (resolved) return 1;
    for (i = 0; i < entry_count; i++) {
        if (!entries[i].absolute && archive_start(entries[i].archive) == -2) return -1; /* no disc yet: next time */
    }
    for (i = 0; i < entry_count; i++) {
        Entry *entry = &entries[i];
        long base = entry->absolute ? 0 : archive_start(entry->archive);
        if (base < 0) {
            free(entry->file);
            free(entry->row_offsets);
            free(entry->pixels);
            free(entry->image);
            continue;
        }
        entry->offset += (uint32_t)base;
        if (entry->clut_entries) entry->clut_offset += (uint32_t)base;
        entry->absolute = 1;
        entries[kept++] = *entry;
    }
    entry_count = kept;
    generation++; /* the entries' order changes: their indexes with it */
    if (!entry_count) {
        fprintf(stderr, "memories-pc: texture packs: none of their archives is on the disc\n");
        resolved = 1; /* nothing to paint, and nothing to ask again every frame */
        return 0;
    }
    qsort(entries, (size_t)entry_count, sizeof(*entries), compare);
    recall_heads();
    resolved = 1;
    fprintf(stderr, "memories-pc: texture packs: %d images\n", entry_count);
    return 1;
}

static void wait_for_image(int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < waiting_count && i < WAITING_AREAS; i++) {
        if (waiting[i].x == x && waiting[i].y == y && waiting[i].w == w && waiting[i].h == h) return;
    }
    if (waiting_count < WAITING_AREAS) {
        waiting[waiting_count].x = x;
        waiting[waiting_count].y = y;
        waiting[waiting_count].w = w;
        waiting[waiting_count].h = h;
    }
    if (waiting_count <= WAITING_AREAS) waiting_count++;
}

/* After an upload: every word of it that a pack image covers gets the
 * image's texels in the shadow. */
static void paint(int x, int y, int w, int h)
{
    int i, j, asked = 0;
    if (!resolved) {
        wanted_resolve = 1;
        return;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int vx = (x + i) & (SOFT_GPU_WIDTH - 1), vy = (y + j) & (SOFT_GPU_HEIGHT - 1), row, word, index, k, per;
            size_t at = (size_t)vy * SOFT_GPU_WIDTH + vx;
            uint32_t tag = TextureDump_Tags[at], place;
            uint16_t was = entry_of[at];
            Entry *entry;
            entry_of[at] = 0;
            if (!tag || (index = locate(tag - 1, &row, &word)) < 0) {
                if (was) map_generation++;
                continue;
            }
            index = head_of(index);
            entry = &entries[index];
            if (!entry->pixels) {
                if (!entry->failed) {
                    entry->wanted = 1;
                    wanted_images = 1;
                    asked = 1;
                }
                if (was) map_generation++;
                continue;
            }
            per = per_word(entry->bpp);
            place = ((uint32_t)row << 16) | (uint32_t)word;
            if (was != index + 1 || place_of[at] != place) map_generation++;
            entry_of[at] = (uint16_t)(index + 1);
            place_of[at] = place;
            for (k = 0; k < per; k++) {
                /* Usually the word's own entry owns the texel; where two
                 * images share the word, each owns part of it. */
                const Entry *owner = entry;
                int orow = row, oword = word, x = word * per + k, sub = k * (4 / per), s;
                uint16_t color;
                if (x < entry->crop_left || x >= entry->crop_left + entry->crop_width) {
                    int other = locate_texel(tag - 1, k, &orow, &oword);
                    if (other < 0) continue;            /* nobody replaces this texel */
                    owner = &entries[head_of(other)];
                    if (!owner->pixels) {
                        if (!owner->failed) {
                            ((Entry *)owner)->wanted = 1;
                            wanted_images = 1;
                            asked = 1;
                        }
                        continue;
                    }
                }
                color = owner->pixels[orow * owner->words * per + oword * per + k];
                for (s = 0; s < 4 / per; s++) *TextureDump_Cell(vx, vy, sub + s) = color;
            }
        }
    }
    if (asked) wait_for_image(x, y, w, h);
}

/* The maps follow the words as the shadow does (texture_dump.c): cleared
 * with them, and moved with them. */
static void forget(int x, int y, int w, int h)
{
#ifdef MEMORIES_TRANSLATED
    for (int j = 0; j < h; j++) {
        int left = w, column = x & (SOFT_GPU_WIDTH - 1);
        size_t row = (size_t)((y + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH;
        while (left > 0) {
            int count = SOFT_GPU_WIDTH - column;
            if (count > left) count = left;
            map_generation += GuestRuntime_Clear16(entry_of + row + column, (size_t)count);
            left -= count;
            column = 0;
        }
    }
#else
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t at = (size_t)((y + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((x + i) & (SOFT_GPU_WIDTH - 1));
            if (entry_of[at]) {
                entry_of[at] = 0;
                map_generation++;
            }
        }
    }
#endif
}

static void follow(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    /* Words still waiting for an image may be among those moved. */
    if (waiting_count) wait_for_image(dx, dy, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t from = (size_t)((sy + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((sx + i) & (SOFT_GPU_WIDTH - 1));
            size_t to = (size_t)((dy + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((dx + i) & (SOFT_GPU_WIDTH - 1));
            if (entry_of[to] != entry_of[from] || (entry_of[from] && place_of[to] != place_of[from])) {
                entry_of[to] = entry_of[from];
                place_of[to] = place_of[from];
                map_generation++;
            }
        }
    }
}

/* Once per textured primitive: a pack image applies if the word of a texel
 * it samples was painted from one and the primitive reads it at that
 * image's depth with its palette. Among the readings of the same words the
 * one whose palette this is: 1 when it is the head, whose colors the
 * shadow holds, so the 1x picture shows them too; 2 for another reading,
 * for the scaled picture alone (sample). */
static int prepare(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    int per = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    int vx = (page_x + (u & 0xff) / per) & (SOFT_GPU_WIDTH - 1), vy = (page_y + (v & 0xff)) & (SOFT_GPU_HEIGHT - 1);
    uint16_t index = entry_of[vy * SOFT_GPU_WIDTH + vx];
    int bpp = depth == 0 ? 4 : depth == 1 ? 8 : 16, head, i;
    uint32_t clut = 0;
    chosen_head = chosen = -1;
    if (!index) return 0;
    head = index - 1;
    if (entries[head].clut_entries) {
        clut = TextureDump_Tags[(clut_y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (clut_x & (SOFT_GPU_WIDTH - 1))];
    }
    for (i = head; i < entry_count && (i == head || sibling(&entries[head], &entries[i])); i++) {
        Entry *entry = &entries[i];
        if (entry->bpp != bpp) continue;
        if (entry->clut_entries && (!clut || clut - 1 != entry->clut_offset)) continue;
        if (!entry->image) {
            /* Read between frames (TexturePack_Service); the head's colors
             * are not this reading's, so nothing replaces until then. */
            if (!entry->failed && !entry->wanted) {
                entry->wanted = 1;
                wanted_images = 1;
            }
            return 0;
        }
        chosen_head = head;
        chosen = i;
        return i == head ? 1 : 2;
    }
    return 0;
}

static void free_entries(void)
{
    int i;
    for (i = 0; i < entry_count; i++) {
        free(entries[i].file);
        free(entries[i].row_offsets);
        free(entries[i].pixels);
        free(entries[i].image);
    }
    free(entries);
    entries = NULL;
    entry_count = 0;
    free(recalled);
    recalled = NULL;
    recalled_count = 0;
}

/* What was wrong with a pack's entries, kind by kind: how many, and the
 * first one's file, for the one line the Mods window has room for. */
typedef struct {
    int count;
    char first[64];
} Problem;

static void problem(Problem *kind, const char *file)
{
    if (!kind->count++) snprintf(kind->first, sizeof(kind->first), "%s", file ? file : "(no file)");
}

static void describe(char *out, size_t size, const Problem *kind, const char *one, const char *many)
{
    size_t length = strlen(out);
    if (!kind->count || length >= size) return;
    snprintf(out + length, size - length, "%s%d %s (first: %s)", length ? "; " : "", kind->count,
             kind->count == 1 ? one : many, kind->first);
}

/* The images are read when the game first needs them, so at load a file is
 * only checked to be there: a missing one shows as the pack is applied, not
 * when a screen happens to want the picture. It is not opened. A pack names
 * thousands of images and loads while the frame waits (the Mods window
 * applies it from inside a present), and each open can be slow on a cold
 * disc, more so with an on-access virus scanner: applying a 4515-image pack
 * held the frame over 20 s and Windows closed the game as not responding
 * (26 September 2026, Windows, tmp/pc hang report). A file that is there but is no
 * PNG fails when it is first decoded (load_pixels), which reports it and
 * keeps the original texture. */
static int readable(const char *path)
{
    return access(path, R_OK) == 0;
}

/* The size of the block an upload of the image or palette starting at a
 * disc offset covers: 0 when the pack has none there. */
static int block_at(uint32_t disc, int *words, int *rows)
{
    int i;
    for (i = 0; i < entry_count; i++) {
        const Entry *entry = &entries[i];
        if (entry->offset == disc && !entry->row_offsets && (entry->stride == (uint32_t)entry->words || entry->rows == 1)) {
            *words = entry->words;
            *rows = entry->rows;
            return 1;
        }
        if (entry->clut_entries && entry->clut_offset == disc) {
            *words = entry->clut_entries;
            *rows = 1;
            return 1;
        }
    }
    return 0;
}

/* Whether VRAM's block at x,y holds the disc's bytes from `disc` on, or a
 * made block's (`made_data`). */
static int holds_disc(const uint16_t *vram, uint32_t disc, const unsigned char *made_data, int x, int y, int words,
                      int rows)
{
    size_t bytes = (size_t)words * rows * 2, first = disc % 2048;
    int sectors = (int)((first + bytes + 2047) / 2048), j, same = 1;
    unsigned char *data;
    if (made_data) {
        for (j = 0; same && j < rows; j++)
            same = memcmp(vram + (size_t)(y + j) * SOFT_GPU_WIDTH + x, made_data + (size_t)j * words * 2,
                          (size_t)words * 2) == 0;
        return same;
    }
    data = malloc((size_t)sectors * 2048);
    if (!data) return 0;
    if (Memories_DiscReadSectors((int)(disc / 2048), sectors, data) != sectors) same = 0;
    for (j = 0; same && j < rows; j++)
        same = memcmp(vram + (size_t)(y + j) * SOFT_GPU_WIDTH + x, data + first + (size_t)j * words * 2,
                      (size_t)words * 2) == 0;
    free(data);
    return same;
}

/* After a state load VRAM holds its pictures with the disc's tags the state
 * kept, if it kept them (state.c), but not those of the port's own images
 * nor any from an older state: a block left untagged that starts with a
 * recalled head and matches the disc throughout is tagged as its upload
 * was. Between frames, with the entries resolved; then all of VRAM is
 * painted.
 *
 * The disc holds some pictures twice, byte for byte: 21 of the Free Duel
 * portraits (and their palettes) are also campaign portraits, each pair a
 * separate entry of the pack (issue #278). Such a block is settled by its
 * neighbours: a screen's pictures are uploaded side by side from one file,
 * so the copy whose disc offset lies nearest the tags around the block
 * (once the unique ones are known) is the one that was uploaded. */
#define TWINS_MAX 4
typedef struct {
    int x, y, count;
    uint32_t disc[TWINS_MAX];
    int words[TWINS_MAX], rows[TWINS_MAX];
} Twins;

static void tag_block(int x, int y, uint32_t disc, int words, int rows)
{
    int i, j;
    for (j = 0; j < rows; j++)
        for (i = 0; i < words; i++)
            TextureDump_Tags[(size_t)(y + j) * SOFT_GPU_WIDTH + x + i] = disc + (uint32_t)(j * words + i) * 2 + 1;
}

/* How far `disc` lies from the disc offsets tagged just around the block at
 * x,y (words x rows): the row above and below and the word either side.
 * 0xFFFFFFFF with none tagged. */
static uint32_t neighbour_distance(int x, int y, int words, int rows, uint32_t disc)
{
    uint32_t best = 0xFFFFFFFFu;
    int i, j;
    for (j = -1; j <= rows; j++) {
        int inside = j >= 0 && j < rows;
        if (y + j < 0 || y + j >= SOFT_GPU_HEIGHT) continue;
        for (i = -1; i <= words; i++) {
            uint32_t tag, at, gap;
            if (inside && i >= 0 && i < words) i = words; /* the block's own words */
            if (x + i < 0 || x + i >= SOFT_GPU_WIDTH) continue;
            tag = TextureDump_Tags[(size_t)(y + j) * SOFT_GPU_WIDTH + x + i];
            if (!tag || tag - 1 >= TEXTURE_MADE_BASE) continue;
            at = tag - 1;
            gap = at > disc ? at - disc : disc - at;
            if (gap < best) best = gap;
        }
    }
    return best;
}

static void rediscover(void)
{
    const uint16_t *vram = SoftGpu_Vram();
    Twins *twins = NULL;
    int x, y, twin_count = 0, twin_room = 0, progress;
    if (!recalled_count || !vram || !TextureDump_Tags) return;
    for (y = 0; y < SOFT_GPU_HEIGHT; y++) {
        for (x = 0; x + RECALL_BYTES / 2 <= SOFT_GPU_WIDTH; x++) {
            const uint16_t *at = vram + (size_t)y * SOFT_GPU_WIDTH + x;
            Twins found;
            int w, r, k;
            if (TextureDump_Tags[(size_t)y * SOFT_GPU_WIDTH + x] || (k = recalled_first(at)) < 0) continue;
            /* The places sharing the head whose bytes the block holds. */
            found.x = x;
            found.y = y;
            found.count = 0;
            for (; k < recalled_count && !memcmp(at, recalled[k].head, RECALL_BYTES); k++) {
                int seen = 0, n;
                for (n = 0; n < found.count && n < TWINS_MAX; n++) seen |= found.disc[n] == recalled[k].disc;
                if (seen || !block_at(recalled[k].disc, &w, &r) || x + w > SOFT_GPU_WIDTH ||
                    y + r > SOFT_GPU_HEIGHT || !holds_disc(vram, recalled[k].disc, recalled[k].data, x, y, w, r))
                    continue;
                if (found.count < TWINS_MAX) {
                    found.disc[found.count] = recalled[k].disc;
                    found.words[found.count] = w;
                    found.rows[found.count] = r;
                }
                found.count++;
            }
            if (found.count == 1) {
                tag_block(x, y, found.disc[0], found.words[0], found.rows[0]);
                x += found.words[0] - 1;
            } else if (found.count > 1 && found.count <= TWINS_MAX) {
                if (twin_count == twin_room) {
                    int room = twin_room ? twin_room * 2 : 64;
                    Twins *grown = realloc(twins, sizeof(*twins) * (size_t)room);
                    if (!grown) continue;
                    twins = grown;
                    twin_room = room;
                }
                twins[twin_count++] = found;
            }
        }
    }
    /* The twins, nearest neighbours first: one settled can settle the next. */
    do {
        int i, n;
        progress = 0;
        for (i = 0; i < twin_count; i++) {
            Twins *block = &twins[i];
            uint32_t best = 0xFFFFFFFFu, second = 0xFFFFFFFFu;
            int chosen = -1;
            if (!block->count || TextureDump_Tags[(size_t)block->y * SOFT_GPU_WIDTH + block->x]) continue;
            for (n = 0; n < block->count; n++) {
                uint32_t gap = neighbour_distance(block->x, block->y, block->words[n], block->rows[n], block->disc[n]);
                if (gap < best) {
                    second = best;
                    best = gap;
                    chosen = n;
                } else if (gap < second) {
                    second = gap;
                }
            }
            if (chosen < 0 || best == second) continue; /* no neighbours, or no nearer copy */
            tag_block(block->x, block->y, block->disc[chosen], block->words[chosen], block->rows[chosen]);
            block->count = 0;
            progress = 1;
        }
    } while (progress);
    free(twins);
}

static void restored(void) { wanted_rediscover = 1; }

/* The hooks, the shadow and the maps, once there are entries: the new ones
 * are sorted in with the others between frames (TexturePack_Service). */
static int install(void)
{
    if (!TextureDump_EnableShadow()) return 0;
    if (!entry_of) entry_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*entry_of));
    if (!place_of) place_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*place_of));
    if (!entry_of || !place_of) return 0;
    TextureDump_Paint = paint;
    TextureDump_Prepare = prepare;
    TextureDump_Sample = sample;
    TextureDump_Forget = forget;
    TextureDump_Follow = follow;
    TextureDump_Recall = recall;
    TextureDump_Restored = restored;
    /* The disc's directory and the images wait for TexturePack_Service,
     * between frames: a mod is applied while the game starts, before the
     * disc is open, and an upload can ask for an image from the interrupt
     * tick. Until then paint only notes what it needs and sample sees no
     * image. */
    resolved = 0; /* the new entries, and the order, with the others */
    wanted_resolve = 1;
    generation++;
    return 1;
}

/* Made entries `first` on joined to the entries, each with its own copy of
 * the file's path (free_entries frees it). 0 out of memory. */
static int add_made(int first)
{
    Entry *more;
    int i;
    if (first >= made_count) return 1;
    more = realloc(entries, (size_t)(entry_count + made_count - first) * sizeof(*entries));
    if (!more) return 0;
    entries = more;
    for (i = first; i < made_count; i++) {
        Entry *entry = &entries[entry_count];
        *entry = made[i];
        entry->file = strdup(made[i].file);
        if (!entry->file) return 0;
        entry_count++;
    }
    return 1;
}

/* The place of a block of bytes: an identical one's, else a new one. 0
 * when there is no room. */
static uint32_t block_place(const void *data, uint32_t bytes)
{
    Block *more;
    int i;
    for (i = 0; i < block_count; i++) {
        if (blocks[i].bytes == bytes && !memcmp(blocks[i].data, data, bytes)) return blocks[i].offset;
    }
    if (made_next - TEXTURE_MADE_BASE > 0x3FFFFFFFu - bytes) return 0;
    more = realloc(blocks, (size_t)(block_count + 1) * sizeof(*blocks));
    if (!more) return 0;
    blocks = more;
    blocks[block_count].data = malloc(bytes);
    if (!blocks[block_count].data) return 0;
    memcpy(blocks[block_count].data, data, bytes);
    blocks[block_count].bytes = bytes;
    blocks[block_count].offset = made_next;
    made_next += (bytes + 0xFFFu) & ~0xFFFu;
    return blocks[block_count++].offset;
}

static int add_made_entry(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                          const char *file, int x, int y, int w, int h, int see_through)
{
    Entry *more, entry;
    int i;
    if (!pixels || !file || words < 1 || words > SOFT_GPU_WIDTH || rows < 1 || rows > SOFT_GPU_HEIGHT ||
        (bpp != 4 && bpp != 8 && bpp != 16) || clut_entries < 0 || (clut_entries && !clut) || x < 0 || y < 0 || w < 1 ||
        h < 1 || entry_count >= 65535)
        return 0;
    memset(&entry, 0, sizeof(entry));
    entry.offset = block_place(pixels, (uint32_t)words * (uint32_t)rows * 2);
    entry.clut_offset = clut_entries ? block_place(clut, (uint32_t)clut_entries * 2) : 0;
    if (!entry.offset || (clut_entries && !entry.clut_offset)) return 0;
    for (i = 0; i < made_count; i++) {
        if (made[i].offset == entry.offset && made[i].clut_offset == entry.clut_offset && made[i].bpp == bpp)
            return 1; /* the same picture: the first file is the one drawn */
    }
    entry.words = words;
    entry.rows = rows;
    entry.bpp = bpp;
    entry.stride = (uint32_t)words;
    entry.clut_entries = clut_entries;
    entry.crop_width = words * per_word(bpp);
    entry.absolute = 1;
    entry.made = 1;
    entry.see_through = see_through;
    entry.source_x = x;
    entry.source_y = y;
    entry.source_w = w;
    entry.source_h = h;
    entry.position = made_count;
    more = realloc(made, (size_t)(made_count + 1) * sizeof(*made));
    if (!more) return 0;
    made = more;
    entry.file = strdup(file);
    if (!entry.file) return 0;
    made[made_count++] = entry;
    if (!add_made(made_count - 1) || !install()) return 0;
    return 1;
}

int TexturePack_AddMade(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                        const char *file, int x, int y, int w, int h)
{
    return add_made_entry(pixels, words, rows, bpp, clut, clut_entries, file, x, y, w, h, 0);
}

int TexturePack_AddMadeSeeThrough(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                                  const char *file, int x, int y, int w, int h)
{
    return add_made_entry(pixels, words, rows, bpp, clut, clut_entries, file, x, y, w, h, 1);
}

/* A mod that writes "assets": "<directory>" ships the pictures instead of
 * listing them: every PNG under it whose path is a name in the catalog
 * replaces that image. The names a mod ships are far fewer than the
 * catalog, so the directory is walked once and searched per name. */
typedef struct {
    char **names;            /* the name the file stands for, without ".png" */
    char **paths;            /* where the file is under the directory, without ".png" */
    char **settings;         /* the setting that switches it, or NULL */
    unsigned char *used;     /* a name the catalog holds: the rest are reported */
    int count, room;
} Folder;

static void folder_free(Folder *folder)
{
    int i;
    for (i = 0; i < folder->count; i++) {
        free(folder->names[i]);
        free(folder->paths[i]);
        free(folder->settings[i]);
    }
    free(folder->names);
    free(folder->paths);
    free(folder->settings);
    free(folder->used);
    memset(folder, 0, sizeof(*folder));
}

/* The catalog's names in order, so a path can be tested against them by
 * halving. Built once; the table is constant. */
static int by_name(const void *a, const void *b)
{
    return strcmp(asset_specs[*(const int *)a].name, asset_specs[*(const int *)b].name);
}

static int catalog_holds(const char *name)
{
    static int order[ASSET_COUNT], ready;
    int low = 0, high = ASSET_COUNT - 1;
    if (!ready) {
        int i;
        for (i = 0; i < ASSET_COUNT; i++) order[i] = i;
        qsort(order, ASSET_COUNT, sizeof(*order), by_name);
        ready = 1;
    }
    while (low <= high) {
        int middle = low + (high - low) / 2;
        int which = strcmp(asset_specs[order[middle]].name, name);
        if (!which) return 1;
        if (which < 0) low = middle + 1;
        else high = middle - 1;
    }
    return 0;
}

static int folder_add(Folder *folder, const char *name, const char *path, const char *setting)
{
    if (folder->count == folder->room) {
        int room = folder->room ? folder->room * 2 : 32;
        char **names = realloc(folder->names, (size_t)room * sizeof(*names));
        char **paths = realloc(folder->paths, (size_t)room * sizeof(*paths));
        char **settings = realloc(folder->settings, (size_t)room * sizeof(*settings));
        unsigned char *used = realloc(folder->used, (size_t)room);
        if (names) folder->names = names;
        if (paths) folder->paths = paths;
        if (settings) folder->settings = settings;
        if (used) folder->used = used;
        if (!names || !paths || !settings || !used) return 0;
        folder->room = room;
    }
    if (!(folder->names[folder->count] = strdup(name))) return 0;
    if (!(folder->paths[folder->count] = strdup(path))) {
        free(folder->names[folder->count]);
        return 0;
    }
    folder->settings[folder->count] = NULL;
    if (setting && !(folder->settings[folder->count] = strdup(setting))) {
        free(folder->names[folder->count]);
        free(folder->paths[folder->count]);
        return 0;
    }
    folder->used[folder->count++] = 0;
    return 1;
}

/* The name a file's path stands for, and the setting that switches it.
 *
 * A path that is a name is that name, switched by the mod's setting of the
 * same name as its folder if it declares one (assets/attributes/light.png is
 * attributes/light, switched by "attributes"). Otherwise its first part may
 * be one of the mod's settings, and the rest the name:
 * assets/hd_art/card_art/001.png is card_art/001, switched off with the
 * mod's "hd_art". The whole path is tried first, so a directory that is
 * also a folder of the catalog (assets/card_art/...) keeps meaning it.
 * 0 when the path is neither, which is reported. */
static int folder_resolve(const char *path, int (*part)(const char *, void *), void *context,
                          const char **name, char *setting, size_t setting_size)
{
    const char *rest = strchr(path, '/');
    size_t length;
    *name = path;
    setting[0] = '\0';
    if (catalog_holds(path)) {
        /* A setting named after the folder switches it, with nothing to nest:
         * a mod declaring "attributes" switches assets/attributes/... The
         * name is the whole path either way, so this cannot change which
         * image a file stands for. */
        if (rest && part) {
            size_t part_length = (size_t)(rest - path);
            if (part_length < setting_size) {
                memcpy(setting, path, part_length);
                setting[part_length] = '\0';
                if (part(setting, context) < 0) setting[0] = '\0';   /* not declared */
            }
        }
        return 1;
    }
    if (!rest || (length = (size_t)(rest - path)) >= setting_size) return 0;
    memcpy(setting, path, length);
    setting[length] = '\0';
    /* part() tells a setting the mod declares (1 or 0) from one it does not (-1). */
    if (!part || part(setting, context) < 0 || !catalog_holds(rest + 1)) {
        setting[0] = '\0';
        return 0;
    }
    *name = rest + 1;
    return 1;
}

/* Every PNG under `root`, named by its path below `base` without the
 * suffix. Depth is bounded: the longest name the catalog holds is four
 * parts, one more for a setting's directory, and a link loop would
 * otherwise never end. */
static void folder_walk(Folder *folder, const char *root, const char *below, int depth,
                        int (*part)(const char *, void *), void *context)
{
    char path[1200], name[256];
    DIR *directory;
    struct dirent *entry;
    if (depth > 5 || !(directory = opendir(root))) return;
    while ((entry = readdir(directory))) {
        struct stat info;
        size_t length;
        if (entry->d_name[0] == '.') continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= sizeof(path)) continue;
        if ((size_t)snprintf(name, sizeof(name), "%s%s%s", below, *below ? "/" : "", entry->d_name) >= sizeof(name))
            continue;
        if (stat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            folder_walk(folder, path, name, depth + 1, part, context);
            continue;
        }
        length = strlen(name);
        if (length < 5 || strcmp(name + length - 4, ".png")) continue;
        name[length - 4] = '\0';
        {
            const char *stands_for;
            char setting[64];
            folder_resolve(name, part, context, &stands_for, setting, sizeof(setting));
            if (!folder_add(folder, stands_for, name, setting[0] ? setting : NULL)) break;
        }
    }
    closedir(directory);
}

/* The folder sorted, so a name is found by halving rather than by walking
 * the catalog against every file. `used` follows its name. */
static void folder_sort(Folder *folder)
{
    int i, j;
    for (i = 1; i < folder->count; i++) {   /* insertion: the setting moves with the name */
        char *name = folder->names[i], *path = folder->paths[i], *setting = folder->settings[i];
        for (j = i; j > 0 && strcmp(folder->names[j - 1], name) > 0; j--) {
            folder->names[j] = folder->names[j - 1];
            folder->paths[j] = folder->paths[j - 1];
            folder->settings[j] = folder->settings[j - 1];
        }
        folder->names[j] = name;
        folder->paths[j] = path;
        folder->settings[j] = setting;
    }
    for (i = 0; i < folder->count; i++) folder->used[i] = 0;
}

/* The file to use for a name, and every file that stands for it marked as
 * used. A mod can ship the same asset under two settings -- one part of it
 * redrawn, another left alone -- so the one whose setting is on is the one
 * taken; otherwise the first, which is then left out as switched off. */
static int folder_find(Folder *folder, const char *name, int (*part)(const char *, void *), void *context)
{
    int low = 0, high = folder->count - 1, first, i, chosen = -1;
    while (low <= high) {
        int middle = low + (high - low) / 2, order = strcmp(folder->names[middle], name);
        if (order < 0) low = middle + 1;
        else high = middle - 1;
    }
    first = low;        /* the first name not less than `name` */
    if (first >= folder->count || strcmp(folder->names[first], name)) return -1;
    for (i = first; i < folder->count && !strcmp(folder->names[i], name); i++) {
        const char *setting = folder->settings[i];
        folder->used[i] = 1;
        if (chosen < 0) chosen = i;
        if (setting && part && part(setting, context) == 0) continue;   /* switched off */
        return i;
    }
    return chosen;      /* every copy is switched off: the first says so */
}

static int load_entries(const char *from, const JsonValue *list, int named, Folder *folder, unsigned rank,
                        int (*part)(const char *setting, void *context), void *context,
                        char *problems, size_t problems_size)
{
    char path[1200];
    const JsonValue *item;
    int count, room, before = entry_count, position = 0, full = 0, off = 0;
    Problem unreadable = {0}, outside = {0}, measures = {0}, unaddressed = {0}, row_count = {0}, undeclared = {0};
    Problem unknown = {0}, malformed = {0};
    Entry *more;
    if (problems && problems_size) problems[0] = '\0';
    if (named && !folder) {
        for (item = Json_At(list, 0); item; item = Json_Next(item)) {
            int i;
            for (i = 0; i < ASSET_COUNT; i++) if (!strcmp(Json_Name(item), asset_specs[i].name)) break;
            if (i == ASSET_COUNT) problem(&unknown, Json_Name(item));
        }
    }
    count = named ? ASSET_COUNT : Json_Count(list);
    /* Named loads walk the whole catalog but only the specs the mod asked for
     * become entries, and one name can cover several (a card UI sprite that
     * every package repeats): room for those, not for all of the catalog. */
    room = count;
    if (named) {
        int i;
        for (room = 0, i = 0; i < ASSET_COUNT; i++)
            if (folder ? folder_find(folder, asset_specs[i].name, part, context) >= 0
                       : Json_Member(list, asset_specs[i].name) != NULL) room++;
    }
    if (folder) memset(folder->used, 0, (size_t)folder->count);   /* the count pass marked them */
    more = realloc(entries, (size_t)(entry_count + (room ? room : 1)) * sizeof(*entries));
    if (more) {
        entries = more;
        memset(entries + entry_count, 0, (size_t)(room ? room : 1) * sizeof(*entries));
    }
    /* Walked in one pass: a pack can list tens of thousands of images. */
    item = Json_At(list, 0);
    for (; more && position < count; position++, item = named ? item : Json_Next(item)) {
        const AssetSpec *asset = named ? &asset_specs[position] : NULL;
        const JsonValue *rows = NULL, *row, *setting = NULL;
        const char *file = NULL, *archive;
        char shipped[260];
        Entry *entry = &entries[entry_count];
        double offset, clut_offset, words, rows_count, bpp, stride, crop_left, crop_width;
        if (folder) {   /* the name is the picture's own path under the directory */
            int at = folder_find(folder, asset->name, part, context);
            const char *switched;
            if (at < 0) continue;
            switched = folder->settings[at];
            /* The file's own path, not the name: the two differ when a
             * setting's directory sits above it. */
            if ((size_t)snprintf(shipped, sizeof(shipped), "%s.png", folder->paths[at]) >= sizeof(shipped))
                continue;
            file = shipped;
            if (switched) {
                /* A directory named after one of the mod's settings switches
                 * everything under it, as an entry's "setting" does. */
                int on = part ? part(switched, context) : -1;
                if (!on) {
                    off++;
                    continue;
                }
                if (on < 0) problem(&undeclared, switched);
            }
        } else {
            if (asset) item = Json_Member(list, asset->name);
            if (!item) continue;
            rows = Json_Member(item, "row_offsets");
            file = Json_String(Json_Member(item, named ? "image" : "file"), NULL);
            setting = Json_Member(item, "setting");
            if (asset) {
                const JsonValue *member;
                if (Json_TypeOf(item) != JSON_OBJECT || !file || !*file ||
                    (setting && Json_TypeOf(setting) != JSON_STRING)) {
                    problem(&malformed, asset->name);
                    continue;
                }
                for (member = Json_At(item, 0); member; member = Json_Next(member)) {
                    const char *key = Json_Name(member);
                    if (strcmp(key, "image") && strcmp(key, "setting")) problem(&malformed, asset->name);
                }
                rows = NULL;
            }
        }
        archive = asset ? asset->archive : Json_String(Json_Member(item, "archive"), NULL);
        if (setting) { /* a part of the pack the mod's settings switch off, or a setting it lacks: then used */
            const char *key = Json_String(setting, "");
            int on = part && *key ? part(key, context) : -1;
            if (!on) {
                off++;
                continue;
            }
            if (on < 0) problem(&undeclared, *key ? key : file);
        }
        if (!file || !archive || strlen(archive) >= sizeof(entry->archive)) { /* not addressed on the disc */
            problem(&unaddressed, file);
            continue;
        }
        if (!Paths_Contained(file)) { /* "../../x.png" would reach past the mod */
            problem(&outside, file);
            continue;
        }
        if (entry_count >= 65535) { /* entry_of holds index + 1 in 16 bits */
            full = 1;
            break;
        }
        offset = asset ? asset->offset : Json_Number(Json_Member(item, "offset"), -1);
        words = asset ? asset->words : Json_Number(Json_Member(item, "words"), 0);
        rows_count = asset ? asset->rows : Json_Number(Json_Member(item, "rows"), 0);
        bpp = asset ? asset->bpp : Json_Number(Json_Member(item, "bpp"), 0);
        clut_offset = asset ? asset->clut_offset : Json_Number(Json_Member(item, "clut_offset"), 0);
        stride = asset ? asset->stride : Json_Number(Json_Member(item, "stride"), words); /* rows contiguous unless said */
        crop_left = asset ? asset->crop_left : Json_Number(Json_Member(item, "crop_left"), 0);
        /* What the game could upload: a texture page at most 1024 words
         * wide, 512 rows, from an offset on a CD. */
        if (!(offset >= 0 && offset < 1e9) || !(clut_offset >= 0 && clut_offset < 1e9) ||
            !(words >= 1 && words <= SOFT_GPU_WIDTH) || !(rows_count >= 1 && rows_count <= SOFT_GPU_HEIGHT) ||
            (bpp != 4 && bpp != 8 && bpp != 16) || !(stride >= 1 && stride <= 1e6) ||
            !(crop_left >= 0 && crop_left < words * per_word((int)bpp))) {
            problem(&measures, file);
            continue;
        }
        crop_width = asset ? asset->width : Json_Number(Json_Member(item, "width"), words * per_word((int)bpp) - crop_left);
        if (!(crop_width >= 1 && crop_left + crop_width <= words * per_word((int)bpp))) {
            problem(&measures, file);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", from, file);
        if (!readable(path)) {
            problem(&unreadable, file);
            continue;
        }
        strcpy(entry->archive, archive);
        entry->offset = (uint32_t)offset;
        entry->words = (int)words;
        entry->rows = (int)rows_count;
        entry->bpp = (int)bpp;
        entry->clut_entries = asset ? asset->clut_entries : (int)Json_Number(Json_Member(item, "clut_entries"), 0);
        entry->clut_offset = entry->clut_entries ? (uint32_t)clut_offset : 0;
        entry->stride = (uint32_t)stride;
        entry->crop_left = (int)crop_left;
        entry->crop_width = (int)crop_width;
        entry->rank = rank;
        entry->position = position;
        if (rows && Json_TypeOf(rows) != JSON_NULL && Json_Count(rows) != entry->rows) {
            problem(&row_count, file); /* read with the stride instead, which may well be wrong */
        } else if (rows && Json_Count(rows) == entry->rows) {
            int r;
            entry->row_offsets = malloc(sizeof(int32_t) * (size_t)entry->rows);
            for (r = 0, row = Json_At(rows, 0); entry->row_offsets && row; r++, row = Json_Next(row)) {
                entry->row_offsets[r] = (int32_t)Json_Number(row, 0);
            }
            entry->stride = 0;
        }
        if (entry->words <= 0 || entry->rows <= 0 || entry->rows > 512 || (!entry->stride && !entry->row_offsets)) {
            free(entry->row_offsets);
            entry->row_offsets = NULL;
            problem(&measures, file);
            continue;
        }
        entry->file = malloc(strlen(from) + strlen(file) + 2); /* the whole path: packs from several directories add up */
        if (!entry->file) {
            free(entry->row_offsets);
            entry->row_offsets = NULL;
            problem(&unreadable, file);
            continue;
        }
        sprintf(entry->file, "%s/%s", from, file);
        entry_count++;
    }
    if (folder) {   /* a picture the catalog has no name for: a typo, most likely */
        int i;
        for (i = 0; i < folder->count; i++)
            if (!folder->used[i]) problem(&unknown, folder->paths[i]);
    }
    if (problems && problems_size) {
        describe(problems, problems_size, &unknown, "unknown asset", "unknown assets");
        describe(problems, problems_size, &malformed, "invalid asset image object", "invalid asset image objects");
        describe(problems, problems_size, &unreadable, "image could not be read", "images could not be read");
        describe(problems, problems_size, &outside, "image is outside the pack", "images are outside the pack");
        describe(problems, problems_size, &measures, "image has measures out of range",
                 "images have measures out of range");
        describe(problems, problems_size, &row_count, "image's row_offsets do not match its rows",
                 "images' row_offsets do not match their rows");
        describe(problems, problems_size, &unaddressed, "image names no file or archive",
                 "images name no file or archive");
        describe(problems, problems_size, &undeclared, "image names a setting the mod does not declare",
                 "images name a setting the mod does not declare");
        if (full) {
            size_t length = strlen(problems);
            if (length < problems_size)
                snprintf(problems + length, problems_size - length, "%sonly 65535 images fit; the rest were left out",
                         length ? "; " : "");
        }
        if (*problems) fprintf(stderr, "memories-pc: texture pack %s: %s\n", from, problems);
    }
    if (entry_count == before) {
        if (!entry_count) free_entries();
        if (off || (named && (folder ? !folder->count : !Json_Count(list)))) { /* every part off: nothing wrong with the pack */
            fprintf(stderr, "memories-pc: texture pack %s: every image is switched off\n", from);
            return 0;
        }
        fprintf(stderr, "memories-pc: texture pack %s: no image is addressed on the disc\n", from);
        return -1;
    }
    if (!install()) {
        free_entries();
        return -1;
    }
    fprintf(stderr, "memories-pc: texture pack %s: %d images\n", from, entry_count - before);
    return entry_count - before;
}

int TexturePack_Load(const char *from, unsigned rank, int (*part)(const char *, void *), void *context,
                     char *problems, size_t problems_size)
{
    char path[1200], error[256];
    JsonDocument *manifest;
    int result;
    snprintf(path, sizeof(path), "%s/manifest.json", from);
    manifest = Json_ParseFile(path, error, sizeof(error));
    if (!manifest || Json_TypeOf(Json_Root(manifest)) != JSON_ARRAY) {
        if (problems && problems_size)
            snprintf(problems, problems_size, "manifest.json %s", manifest ? "is not an array" : error);
        Json_Free(manifest);
        return -1;
    }
    result = load_entries(from, Json_Root(manifest), 0, NULL, rank, part, context, problems, problems_size);
    Json_Free(manifest);
    return result;
}

int TexturePack_LoadAssets(const char *from, const JsonValue *assets, unsigned rank,
                           int (*part)(const char *, void *), void *context, char *problems, size_t size)
{
    if (Json_TypeOf(assets) != JSON_OBJECT) {
        if (problems && size) snprintf(problems, size, "assets is not an object");
        return -1;
    }
    return load_entries(from, assets, 1, NULL, rank, part, context, problems, size);
}

int TexturePack_LoadAssetFolder(const char *directory, unsigned rank, int (*part)(const char *, void *),
                                void *context, char *problems, size_t size)
{
    Folder folder = {0};
    int result;
    folder_walk(&folder, directory, "", 0, part, context);
    folder_sort(&folder);
    result = load_entries(directory, NULL, 1, &folder, rank, part, context, problems, size);
    folder_free(&folder);
    return result;
}

/* Between frames, on the main thread with the clock held: the disc's
 * directory and the images an upload asked for, then the words they cover. */
void TexturePack_Service(void)
{
    sigset_t held, previous;
    int i, painted = 0, everywhere = 0;
    if (!entries || (!wanted_resolve && !wanted_images && !wanted_rediscover)) return;
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    sigprocmask(SIG_BLOCK, &held, &previous);
    if (wanted_resolve) {
        wanted_resolve = 0;
        int got = resolve();
        if (got > 0) everywhere = 1;          /* uploads since the load were skipped */
        else if (got < 0) wanted_resolve = 1; /* no disc yet: again next frame */
    }
    if (wanted_rediscover && resolved) {
        wanted_rediscover = 0;
        rediscover();
        everywhere = 1;
    }
    if (wanted_images && resolved) {
        wanted_images = 0;
        /* Another reading of words already painted (prepare) is sampled
         * from its image alone: only the uploads that waited are painted. */
        for (i = 0; i < entry_count; i++) {
            if (entries[i].wanted && load_pixels(&entries[i])) painted = 1;
        }
    }
    if (everywhere || (painted && waiting_count > WAITING_AREAS)) {
        waiting_count = 0;
        paint(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    } else if (painted) {
        /* paint notes again what is still missing, so the list is read first. */
        int count = waiting_count, k;
        Area areas[WAITING_AREAS];
        memcpy(areas, waiting, sizeof(areas));
        waiting_count = 0;
        for (k = 0; k < count; k++) paint(areas[k].x, areas[k].y, areas[k].w, areas[k].h);
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void TexturePack_Unload(void)
{
    if (!entries) return;
    TextureDump_Paint = NULL;
    TextureDump_Prepare = NULL;
    TextureDump_Sample = NULL;
    TextureDump_Forget = NULL;
    TextureDump_Follow = NULL;
    TextureDump_Recall = NULL;
    TextureDump_Restored = NULL;
    if (TextureDump_Shadow) {
        memset(TextureDump_Shadow, 0, (size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Shadow));
    }
    if (entry_of) memset(entry_of, 0, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * sizeof(*entry_of));
    free_entries();
    resolved = 0;
    wanted_resolve = wanted_images = wanted_rediscover = 0;
    waiting_count = 0;
    generation++;
    map_generation++;
    /* The images the port made stay: they are no pack's. */
    if (made_count && (!add_made(0) || !install())) free_entries();
}

int TexturePack_EntryFor(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    if (!entries || !entry_of || !TextureDump_Tags || !prepare(page_x, page_y, depth, clut_x, clut_y, u, v)) return 0;
    return chosen + 1;
}

int TexturePack_EntryHead(int entry)
{
    if (entry < 1 || entry > entry_count) return 0;
    return head_of(entry - 1) + 1;
}

int TexturePack_EntryImage(int entry, const unsigned char **rgba, int *width, int *height, int *crop_left,
                           int *crop_width, int *rows, int *texels_per_word)
{
    const Entry *at;
    if (entry < 0 && -entry <= bank_sprite_count) {
        const BankSprite *sprite = &bank_sprites[-entry - 1];
        if (!sprite->image) return 0;
        *rgba = sprite->image; *width = sprite->image_width; *height = sprite->image_height;
        *crop_left = 0; *crop_width = sprite->w; *rows = sprite->h; *texels_per_word = 1;
        return 1;
    }
    if (entry < 1 || entry > entry_count || !entries[entry - 1].image) return 0;
    at = &entries[entry - 1];
    *rgba = at->image;
    *width = at->image_width;
    *height = at->image_height;
    *crop_left = at->crop_left;
    *crop_width = at->crop_width;
    *rows = at->rows;
    *texels_per_word = per_word(at->bpp);
    return 1;
}

unsigned TexturePack_Generation(void) { return generation; }
unsigned TexturePack_MapGeneration(void) { return map_generation; }
const uint16_t *TexturePack_EntryMap(void) { return entry_of; }
const uint32_t *TexturePack_PlaceMap(void) { return place_of; }
