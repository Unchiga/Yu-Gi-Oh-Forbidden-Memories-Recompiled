#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
#include "pc/guest/translated_runtime.h"
#endif
#include "soft_gpu.h"
#include "texture_dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vertex {
    int x, y, r, g, b, u, v;
} Vertex;

static uint16_t vram[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
typedef struct GpuState {
    int clip_x1, clip_y1, clip_x2, clip_y2; /* inclusive */
    int offset_x, offset_y;
    int page_x, page_y, blend, depth, dither;
    int window_mask_x, window_mask_y, window_x, window_y;
    int mask_set, mask_check;
    int clut_x, clut_y;
} GpuState;
static GpuState gpu;
/* This primitive may sample the texture pack's shadow. Not in `gpu`: that
 * struct is a save state's, and its layout is fixed. */
static int shadow_on;

/* Where texels and palettes are read: VRAM, or one of the banks below. Set by
 * every texture-page word, so it is never stale and never part of a state. */
static uint16_t *texture_source;
static int texture_bank;

static uint16_t *banks[SOFT_GPU_BANKS];

/* How far the polygon being drawn fades into what is under it, 0 (not at
 * all) to 255 (SoftGpu_FadeWord, soft_gpu.h). */
static int fade;

/* Where plot writes: VRAM, or a widescreen target while a primitive is drawn
 * a second time into it. Dithering keeps VRAM's phase in a shifted target. */
static uint16_t *target = vram;
static int dither_shift;

/* Widescreen targets. A full-screen drawing area (the game's double
 * buffers) gets a VRAM-shaped companion in which the area is `margin` wider
 * on each side: every primitive drawn to the area is drawn again there,
 * shifted right by the margin and clipped to the wider box, so the geometry
 * the GTE projects left and right of the 4:3 frame, which the hardware clips,
 * has somewhere to land. VRAM itself is never widened, so nothing the game
 * reads back changes.
 * Fills, loads and copies into the area are mirrored into its centre. While
 * the recorder draws a scaled picture it draws the widened one too, and that
 * is what the window shows: the primitives are then not drawn again here
 * (wide_rastered), which halves the time widescreen spends drawing. */
#define WIDE_TARGETS 4
typedef struct WideTarget {
    int x1, y1, x2, y2, margin;
    int drawn;        /* primitives since this target was last presented */
    unsigned stamp;   /* last use, for reuse of the oldest slot */
    uint16_t *pixels;
    uint32_t *picture; /* scaled companion, also used with the GL recorder */
} WideTarget;
static WideTarget wide[WIDE_TARGETS];
static int wide_on;
static unsigned wide_clock;
/* The scaled picture (SoftGpu_SetScale): scale x scale pixels per word. */
static uint32_t *picture;
static uint32_t *wide_picture; /* only the widened primitive pass writes here */
static int scale = 1, scale_shift; /* scale is 1, 2, 4 or 8: the picture wraps with masks and divides with shifts */
static const SoftGpuRecorder *recorder; /* draws the picture instead, from a record (soft_gpu.h) */
static void wide_drop(void);
/* Whether the widescreen targets get the primitives (see above). */
static int wide_rastered(void) { return !(recorder && scale > 1); }
#define PICTURE_WIDTH (SOFT_GPU_WIDTH << scale_shift)
#define PICTURE_HEIGHT (SOFT_GPU_HEIGHT << scale_shift)
static inline __attribute__((always_inline)) uint16_t *pixel(int x, int y);
static int owns(const Vertex *a, const Vertex *b);

uint16_t *SoftGpu_Bank(int bank)
{
    if (bank <= 0 || bank >= SOFT_GPU_BANKS) {
        return NULL;
    }
    if (!banks[bank]) {
        banks[bank] = calloc(SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(uint16_t));
    }
    return banks[bank];
}

const uint16_t *SoftGpu_BankPixels(int bank)
{
    return bank > 0 && bank < SOFT_GPU_BANKS ? banks[bank] : NULL;
}

static inline __attribute__((always_inline)) uint32_t expand(uint16_t c)
{
    uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static inline __attribute__((always_inline)) uint32_t *picture_pixel(int hx, int hy)
{
    return &picture[((size_t)(hy & (PICTURE_HEIGHT - 1)) << (10 + scale_shift)) + (size_t)(hx & (PICTURE_WIDTH - 1))];
}

/* The words x..x+w-1, y..y+h-1 of VRAM, copied into the picture. */
static void picture_from_words(int x, int y, int w, int h)
{
    int i, j, sx, sy;
    if (!picture) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint32_t color = expand(vram[((y + j) & 511) * SOFT_GPU_WIDTH + ((x + i) & 1023)]);
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) *picture_pixel((x + i) * scale + sx, (y + j) * scale + sy) = color;
            }
        }
    }
}

static const int8_t dither_matrix[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

void (*SoftGpu_SettleObserver)(int x, int y, int w, int h);

/* --- The deferred raster (soft_gpu.h, SoftGpu_SetDeferred) ------------
 * A primitive drawn into a full-screen drawing area (the game's two
 * buffers) can be kept back: its words are copied, with the drawing state
 * it is rastered with, into its area's queue, and the queue is rastered, in
 * the order it came, by the first settle that crosses the area, or dropped
 * whole by a fill that covers the area (the game clears the buffer it is
 * about to draw, two frames after it drew into it). What a kept-back
 * primitive does as it comes is still done as it comes: the state it
 * leaves (its palette, page, blend and depth), the texture dump and the
 * pack's prepare, the recorder's copy of the batch. Its pixels, and nothing
 * else, wait. That holds only while these hold, each checked as the
 * primitive comes (else it is rastered at once, after what it follows has
 * been rastered):
 *
 * - it samples VRAM: not a texture bank (whose writers rewrite it unseen),
 *   not the texture pack's shadow (painted between frames);
 * - the texels its coordinates span (texture_rects) and its palette lie
 *   outside every held area, its own included, so nothing it reads is
 *   waiting; and once held, any write into what it reads (a load, a move, a
 *   fill, a primitive rastered at once) rasters it first;
 * - the provenance tags and shadow cells it would clear are already clear
 *   (TextureDump_Clean), so not clearing them later changes nothing;
 * - it is not the life-point panel the opponent's name is drawn over (the
 *   name comes from the host as the panel is drawn);
 * - nothing else reads or writes VRAM behind the software GPU's back: no
 *   applied code mod binds SoftGpu_Vram or SoftGpu_StateData (mods.c);
 * - VRAM is the only picture: no scaled picture and no recorder drawing one
 *   (whose copy of VRAM would miss what is held), no widescreen target
 *   rastered beside the area.
 *
 * Held areas never overlap, and none samples another: before an area holds
 * its first primitive, the areas that meet it or sample it are rastered.
 * So each area's queue can be rastered on its own.
 *
 * The game clock's tick (MEMORIES_CLOCK=interrupt) can load, store, move or
 * fill at any instruction. It is held off (SoftGpu_HoldClock) through each
 * batch while work can be held (holding()) or is, through every look at the
 * queues while anything is held and through every change of given_up[], so
 * it never sees a queue or given_up[] half made or half rastered, and never
 * finds `gpu` swapped for a held primitive's state. At 2x and above, with a
 * recorder or with a mod's VRAM, a batch neither holds it nor calls hold. */
#define HELD_AREAS SOFT_GPU_QUEUES
#define HELD_SAMPLED 48
#define HELD_WORDS_MAX (4u << 20) /* past this an area's queue is rastered and started again */
typedef struct HeldEntry {
    uint32_t at, count, state;
} HeldEntry;
typedef struct HeldArea {
    int on, x, y, w, h;            /* the drawing area */
    unsigned stamp;
    uint32_t *words;
    size_t word_count, word_room;
    HeldEntry *entries;
    size_t count, room;
    GpuState *states;
    size_t state_count, state_room;
    int sampled[HELD_SAMPLED][4];  /* x, y, w, h: the texels and palette rows its work reads */
    int sampled_count, sampled_all;
    uint16_t *before;              /* poison: the area's words as the queue began */
} HeldArea;
static HeldArea held[HELD_AREAS];
static volatile int held_any;
static int deferred_on, deferred_poison, mod_vram_users, replaying, in_batch;
static unsigned held_clock;
/* The provenance tags of each area were found clear at this count of
 * TextureDump_Touched (clean_tags: the tags it was for). */
static unsigned clean_touches[HELD_AREAS];
static const uint32_t *clean_tags[HELD_AREAS];
static SoftGpuDeferredStats deferred_stats;
/* Drawing areas a primitive was rastered at once into since their last
 * clear: they hold nothing until it (with a texture pack or a mod's bank,
 * most of a frame cannot be held, and holding the rest only to raster it at
 * the next such primitive costs the time it would save). One that met a
 * bank, the shadow or the panel since its last clear holds nothing after
 * the next clear either (`again`): with the pack a duel frame always meets
 * one, and holding its first primitives only to raster them at it cost more
 * than not holding. A cycle that meets none lets it hold again after it.
 * `drawn`: a primitive came into it since the last clear (a clear right
 * after a clear, as the game's, ends no cycle). */
typedef struct GivenUp {
    int x, y, w, h, again, drawn;
} GivenUp;
static GivenUp given_up[HELD_AREAS];
static int given_up_count;
#define POISON 0x7c1f /* magenta: what a reader that does not settle sees of held work */

void (*SoftGpu_HoldClock)(int hold);
unsigned long long (*SoftGpu_Clock)(void);
static int clock_depth;

static void hold_clock(void)
{
    if (clock_depth++ == 0 && SoftGpu_HoldClock) SoftGpu_HoldClock(1);
}

static void release_clock(void)
{
    if (--clock_depth == 0 && SoftGpu_HoldClock) SoftGpu_HoldClock(0);
}

/* given_up[] is changed with the clock held: a fill from the tick
 * (before_fill) changes it too, and one in the middle of a removal or an
 * addition would leave a count of -1 or 3. */
static GivenUp *area_given_up(int x, int y, int w, int h)
{
    for (int i = 0; i < given_up_count; i++)
        if (given_up[i].x == x && given_up[i].y == y && given_up[i].w == w && given_up[i].h == h) return &given_up[i];
    return NULL;
}

/* Whether the area is given up; if it is, it was drawn into this cycle. */
static int drawn_given_up(int x, int y, int w, int h)
{
    GivenUp *gone;
    hold_clock();
    if ((gone = area_given_up(x, y, w, h)) != NULL) gone->drawn = 1;
    release_clock();
    return gone != NULL;
}

/* `again`: a bank, the shadow or the panel made it (they come back each
 * frame), so the next cycle starts given up too. */
static void give_up(int x, int y, int w, int h, int again)
{
    GivenUp *gone;
    hold_clock();
    gone = area_given_up(x, y, w, h);
    if (!gone) {
        if (given_up_count == HELD_AREAS) memmove(&given_up[0], &given_up[1], sizeof(given_up[0]) * (HELD_AREAS - 1));
        else given_up_count++;
        gone = &given_up[given_up_count - 1];
        gone->x = x;
        gone->y = y;
        gone->w = w;
        gone->h = h;
        gone->again = 0;
    }
    gone->drawn = 1;
    gone->again |= again;
    release_clock();
}

/* Every area may hold again from its next primitive: when the switch
 * changes, at a reset, a state load, a new scale or recorder, a mod's VRAM.
 * Only what holding costs depends on given_up[], never what is drawn. */
static void forget_given_up(void)
{
    if (!given_up_count) return;
    hold_clock();
    given_up_count = 0;
    release_clock();
}

/* Whether [a, a + la) and [b, b + lb) meet on a circle of `size`. */
static int spans_meet(int a, int la, int b, int lb, int size)
{
    int pa[2][2], pb[2][2], na = 1, nb = 1, i, j;
    if (la <= 0 || lb <= 0) return 0;
    if (la >= size || lb >= size) return 1;
    a &= size - 1;
    b &= size - 1;
    pa[0][0] = a;
    pa[0][1] = a + la;
    pb[0][0] = b;
    pb[0][1] = b + lb;
    if (a + la > size) {
        pa[0][1] = size;
        pa[1][0] = 0;
        pa[1][1] = a + la - size;
        na = 2;
    }
    if (b + lb > size) {
        pb[0][1] = size;
        pb[1][0] = 0;
        pb[1][1] = b + lb - size;
        nb = 2;
    }
    for (i = 0; i < na; i++)
        for (j = 0; j < nb; j++)
            if (pa[i][0] < pb[j][1] && pb[j][0] < pa[i][1]) return 1;
    return 0;
}

/* Rectangles of VRAM words, wrapping as transfers do. */
static int rects_meet(int x, int y, int w, int h, int x2, int y2, int w2, int h2)
{
    return spans_meet(x, w, x2, w2, SOFT_GPU_WIDTH) && spans_meet(y, h, y2, h2, SOFT_GPU_HEIGHT);
}

/* Whether [a, a + la) on a circle of `size` covers lo..hi (inclusive, not
 * wrapping): within the part before the wrap, or within the part after. */
static int span_covers(int a, int la, int size, int lo, int hi)
{
    if (la >= size) return 1;
    if (la <= 0) return 0;
    a &= size - 1;
    if (a + la <= size) return a <= lo && hi < a + la;
    return a <= lo || hi < a + la - size;
}

static int area_meets(const HeldArea *a, int x, int y, int w, int h)
{
    return a->on && rects_meet(a->x, a->y, a->w, a->h, x, y, w, h);
}

static int area_samples(const HeldArea *a, int x, int y, int w, int h)
{
    if (!a->on) return 0;
    if (a->sampled_all) return 1;
    for (int i = 0; i < a->sampled_count; i++)
        if (rects_meet(a->sampled[i][0], a->sampled[i][1], a->sampled[i][2], a->sampled[i][3], x, y, w, h)) return 1;
    return 0;
}

static void held_recount(void)
{
    int any = 0;
    for (int a = 0; a < HELD_AREAS; a++) any |= held[a].on;
    held_any = any;
}

static void held_reset(HeldArea *a)
{
    a->on = 0;
    a->word_count = a->count = a->state_count = 0;
    a->sampled_count = a->sampled_all = 0;
    held_recount();
}

static size_t polygon(const uint32_t *words, size_t count);
static size_t lines(const uint32_t *words, size_t count);
static size_t rectangle(const uint32_t *words, size_t count);

/* The area's queue, rastered as it would have been as it came. */
static void settle_area(HeldArea *a, int why)
{
    GpuState live;
    uint16_t *live_source = texture_source, *live_target = target;
    uint32_t *live_wide = wide_picture;
    int live_bank = texture_bank, live_shadow = shadow_on, live_fade = fade, live_shift = dither_shift;
    unsigned long long started;
    size_t i;
    if (!a->on || replaying) return;
    hold_clock();
    live = gpu;
    started = SoftGpu_Clock ? SoftGpu_Clock() : 0;
    if (a->before) { /* poison: the words it was drawn over */
        for (int j = 0; j < a->h; j++)
            memcpy(vram + (a->y + j) * SOFT_GPU_WIDTH + a->x, a->before + (size_t)j * a->w, (size_t)a->w * 2);
    }
    replaying = 1;
    for (i = 0; i < a->count; i++) {
        const HeldEntry *e = &a->entries[i];
        const uint32_t *words = a->words + e->at;
        uint32_t command = words[0] >> 24;
        gpu = a->states[e->state];
        texture_source = vram;
        texture_bank = 0;
        shadow_on = 0;
        fade = 0;
        target = vram;
        dither_shift = 0;
        wide_picture = NULL;
        (command < 0x40 ? polygon : command < 0x60 ? lines : rectangle)(words, e->count);
    }
    replaying = 0;
    gpu = live;
    texture_source = live_source;
    texture_bank = live_bank;
    shadow_on = live_shadow;
    fade = live_fade;
    target = live_target;
    dither_shift = live_shift;
    wide_picture = live_wide;
    deferred_stats.rastered += a->count;
    deferred_stats.settled++;
    deferred_stats.why[why]++;
    if (SoftGpu_Clock) {
        unsigned long long took = SoftGpu_Clock() - started;
        deferred_stats.settle_us += took;
        if (!in_batch) deferred_stats.settle_outside_us += took;
    }
    held_reset(a);
    release_clock();
}

static void drop_area(HeldArea *a)
{
    deferred_stats.dropped += a->count;
    deferred_stats.cleared++;
    held_reset(a);
}

static void settle_all(int why)
{
    if (!held_any) return;
    hold_clock();
    for (int a = 0; a < HELD_AREAS; a++) settle_area(&held[a], why);
    release_clock();
}

/* Before x,y,w,h is read: what is held there is drawn. */
static void before_read(int x, int y, int w, int h, int why)
{
    if (!held_any) return;
    hold_clock();
    for (int a = 0; a < HELD_AREAS; a++)
        if (area_meets(&held[a], x, y, w, h)) settle_area(&held[a], why);
    release_clock();
}

/* A transfer about to write x,y,w,h: a load's pixels, a move's source or
 * a fill's colour. */
typedef struct Write {
    const uint16_t *pixels; /* a load */
    int sx, sy, move;       /* a move */
    int fill;               /* a fill, of `color` */
    uint16_t color;
} Write;

/* Whether the transfer leaves every word of x,y,w,h as it is: the game
 * uploads the same texels and palettes again frame after frame, and a
 * write that changes nothing changes nothing a held primitive reads. */
static int write_same(const Write *write, int x, int y, int w, int h)
{
    int set = gpu.mask_set ? 0x8000 : 0;
    if (!write) return 0;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint16_t now = vram[((y + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((x + i) & (SOFT_GPU_WIDTH - 1))], next;
            if (write->fill) {
                next = write->color; /* a fill sets no mask bit and checks none */
            } else {
                if (gpu.mask_check && (now & 0x8000)) continue; /* kept */
                next = (uint16_t)((write->move ? vram[((write->sy + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH +
                                                      ((write->sx + i) & (SOFT_GPU_WIDTH - 1))]
                                               : write->pixels[j * w + i]) | set);
            }
            if (next != now) return 0;
        }
    }
    return 1;
}

/* Before x,y,w,h is written: what is held there is drawn, and what is held
 * anywhere that reads it, unless `write` (NULL: a primitive) leaves every
 * word as it was. */
static void before_write(int x, int y, int w, int h, int why, const Write *write)
{
    int same = -1;
    if (!held_any) return;
    hold_clock();
    for (int a = 0; a < HELD_AREAS; a++)
        if (area_meets(&held[a], x, y, w, h)) settle_area(&held[a], why);
    for (int a = 0; a < HELD_AREAS; a++) {
        if (!area_samples(&held[a], x, y, w, h)) continue;
        if (same < 0) same = write_same(write, x, y, w, h);
        if (!same) settle_area(&held[a], why);
    }
    release_clock();
}

/* A fill: an area it covers whole loses what it holds, which the fill
 * would overwrite word for word (a fill does not check the mask). */
static void before_fill(int x, int y, int w, int h, uint16_t color)
{
    const Write fill = {NULL, 0, 0, 0, 1, color};
    if (given_up_count) {
        /* With the clock held: a fill from the tick in the middle of this
         * would remove an entry twice. Nothing given up, nothing to hold. */
        hold_clock();
        for (int i = 0; i < given_up_count; i++) { /* a clear: the area may hold again */
            GivenUp *gone = &given_up[i];
            if (!span_covers(x, w, SOFT_GPU_WIDTH, gone->x, gone->x + gone->w - 1) ||
                !span_covers(y, h, SOFT_GPU_HEIGHT, gone->y, gone->y + gone->h - 1) || !gone->drawn)
                continue;
            if (gone->again) { /* a bank, the shadow or the panel will be back: not yet */
                gone->again = gone->drawn = 0;
                continue;
            }
            memmove(&given_up[i], &given_up[i + 1], sizeof(given_up[0]) * (size_t)(given_up_count - i - 1));
            given_up_count--;
            i--;
        }
        release_clock();
    }
    if (!held_any) return;
    hold_clock();
    for (int a = 0; a < HELD_AREAS; a++) {
        HeldArea *area = &held[a];
        if (area->on && span_covers(x, w, SOFT_GPU_WIDTH, area->x, area->x + area->w - 1) &&
            span_covers(y, h, SOFT_GPU_HEIGHT, area->y, area->y + area->h - 1))
            drop_area(area);
    }
    before_write(x, y, w, h, SOFT_GPU_WHY_WRITE, &fill);
    release_clock();
}

/* Whether held work may be added now (the rules above that do not depend
 * on the primitive). */
static int holding(void)
{
    return deferred_on && !mod_vram_users && !picture && !(recorder && scale > 1);
}

/* The words of the texture the primitive reads (texture_rects, as
 * rectangles): the words of its page that hold the texels its coordinates
 * span and, below 16 bits, its palette row. `uv`: the least and greatest u and v
 * it reads (polygon_uv; for a sprite u, v to u + w - 1, v + h - 1, which
 * past 255 wraps around the page); all of the page on an axis the texture
 * window folds or that wraps. Drawing areas end where textures begin (the
 * buffers' last row is 239, the game's strips start at 240), so not a
 * texel more. */
static void texel_rect(int rect[4], const int uv[4])
{
    int shift = 2 - gpu.depth, u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    if (gpu.window_mask_x || uv[2] > 255) u0 = 0, u1 = 255;
    if (gpu.window_mask_y || uv[3] > 255) v0 = 0, v1 = 255;
    rect[0] = gpu.page_x + (u0 >> shift);
    rect[1] = gpu.page_y + v0;
    rect[2] = (u1 >> shift) - (u0 >> shift) + 1;
    rect[3] = v1 - v0 + 1;
}

/* texel_rect's words and, below 16 bits, the palette row's (counted only
 * as far as the texels' indices reach past VRAM's edge, below). Returns
 * how many rectangles. */
static int texture_rects(int rects[2][4], const int uv[4])
{
    texel_rect(rects[0], uv);
    if (gpu.depth >= 2) return 1;
    rects[1][0] = gpu.clut_x;
    rects[1][1] = gpu.clut_y;
    rects[1][2] = gpu.depth == 0 ? 16 : 256;
    rects[1][3] = 1;
    if (gpu.clut_x + rects[1][2] > SOFT_GPU_WIDTH) {
        /* An 8-bit palette from x 768 on runs past VRAM's edge, into the
         * first buffer's columns, for indices the texels never hold (the
         * game's palettes at 896 have 128 colours): only up to the greatest
         * index the texels it reads hold (a 4-bit one, 16 from a multiple
         * of 16, never wraps). */
        int greatest = 0;
        for (int j = 0; j < rects[0][3]; j++)
            for (int i = 0; i < rects[0][2]; i++) {
                uint16_t word = vram[((rects[0][1] + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH +
                                     ((rects[0][0] + i) & (SOFT_GPU_WIDTH - 1))];
                int high = word >> 8, low = word & 0xff;
                greatest = high > greatest ? high : low > greatest ? low : greatest;
            }
        rects[1][2] = greatest + 1;
    }
    return 2;
}

/* Whether the tags and shadow cells under the area are all clear (and so
 * stay what drawing would leave them). */
static int area_clean(int slot, int x, int y, int w, int h)
{
    unsigned touches;
    if (!TextureDump_Tags) return 1;
    touches = TextureDump_TouchCount();
    if (slot >= 0 && clean_tags[slot] == TextureDump_Tags && clean_touches[slot] == touches) return 1;
    for (int j = y; j < y + h; j++) {
        const uint8_t *clean = TextureDump_Clean + (j & (SOFT_GPU_HEIGHT - 1)) * TEXTURE_CLEAN_COLUMNS;
        for (int b = x / TEXTURE_CLEAN_WORDS; b <= (x + w - 1) / TEXTURE_CLEAN_WORDS; b++)
            if (!clean[b % TEXTURE_CLEAN_COLUMNS]) return 0;
    }
    if (slot >= 0) {
        clean_tags[slot] = TextureDump_Tags;
        clean_touches[slot] = touches;
    }
    return 1;
}

static int grow(void **items, size_t *room, size_t need, size_t size)
{
    void *more;
    size_t wanted = *room ? *room : 256;
    if (need <= *room) return 1;
    while (wanted < need) wanted *= 2;
    if (!(more = realloc(*items, wanted * size))) return 0;
    *items = more;
    *room = wanted;
    return 1;
}

/* The union of two sampled rectangles into `out`, and how much larger it is
 * than the two: -1 if either wraps, or the union reaches into a drawing area
 * held now or before (the buffers: a rectangle there would tie them). */
static long long sampled_union(const int a[4], const int b[4], int out[4])
{
    int k;
    if (a[0] + a[2] > SOFT_GPU_WIDTH || a[1] + a[3] > SOFT_GPU_HEIGHT || b[0] + b[2] > SOFT_GPU_WIDTH ||
        b[1] + b[3] > SOFT_GPU_HEIGHT)
        return -1;
    out[0] = a[0] < b[0] ? a[0] : b[0];
    out[1] = a[1] < b[1] ? a[1] : b[1];
    out[2] = (a[0] + a[2] > b[0] + b[2] ? a[0] + a[2] : b[0] + b[2]) - out[0];
    out[3] = (a[1] + a[3] > b[1] + b[3] ? a[1] + a[3] : b[1] + b[3]) - out[1];
    for (k = 0; k < HELD_AREAS; k++)
        if (rects_meet(out[0], out[1], out[2], out[3], held[k].x, held[k].y, held[k].w, held[k].h)) return -1;
    return (long long)out[2] * out[3] - (long long)a[2] * a[3] - (long long)b[2] * b[3];
}

/* What a held primitive samples, added to its area's rectangles: kept as
 * it is while there is room; else merged into the one it grows least, or,
 * if every such union reaches into a buffer, the two that grow least
 * merged to make room (a write in a merged rectangle then rasters the area
 * when it need not, never the other way); else any write at all rasters
 * the area. */
static void add_sampled(HeldArea *area, const int rect[4])
{
    int k, j, best = -1, pair = -1, joined[4];
    long long growth = 0, more;
    for (k = 0; k < area->sampled_count; k++) {
        const int *s = area->sampled[k];
        if (s[0] <= rect[0] && s[1] <= rect[1] && rect[0] + rect[2] <= s[0] + s[2] && rect[1] + rect[3] <= s[1] + s[3])
            return; /* within one already (rectangles that wrap are only ever equal) */
    }
    if (area->sampled_count < HELD_SAMPLED) {
        memcpy(area->sampled[area->sampled_count++], rect, 4 * sizeof(int));
        return;
    }
    for (k = 0; k < area->sampled_count; k++) {
        more = sampled_union(area->sampled[k], rect, joined);
        if (more >= 0 && (best < 0 || more < growth)) best = k, growth = more;
    }
    if (best >= 0) {
        sampled_union(area->sampled[best], rect, joined);
        memcpy(area->sampled[best], joined, sizeof(joined));
        return;
    }
    for (k = 0; k < area->sampled_count; k++)
        for (j = k + 1; j < area->sampled_count; j++) {
            more = sampled_union(area->sampled[k], area->sampled[j], joined);
            if (more >= 0 && (best < 0 || more < growth)) best = k, pair = j, growth = more;
        }
    if (best < 0) {
        area->sampled_all = 1; /* any write at all rasters it */
        return;
    }
    sampled_union(area->sampled[best], area->sampled[pair], joined);
    memcpy(area->sampled[best], joined, sizeof(joined));
    memcpy(area->sampled[pair], rect, 4 * sizeof(int));
}

/* The held area for the current drawing area, its queue begun if it was
 * not: NULL if it cannot be (no memory). */
static HeldArea *held_area(void)
{
    int x = gpu.clip_x1, y = gpu.clip_y1, w = gpu.clip_x2 - gpu.clip_x1 + 1, h = gpu.clip_y2 - gpu.clip_y1 + 1;
    int a, oldest = -1, slot = -1;
    HeldArea *area;
    for (a = 0; a < HELD_AREAS; a++)
        if (held[a].on && held[a].x == x && held[a].y == y && held[a].w == w && held[a].h == h) return &held[a];
    /* Holding here writes here: what meets the area, or reads it, first. */
    before_write(x, y, w, h, SOFT_GPU_WHY_AREA, NULL);
    for (a = 0; a < HELD_AREAS; a++) {
        if (!held[a].on && slot < 0) slot = a;
        if (held[a].on && (oldest < 0 || held[a].stamp < held[oldest].stamp)) oldest = a;
    }
    if (slot < 0) {
        settle_area(&held[oldest], SOFT_GPU_WHY_AREA);
        slot = oldest;
    }
    area = &held[slot];
    if (deferred_poison) {
        uint16_t *copy = realloc(area->before, (size_t)w * h * sizeof(uint16_t));
        if (!copy) return NULL;
        area->before = copy;
        for (int j = 0; j < h; j++) {
            uint16_t *row = vram + (y + j) * SOFT_GPU_WIDTH + x;
            memcpy(area->before + (size_t)j * w, row, (size_t)w * 2);
            for (int i = 0; i < w; i++) row[i] = POISON;
        }
    }
    if (area->x != x || area->y != y || area->w != w || area->h != h) clean_tags[slot] = NULL;
    area->x = x;
    area->y = y;
    area->w = w;
    area->h = h;
    area->on = 1;
    area->stamp = ++held_clock;
    held_recount();
    return area;
}

/* Called by a primitive as it comes, once what it does as it comes is done
 * and before it rasters: 1 when it is held (it is not rastered now), 0 when
 * it is to be rastered now, what it follows having been rastered first.
 * `uv`: NULL, or the texture coordinates it spans (texture_rects) in the
 * state's page and palette; a textured polygon passes NULL and its
 * vertices instead (`v`, `vertices`), whose span (polygon_uv) is taken only
 * if a rule needs it; `panel`: it is the life-point panel the opponent's name goes
 * over. */
static void polygon_uv(const Vertex *v, int vertices, int uv[4]);
static int hold(const uint32_t *words, size_t count, const int *uv, const Vertex *v, int vertices, int panel)
{
    int x = gpu.clip_x1, y = gpu.clip_y1, w = gpu.clip_x2 - gpu.clip_x1 + 1, h = gpu.clip_y2 - gpu.clip_y1 + 1;
    int textured = uv != NULL || v != NULL, rects[2][4], n = -1, now = -1, i, slot = -1, span[4];
    HeldArea *area;
    if (replaying || target != vram || (!holding() && !held_any)) return 0;
    /* The rules that need no rectangles first: what goes at once anyway
     * (a bank, the shadow, an area given up) never scans a palette. */
    if (w < 256 || h < 192 || x + w > SOFT_GPU_WIDTH || y + h > SOFT_GPU_HEIGHT) {
        now = SOFT_GPU_NOW_COUNT; /* not a full screen: not counted */
    } else if (!holding() || SoftGpu_WideMargin(gpu.clip_x1, gpu.clip_y1, gpu.clip_x2, gpu.clip_y2)) {
        now = SOFT_GPU_NOW_OTHER;
    } else if (textured && texture_source != vram) {
        now = SOFT_GPU_NOW_BANK;
    } else if (textured && shadow_on == 1) {
        now = SOFT_GPU_NOW_SHADOW;
    } else if (panel) {
        now = SOFT_GPU_NOW_PANEL;
    } else if (given_up_count && drawn_given_up(x, y, w, h)) {
        now = SOFT_GPU_NOW_AFTER;
    } else {
        if (textured && !uv) polygon_uv(v, vertices, span), uv = span;
        n = textured ? texture_rects(rects, uv) : 0;
        for (int a = 0; a < HELD_AREAS; a++)
            if (held[a].on && held[a].x == x && held[a].y == y && held[a].w == w && held[a].h == h) slot = a;
        for (i = 0; i < n; i++) {
            if (rects_meet(rects[i][0], rects[i][1], rects[i][2], rects[i][3], x, y, w, h)) now = SOFT_GPU_NOW_TEXTURE;
            for (int a = 0; a < HELD_AREAS; a++)
                if (area_meets(&held[a], rects[i][0], rects[i][1], rects[i][2], rects[i][3]))
                    now = SOFT_GPU_NOW_TEXTURE;
        }
        if (now < 0 && !area_clean(slot, x, y, w, h)) now = SOFT_GPU_NOW_TAGS;
    }
    if (now < 0 && count && count <= HELD_WORDS_MAX) {
        hold_clock();
        area = held_area();
        if (area && area->word_count + count > HELD_WORDS_MAX) {
            settle_area(area, SOFT_GPU_WHY_ALL);
            area = held_area();
        }
        if (area && grow((void **)&area->words, &area->word_room, area->word_count + count, sizeof(uint32_t)) &&
            grow((void **)&area->entries, &area->room, area->count + 1, sizeof(HeldEntry)) &&
            grow((void **)&area->states, &area->state_room, area->state_count + 1, sizeof(GpuState))) {
            HeldEntry *e = &area->entries[area->count++];
            if (!area->state_count || memcmp(&area->states[area->state_count - 1], &gpu, sizeof(gpu)))
                area->states[area->state_count++] = gpu;
            e->state = (uint32_t)(area->state_count - 1);
            e->at = (uint32_t)area->word_count;
            e->count = (uint32_t)count;
            memcpy(area->words + area->word_count, words, count * sizeof(uint32_t));
            area->word_count += count;
            for (i = 0; i < n && !area->sampled_all; i++) add_sampled(area, rects[i]);
            deferred_stats.held++;
            release_clock();
            return 1;
        }
        if (area) settle_area(area, SOFT_GPU_WHY_ALL); /* out of memory: as it came */
        release_clock();
        now = SOFT_GPU_NOW_OTHER;
    }
    if (now < SOFT_GPU_NOW_COUNT) {
        deferred_stats.immediate++;
        deferred_stats.now[now < 0 ? SOFT_GPU_NOW_OTHER : now]++;
        if (now >= 0 && now < SOFT_GPU_NOW_OTHER && holding())
            give_up(x, y, w, h, now == SOFT_GPU_NOW_BANK || now == SOFT_GPU_NOW_SHADOW || now == SOFT_GPU_NOW_PANEL);
    }
    if (held_any) {
        /* Rastered now, so after what it draws over and what it reads. */
        hold_clock();
        before_write(x, y, w, h, SOFT_GPU_WHY_DRAW, NULL);
        if (textured && texture_source == vram) {
            /* Its palette whole: the texels that bound it may have been held. */
            if (n < 0) {
                if (!uv) polygon_uv(v, vertices, span), uv = span;
                texel_rect(rects[0], uv);
            }
            before_read(rects[0][0], rects[0][1], rects[0][2], rects[0][3], SOFT_GPU_WHY_DRAW);
            if (gpu.depth < 2) before_read(gpu.clut_x, gpu.clut_y, gpu.depth == 0 ? 16 : 256, 1, SOFT_GPU_WHY_DRAW);
        }
        release_clock();
    }
    return 0;
}

void SoftGpu_SetDeferred(int on, int poison)
{
    settle_all(SOFT_GPU_WHY_ALL); /* nothing is held after it, so nothing below needs the clock held */
    if (on && !deferred_on) memset(&deferred_stats, 0, sizeof(deferred_stats));
    deferred_on = on != 0;
    deferred_poison = on && poison;
    forget_given_up();
    for (int a = 0; a < HELD_AREAS; a++) {
        free(held[a].before);
        held[a].before = NULL;
    }
}

int SoftGpu_Deferred(void) { return deferred_on; }

void SoftGpu_SetModVramUsers(int users)
{
    if (users && !mod_vram_users) {
        settle_all(SOFT_GPU_WHY_ALL);
        forget_given_up();
    }
    mod_vram_users = users;
}

void SoftGpu_DeferredStats(SoftGpuDeferredStats *out)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    out = GuestRuntime_ResolveData(out, sizeof(*out));
#endif
    int took = held_any;
    *out = deferred_stats;
    if (took) hold_clock(); /* the queues as they stand, not half made by the tick */
    for (int a = 0; a < HELD_AREAS; a++) {
        out->queue_x[a] = held[a].on ? held[a].x : -1;
        out->queue_y[a] = held[a].on ? held[a].y : -1;
        out->queued[a] = held[a].on ? held[a].count : 0;
    }
    if (took) release_clock();
}

void SoftGpu_Settle(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (SoftGpu_SettleObserver) SoftGpu_SettleObserver(x, y, w, h);
    if (held_any)
        before_read(x, y, w, h, w >= SOFT_GPU_WIDTH && h >= SOFT_GPU_HEIGHT ? SOFT_GPU_WHY_ALL : SOFT_GPU_WHY_READ);
}

void SoftGpu_SettleTexture(int depth, int page_x, int page_y, int clut_x, int clut_y)
{
    SoftGpu_Settle(page_x, page_y, depth == 0 ? 64 : depth == 1 ? 128 : 256, 256);
    if (depth < 2) SoftGpu_Settle(clut_x, clut_y, depth == 0 ? 16 : 256, 1);
}

const uint16_t *SoftGpu_Vram(void)
{
    SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    return vram;
}

const uint16_t *SoftGpu_VramRaw(void)
{
    return vram;
}

void SoftGpu_StateWords(uint32_t words[6])
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    words = GuestRuntime_ResolveData(words, 6 * sizeof(*words));
#endif
    uint32_t bank = 0, b;
    for (b = 1; b < 16; b++) { /* an E1 word names banks 0-15 only (set_page): 16-31 come back as VRAM */
        if (banks[b] && texture_source == banks[b]) bank = b;
    }
    words[0] = 0xe1000000u | (uint32_t)(gpu.page_x / 64) | ((uint32_t)(gpu.page_y / 256) << 4) |
               ((uint32_t)gpu.blend << 5) | ((uint32_t)gpu.depth << 7) | ((uint32_t)gpu.dither << 9) | (bank << 11);
    words[1] = 0xe2000000u | (uint32_t)gpu.window_mask_x | ((uint32_t)gpu.window_mask_y << 5) |
               ((uint32_t)gpu.window_x << 10) | ((uint32_t)gpu.window_y << 15);
    words[2] = 0xe3000000u | (uint32_t)gpu.clip_x1 | ((uint32_t)gpu.clip_y1 << 10);
    words[3] = 0xe4000000u | (uint32_t)gpu.clip_x2 | ((uint32_t)gpu.clip_y2 << 10);
    words[4] = 0xe5000000u | ((uint32_t)gpu.offset_x & 0x7ff) | (((uint32_t)gpu.offset_y & 0x7ff) << 11);
    words[5] = 0xe6000000u | (uint32_t)gpu.mask_set | ((uint32_t)gpu.mask_check << 1);
}

static void resync_recorder(void)
{
    uint32_t words[6];
    if (!recorder) return;
    /* The recorder takes VRAM whole: both callers (SoftGpu_SetRecorder,
     * SoftGpu_PictureFromVram) have settled it. */
    SoftGpu_StateWords(words);
    recorder->resync(scale, words);
}

void SoftGpu_Reset(void)
{
    settle_all(SOFT_GPU_WHY_ALL); /* drawn with the state it came with, before the tags start again */
    memset(clean_tags, 0, sizeof(clean_tags));
    forget_given_up();
    memset(&gpu, 0, sizeof(gpu));
    texture_source = vram;
    texture_bank = 0;
    TextureDump_Init();
    gpu.clip_x2 = SOFT_GPU_WIDTH - 1;
    gpu.clip_y2 = SOFT_GPU_HEIGHT - 1;
    if (recorder && scale > 1) { /* the state alone: VRAM and the picture stay */
        uint32_t words[6];
        SoftGpu_StateWords(words);
        recorder->gp0(words, 6);
    }
}

static inline __attribute__((always_inline)) uint16_t *pixel(int x, int y)
{
    return &target[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

/* VRAM itself, whatever `target` is. Loads, stores, copies and fills always
 * mean VRAM, and they can arrive from the interrupt tick (a LoadImage in a
 * callback) while a primitive is being drawn a second time into a widescreen
 * target: through pixel() they landed there instead, and VRAM missed the
 * upload (the password screen's letters, differently on every run). */
static inline __attribute__((always_inline)) uint16_t *vram_pixel(int x, int y)
{
    return &vram[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

int SoftGpu_SetScale(int wanted)
{
    uint32_t *made = NULL;
    int rastered;
    if (wanted != 1 && wanted != 2 && wanted != 4 && wanted != 8) return 0;
    if (wanted == scale) return 1;
    /* Before the picture and the scale change: what was kept back is drawn
     * the way it came. */
    SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    rastered = wide_rastered();
    if (wanted > 1 && !recorder) {
        made = calloc((size_t)SOFT_GPU_WIDTH * wanted * SOFT_GPU_HEIGHT * wanted, sizeof(*made));
        if (!made) return 0;
    }
    for (int t = 0; t < WIDE_TARGETS; t++) {
        free(wide[t].picture);
        wide[t].picture = NULL;
    }
    free(picture);
    picture = made;
    scale = wanted;
    scale_shift = wanted == 8 ? 3 : wanted == 4 ? 2 : wanted == 2 ? 1 : 0;
    /* The targets missed the primitives so far: made again from VRAM. */
    if (!rastered && wide_rastered()) wide_drop();
    SoftGpu_PictureFromVram();
    return 1;
}

int SoftGpu_Scale(void) { return scale; }

/* The software picture is drawn with the words: settled with them. */
const uint32_t *SoftGpu_Picture(void)
{
    if (picture) SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    return picture;
}

void SoftGpu_PictureFromVram(void)
{
    SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    forget_given_up(); /* a state load, a new scale: whatever was given up is another frame's */
    if (picture) picture_from_words(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    resync_recorder();
}

void SoftGpu_SetRecorder(const SoftGpuRecorder *wanted)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    if (wanted) wanted = GuestRuntime_ResolveData((void *)wanted, sizeof(*wanted));
#endif
    if (wanted == recorder) return;
    SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT); /* as SetScale */
    forget_given_up();
    recorder = wanted;
    if (recorder) {
        free(picture);
        picture = NULL;
        for (int t = 0; t < WIDE_TARGETS; t++) { /* the recorder draws these too */
            free(wide[t].picture);
            wide[t].picture = NULL;
        }
        resync_recorder();
    } else if (scale > 1) {
        int at_scale = scale;
        wide_drop(); /* they missed the primitives, as in SetScale */
        scale = 1; /* so that SetScale makes the picture again */
        SoftGpu_SetScale(at_scale);
    }
}

/* Seed transferred words in a scaled companion. Primitive rendering then
 * replaces these blocks with samples at the requested internal resolution. */
static void wide_picture_words(WideTarget *wt, int x, int y, int w, int h)
{
    if (!wt->picture) return;
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            uint32_t color = expand(wt->pixels[row * SOFT_GPU_WIDTH + col]);
            for (int sy = 0; sy < scale; sy++) {
                uint32_t *out = wt->picture + (size_t)(row * scale + sy) * PICTURE_WIDTH + col * scale;
                for (int sx = 0; sx < scale; sx++) out[sx] = color;
            }
        }
    }
}

static void wide_picture_prepare(WideTarget *wt)
{
    if (scale <= 1 || wt->picture || recorder) return;
    wt->picture = calloc((size_t)PICTURE_WIDTH * PICTURE_HEIGHT, sizeof(*wt->picture));
    wide_picture_words(wt, wt->x1, wt->y1, wt->x2 - wt->x1 + 1 + 2 * wt->margin,
                       wt->y2 - wt->y1 + 1);
}

/* Copies what VRAM now holds in x,y,w,h into the centre of every
 * widescreen target it overlaps. A fill spanning a target's whole width
 * also fills its sides, as a cleared screen is cleared edge to edge. */
static void wide_mirror(int x, int y, int w, int h, int fill, uint16_t color)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        WideTarget *wt = &wide[t];
        int x1 = x > wt->x1 ? x : wt->x1, x2 = x + w - 1 < wt->x2 ? x + w - 1 : wt->x2;
        int y1 = y > wt->y1 ? y : wt->y1, y2 = y + h - 1 < wt->y2 ? y + h - 1 : wt->y2;
        int row, column;
        if (!wt->pixels || x1 > x2 || y1 > y2) {
            continue;
        }
        for (row = y1; row <= y2; row++) {
            uint16_t *out = wt->pixels + row * SOFT_GPU_WIDTH;
            memcpy(out + x1 + wt->margin, vram + row * SOFT_GPU_WIDTH + x1, (size_t)(x2 - x1 + 1) * 2);
            if (fill && x <= wt->x1 && x + w - 1 >= wt->x2) {
                for (column = 0; column < wt->margin; column++) {
                    out[wt->x1 + column] = color;
                    out[wt->x2 + wt->margin + 1 + column] = color;
                }
            }
        }
        if (fill && x <= wt->x1 && x + w - 1 >= wt->x2)
            wide_picture_words(wt, wt->x1, y1, wt->x2 - wt->x1 + 1 + 2 * wt->margin, y2 - y1 + 1);
        else
            wide_picture_words(wt, x1 + wt->margin, y1, x2 - x1 + 1, y2 - y1 + 1);
    }
}

int SoftGpu_WideMargin(int x1, int y1, int x2, int y2)
{
    int w = x2 - x1 + 1, h = y2 - y1 + 1, margin;
    if (!wide_on || w < 256 || h < 192) return 0;
    margin = (w / 6 + 1) & ~1; /* w * 4/3 in all, rounded to even */
    return x2 + 2 * margin < SOFT_GPU_WIDTH ? margin : 0;
}

int SoftGpu_Widescreen(void) { return wide_on; }

/* The target for the current drawing area, made on first use; NULL when
 * widescreen is off or the area is not a full screen. */
static WideTarget *wide_target(void)
{
    int w = gpu.clip_x2 - gpu.clip_x1 + 1, h = gpu.clip_y2 - gpu.clip_y1 + 1, margin, t, oldest = 0;
    WideTarget *wt;
    margin = SoftGpu_WideMargin(gpu.clip_x1, gpu.clip_y1, gpu.clip_x2, gpu.clip_y2);
    if (!margin) {
        return NULL;
    }
    for (t = 0; t < WIDE_TARGETS; t++) {
        wt = &wide[t];
        if (wt->pixels && wt->x1 == gpu.clip_x1 && wt->y1 == gpu.clip_y1 && wt->x2 == gpu.clip_x2 &&
            wt->y2 == gpu.clip_y2) {
            wt->stamp = ++wide_clock;
            wide_picture_prepare(wt);
            return wt;
        }
        if (wide[t].stamp < wide[oldest].stamp) {
            oldest = t;
        }
    }
    wt = &wide[oldest];
    if (!wt->pixels && !(wt->pixels = malloc(sizeof(vram)))) {
        return NULL;
    }
    free(wt->picture);
    wt->picture = NULL;
    memset(wt->pixels, 0, sizeof(vram));
    wt->x1 = gpu.clip_x1;
    wt->y1 = gpu.clip_y1;
    wt->x2 = gpu.clip_x2;
    wt->y2 = gpu.clip_y2;
    wt->margin = margin;
    wt->drawn = 0;
    wt->stamp = ++wide_clock;
    /* Start from what VRAM holds, so a target made mid-frame is not black. */
    wide_mirror(wt->x1, wt->y1, w, h, 0, 0);
    wide_picture_prepare(wt);
    return wt;
}

static void wide_drop(void)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        free(wide[t].picture);
        free(wide[t].pixels);
        memset(&wide[t], 0, sizeof(wide[t]));
    }
}

void SoftGpu_SetWidescreen(int on)
{
    if (on == wide_on) {
        return;
    }
    wide_on = on;
    wide_drop();
}

int SoftGpu_WideRastered(void) { return wide_rastered(); }

const uint32_t *SoftGpu_WidePicture(int x, int y, int w, int h)
{
    for (int t = 0; t < WIDE_TARGETS; t++) {
        WideTarget *wt = &wide[t];
        if (wt->pixels && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w &&
            wt->y2 - wt->y1 + 1 >= h) {
            if (wt->picture) SoftGpu_Settle(x, y, w, h); /* drawn with the area's words */
            return wt->picture;
        }
    }
    return NULL;
}

int SoftGpu_WideFrameView(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    pixels = GuestRuntime_ResolveData(pixels, sizeof(*pixels)); out_x = GuestRuntime_ResolveData(out_x, sizeof(*out_x)); out_w = GuestRuntime_ResolveData(out_w, sizeof(*out_w));
#endif
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        const WideTarget *wt = &wide[t];
        if (wt->pixels && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w && wt->y2 - wt->y1 + 1 >= h) {
            /* Rastered with the area, unless a recorder draws it (then
             * these words are not shown and there is nothing to settle). */
            if (wide_rastered()) SoftGpu_Settle(x, y, w, h);
            *pixels = wt->pixels;
            *out_x = x;
            *out_w = w + 2 * wt->margin;
            return 1;
        }
    }
    return 0;
}

int SoftGpu_WideFrame(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    pixels = GuestRuntime_ResolveData(pixels, sizeof(*pixels)); out_x = GuestRuntime_ResolveData(out_x, sizeof(*out_x)); out_w = GuestRuntime_ResolveData(out_w, sizeof(*out_w));
#endif
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        WideTarget *wt = &wide[t];
        if (wt->pixels && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w && wt->y2 - wt->y1 + 1 >= h) {
            if (wide_rastered()) SoftGpu_Settle(x, y, w, h); /* as SoftGpu_WideFrameView */
            if (!wt->drawn) {
                /* Nothing was drawn since it was last shown (a movie, or a
                 * still loaded straight into VRAM): the sides are stale. */
                int row;
                for (row = y; row < y + h; row++) {
                    uint16_t *out = wt->pixels + (row & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH;
                    memset(out + wt->x1, 0, (size_t)wt->margin * 2);
                    memset(out + wt->x2 + wt->margin + 1, 0, (size_t)wt->margin * 2);
                }
                wide_picture_words(wt, wt->x1, y, wt->margin, h);
                wide_picture_words(wt, wt->x2 + wt->margin + 1, y, wt->margin, h);
            }
            wt->drawn = 0;
            *pixels = wt->pixels;
            *out_x = x;
            *out_w = w + 2 * wt->margin;
            return 1;
        }
    }
    return 0;
}

static void load_words(int x, int y, int w, int h, const uint16_t *pixels)
{
    int i, j;
    if (gpu.mask_check) SoftGpu_Settle(x, y, w, h); /* the mask bits it keeps */
    {
        const Write load = {pixels, 0, 0, 0, 0, 0};
        before_write(x, y, w, h, SOFT_GPU_WHY_WRITE, &load);
    }
    if (TextureDump_Tags) TextureDump_Loaded(x, y, w, h, pixels);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t *target = vram_pixel(x + i, y + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(pixels[j * w + i] | (gpu.mask_set ? 0x8000 : 0));
            }
        }
    }
    wide_mirror(x, y, w, h, 0, 0);
    picture_from_words(x, y, w, h);
}

void SoftGpu_Store(int x, int y, int w, int h, uint16_t *pixels)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    if (w > 0 && h > 0) pixels = GuestRuntime_ResolveData(pixels, (size_t)w * (size_t)h * sizeof(*pixels));
#endif
    int i, j;
    SoftGpu_Settle(x, y, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            pixels[j * w + i] = *vram_pixel(x + i, y + j);
        }
    }
}

static void move_words(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    /* Onto itself without the mask (the game's 2x1 at 0,0 every frame) it
     * changes nothing and reads nothing. */
    if (sx != dx || sy != dy || gpu.mask_set || gpu.mask_check) {
        SoftGpu_Settle(sx, sy, w, h);
        const Write move = {NULL, sx, sy, 1, 0, 0};
        before_write(dx, dy, w, h, SOFT_GPU_WHY_WRITE, &move); /* and writes nothing either */
    }
    if (gpu.mask_check) SoftGpu_Settle(dx, dy, w, h); /* the mask bits it keeps */
    if (TextureDump_Tags) TextureDump_Moved(sx, sy, dx, dy, w, h);
    for (j = 0; j < h; j++) {
        /* Overlapping copies read each row forwards, as the hardware does. */
        for (i = 0; i < w; i++) {
            uint16_t *target = vram_pixel(dx + i, dy + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(*vram_pixel(sx + i, sy + j) | (gpu.mask_set ? 0x8000 : 0));
            }
        }
    }
    wide_mirror(dx, dy, w, h, 0, 0);
    if (picture) {
        /* The picture's own pixels move, row by row forwards like the words. */
        int hw = w * scale, hh = h * scale, hsx = sx * scale, hsy = sy * scale, hdx = dx * scale, hdy = dy * scale;
        if (!gpu.mask_check) {
            for (j = 0; j < hh; j++) {
                for (i = 0; i < hw; i++) *picture_pixel(hdx + i, hdy + j) = *picture_pixel(hsx + i, hsy + j);
            }
        } else {
            /* A word the mask kept was not written: its pixels stay too. */
            for (j = 0; j < h; j++) {
                for (i = 0; i < w; i++) {
                    int px, py;
                    if (*pixel(dx + i, dy + j) != (uint16_t)(*pixel(sx + i, sy + j) | 0x8000)) continue;
                    for (py = 0; py < scale; py++) {
                        for (px = 0; px < scale; px++) {
                            *picture_pixel(hdx + i * scale + px, hdy + j * scale + py) =
                                *picture_pixel(hsx + i * scale + px, hsy + j * scale + py);
                        }
                    }
                }
            }
        }
    }
}

static uint16_t pack(uint32_t rgb24)
{
    return (uint16_t)(((rgb24 >> 3) & 0x1f) | (((rgb24 >> 11) & 0x1f) << 5) |
                      (((rgb24 >> 19) & 0x1f) << 10));
}

static void fill_words(int x, int y, int w, int h, uint32_t rgb24)
{
    int i, j;
    uint16_t color = pack(rgb24);
    before_fill(x, y, w, h, color);
    if (TextureDump_Tags) TextureDump_Cleared(x, y, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *vram_pixel(x + i, y + j) = color;
        }
    }
    wide_mirror(x, y, w, h, 1, color);
    picture_from_words(x, y, w, h);
}

/* The transfers from outside a batch (LoadImage and the like, which can
 * come from the interrupt tick): recorded one by one. A batch's own
 * transfers are in its record already. */
void SoftGpu_Load(int x, int y, int w, int h, const uint16_t *pixels)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    if (w > 0 && h > 0) pixels = GuestRuntime_ResolveData((void *)pixels, (size_t)w * (size_t)h * sizeof(*pixels));
#endif
    if (recorder && scale > 1) recorder->load(x, y, w, h, pixels);
    if (held_any) {
        hold_clock();
        load_words(x, y, w, h, pixels);
        release_clock();
    } else {
        load_words(x, y, w, h, pixels);
    }
}

void SoftGpu_Move(int sx, int sy, int dx, int dy, int w, int h)
{
    if (recorder && scale > 1) recorder->move(sx, sy, dx, dy, w, h);
    if (held_any) {
        hold_clock();
        move_words(sx, sy, dx, dy, w, h);
        release_clock();
    } else {
        move_words(sx, sy, dx, dy, w, h);
    }
}

void SoftGpu_Capture(int sx, int sy, int dx, int dy, int w, int h)
{
    /* The copy stands for what the StoreImage before it read there. */
    SoftGpu_Settle(sx, sy, w, h);
    if (recorder && scale > 1 && recorder->capture) recorder->capture(sx, sy, dx, dy, w, h);
}

void SoftGpu_Fill(int x, int y, int w, int h, uint32_t rgb24)
{
    if (recorder && scale > 1) recorder->fill(x, y, w, h, rgb24);
    if (held_any) {
        hold_clock();
        fill_words(x, y, w, h, rgb24);
        release_clock();
    } else {
        fill_words(x, y, w, h, rgb24);
    }
}

/* What a primitive's texels are read with, and what plot draws it with:
 * the state as the primitive starts, taken once. Nothing a primitive's
 * pixels do changes it, and in locals it stays in registers; read from the
 * globals at each pixel it was loaded again after every store, since any
 * store may alias them here (-fno-strict-aliasing). */
typedef struct Sampler {
    int keep_u, set_u, keep_v, set_v; /* the texture window, as masks */
    int page_x, page_y, depth, clut_x, clut_y, shadow_on;
    const uint16_t *source;
} Sampler;

typedef struct Plotter {
    Sampler texels;
    int clip_x1, clip_y1, clip_x2, clip_y2;
    int mask_check, mask_set, dither, dither_shift, blend, fade;
    uint16_t *target;
    uint32_t *tags; /* NULL into a widescreen target or with provenance tags off */
    uint16_t *shadow;
} Plotter;

static inline __attribute__((always_inline)) Sampler sampler(void)
{
    Sampler s;
    s.keep_u = ~(gpu.window_mask_x * 8);
    s.set_u = (gpu.window_x & gpu.window_mask_x) * 8;
    s.keep_v = ~(gpu.window_mask_y * 8);
    s.set_v = (gpu.window_y & gpu.window_mask_y) * 8;
    s.page_x = gpu.page_x;
    s.page_y = gpu.page_y;
    s.depth = gpu.depth;
    s.clut_x = gpu.clut_x;
    s.clut_y = gpu.clut_y;
    s.shadow_on = shadow_on;
    s.source = texture_source;
    return s;
}

/* The same word in whichever bank the primitive's texture page names. */
static inline __attribute__((always_inline)) uint16_t sample(const Sampler *s, int x, int y)
{
    return s->source[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

static inline __attribute__((always_inline)) uint16_t texel_from(const Sampler *s, int u, int v)
{
    int x, y;
    uint16_t word;
    u = (u & s->keep_u) | s->set_u;
    v = (v & s->keep_v) | s->set_v;
    u &= 0xff;
    v &= 0xff;
    y = s->page_y + v;
    if (s->depth == 0) {
        x = s->page_x + u / 4;
        word = sample(s, s->clut_x + ((sample(s, x, y) >> ((u & 3) * 4)) & 0xf), s->clut_y);
    } else if (s->depth == 1) {
        x = s->page_x + u / 2;
        word = sample(s, s->clut_x + ((sample(s, x, y) >> ((u & 1) * 8)) & 0xff), s->clut_y);
    } else {
        word = sample(s, s->page_x + u, y);
    }
    if (s->shadow_on == 1) {
        /* A replaced texel: the pack's color, 0 for one painted transparent.
         * The texel's own semi-transparency bit stays: a pack replaces the
         * color, not how the game draws it. Black is 0x8000 with that bit
         * and the darkest non-zero color without it (texture_dump.h).
         * (2: the primitive reads the words with another palette than the
         * shadow's image; the scaled picture has that image, VRAM stays.) */
        uint16_t cell = s->depth == 0 ? *TextureDump_Cell(s->page_x + u / 4, y, u & 3)
                        : s->depth == 1 ? *TextureDump_Cell(s->page_x + u / 2, y, (u & 1) * 2)
                                        : *TextureDump_Cell(s->page_x + u, y, 0);
        if (cell == TEXTURE_SHADOW_BLACK) return (word & 0x8000) ? 0x8000 : 0x0001;
        if (cell) return (cell & 0x7fff) ? (uint16_t)((cell & 0x7fff) | (word & 0x8000)) : 0;
    }
    return word;
}

static inline __attribute__((always_inline)) uint16_t texel(int u, int v)
{
    const Sampler s = sampler();
    return texel_from(&s, u, v);
}

static inline __attribute__((always_inline)) int clamp8(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* The state plot draws a primitive with, as it starts. */
static inline __attribute__((always_inline)) Plotter plotter(void)
{
    Plotter p;
    p.texels = sampler();
    p.clip_x1 = gpu.clip_x1;
    p.clip_y1 = gpu.clip_y1;
    p.clip_x2 = gpu.clip_x2;
    p.clip_y2 = gpu.clip_y2;
    p.mask_check = gpu.mask_check;
    p.mask_set = gpu.mask_set ? 0x8000 : 0;
    p.dither = gpu.dither;
    p.dither_shift = dither_shift;
    p.blend = gpu.blend;
    p.fade = fade;
    p.target = target;
    /* A held primitive's tags were found clear as it came (soft_gpu.c's
     * deferred raster): anything tagged since came after it. */
    p.tags = TextureDump_Tags && target == vram && !replaying ? TextureDump_Tags : NULL;
    p.shadow = TextureDump_Shadow;
    return p;
}

/* flags: 1 raw texture, 2 semi-transparent, 4 textured, 8 dither-eligible */
static inline __attribute__((always_inline)) void plot(const Plotter *p, int x, int y, int r, int g, int b, int u,
                                                       int v, int flags)
{
    uint16_t *target, source;
    int semi = flags & 2;
    if (x < p->clip_x1 || x > p->clip_x2 || y < p->clip_y1 || y > p->clip_y2) {
        return;
    }
    target = &p->target[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
    if (p->mask_check && (*target & 0x8000)) {
        return;
    }
    if (p->tags) { /* drawn, not from the disc */
        size_t at = (size_t)(target - vram);
        p->tags[at] = 0;
        if (p->shadow) memset(p->shadow + at * 4, 0, 4 * sizeof(uint16_t));
    }
    if (flags & 4) {
        source = flags & 16 ? (uint16_t)u : texel_from(&p->texels, u, v); /* 16: the texel's word given in u */
        if (!source) {
            return;
        }
        semi = semi && (source & 0x8000);
        if (flags & 1) {
            r = (source & 0x1f) << 3;
            g = ((source >> 5) & 0x1f) << 3;
            b = ((source >> 10) & 0x1f) << 3;
        } else {
            r = ((source & 0x1f) * r) >> 4;
            g = (((source >> 5) & 0x1f) * g) >> 4;
            b = (((source >> 10) & 0x1f) * b) >> 4;
        }
    } else {
        source = 0;
    }
    if ((flags & 8) && p->dither) {
        int offset = dither_matrix[y & 3][(x - p->dither_shift) & 3];
        r += offset;
        g += offset;
        b += offset;
    }
    r = clamp8(r) >> 3;
    g = clamp8(g) >> 3;
    b = clamp8(b) >> 3;
    if (semi) {
        int br = *target & 0x1f, bg = (*target >> 5) & 0x1f, bb = (*target >> 10) & 0x1f;
        switch (p->blend) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
    }
    if (p->fade) {
        int br = *target & 0x1f, bg = (*target >> 5) & 0x1f, bb = (*target >> 10) & 0x1f;
        r = (r * (255 - p->fade) + br * p->fade) / 255;
        g = (g * (255 - p->fade) + bg * p->fade) / 255;
        b = (b * (255 - p->fade) + bb * p->fade) / 255;
    }
    *target = (uint16_t)(r | (g << 5) | (b << 10) | (source & 0x8000) | p->mask_set);
}

/* --- the scaled picture's own pass ------------------------------------
 * The same primitives, drawn again at scale x scale pixels per word into
 * the picture: positions and clipping scaled, texture coordinates carried
 * with a fraction so that a pack's image is sampled at its own resolution,
 * VRAM's own texels otherwise. No dithering; the mask bit is VRAM's. */

/* A texel for the picture: the pack's image if it replaces it, else the
 * word in VRAM through the palette. u and v in 16.16 texels. */
static inline __attribute__((always_inline)) int picture_texel(int u, int v, uint32_t *rgb)
{
    uint16_t word;
    if (texture_bank && scale > 1 && TextureDump_BankSample) { /* 1x: the bank's own texels */
        int got = TextureDump_BankSample(texture_bank, gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y,
                                         u, v, scale, rgb);
        if (got == 1) {
            /* The PNG supplies color and coverage, but the bank texel still
             * controls PS1 semi-transparency. */
            *rgb = (*rgb & 0x7fffffffu) | ((uint32_t)(texel(u >> 16, v >> 16) & 0x8000) << 16);
            return 1;
        }
        if (got == 2) return 0;
    }
    if (shadow_on && TextureDump_Sample) {
        int got = TextureDump_Sample(gpu.page_x, gpu.page_y, gpu.depth, u, v, rgb);
        if (got == 1) {
            /* The pack's color and how much it covers; the texel's own
             * semi-transparency bit. */
            *rgb = (*rgb & 0x7fffffffu) | ((uint32_t)(texel(u >> 16, v >> 16) & 0x8000) << 16);
            return 1;
        }
        if (got == 2) return 0;
    }
    word = texel(u >> 16, v >> 16);
    if (!word) return 0;
    *rgb = expand(word) | ((uint32_t)(word & 0x8000) << 16); /* bit 31: semi-transparent texel */
    return 1;
}

static inline __attribute__((always_inline)) void picture_plot_in(int hx, int hy, int r, int g, int b, int u, int v,
                                                                  int flags)
{
    uint32_t *target, rgb = 0;
    int semi = flags & 2;
    if (gpu.mask_check && (*pixel(hx >> scale_shift, hy >> scale_shift) & 0x8000)) return;
    target = wide_picture ? &wide_picture[(size_t)(hy & (PICTURE_HEIGHT - 1)) * PICTURE_WIDTH +
                                          (hx & (PICTURE_WIDTH - 1))] : picture_pixel(hx, hy);
    if (flags & 4) {
        if (!picture_texel(u, v, &rgb)) return;
        semi = semi && (rgb & 0x80000000u);
        if (flags & 1) {
            r = (int)((rgb >> 16) & 0xff);
            g = (int)((rgb >> 8) & 0xff);
            b = (int)(rgb & 0xff);
        } else {
            r = (int)(((rgb >> 16) & 0xff) * r) >> 7;
            g = (int)(((rgb >> 8) & 0xff) * g) >> 7;
            b = (int)((rgb & 0xff) * b) >> 7;
        }
    }
    r = clamp8(r);
    g = clamp8(g);
    b = clamp8(b);
    if (semi) {
        int br = (int)((*target >> 16) & 0xff), bg = (int)((*target >> 8) & 0xff), bb = (int)(*target & 0xff);
        switch (gpu.blend) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = clamp8(r);
        g = clamp8(g);
        b = clamp8(b);
    }
    if ((flags & 4) && (rgb & 0x7f000000u)) {
        /* A pack pixel that covers part of its place: the result mixed over
         * what was there. */
        int cover = 127 - (int)((rgb >> 24) & 0x7f);
        int br = (int)((*target >> 16) & 0xff), bg = (int)((*target >> 8) & 0xff), bb = (int)(*target & 0xff);
        r = br + (r - br) * cover / 127;
        g = bg + (g - bg) * cover / 127;
        b = bb + (b - bb) * cover / 127;
    }
    if (fade) {
        int br = (int)((*target >> 16) & 0xff), bg = (int)((*target >> 8) & 0xff), bb = (int)(*target & 0xff);
        r = (r * (255 - fade) + br * fade) / 255;
        g = (g * (255 - fade) + bg * fade) / 255;
        b = (b * (255 - fade) + bb * fade) / 255;
    }
    *target = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* The same, for a caller that has not clipped (rectangles and lines). */
static inline __attribute__((always_inline)) void picture_plot(int hx, int hy, int r, int g, int b, int u, int v,
                                                               int flags)
{
    if (hx < gpu.clip_x1 * scale || hx >= (gpu.clip_x2 + 1) * scale || hy < gpu.clip_y1 * scale ||
        hy >= (gpu.clip_y2 + 1) * scale) {
        return;
    }
    picture_plot_in(hx, hy, r, g, b, u, v, flags);
}

static int64_t picture_edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t)(b->x - a->x) * scale * (y - a->y * scale) - (int64_t)(b->y - a->y) * scale * (x - a->x * scale);
}

static void picture_triangle(Vertex a, Vertex b, Vertex c, int flags)
{
    int min_x, max_x, min_y, max_y, bias0, bias1, bias2;
    int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) {
        Vertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    min_x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    max_x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    min_y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    max_y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (max_x - min_x > 1023 || max_y - min_y > 511) return;
    if (min_x < gpu.clip_x1) min_x = gpu.clip_x1;
    if (min_y < gpu.clip_y1) min_y = gpu.clip_y1;
    if (max_x > gpu.clip_x2) max_x = gpu.clip_x2;
    if (max_y > gpu.clip_y2) max_y = gpu.clip_y2;
    bias0 = owns(&b, &c) ? 0 : -1;
    bias1 = owns(&c, &a) ? 0 : -1;
    bias2 = owns(&a, &b) ? 0 : -1;
    {
        /* Edge functions on picture pixels, attributes in 12.20 from vertex
         * a, stepped per picture pixel (a word is `scale` steps). */
        enum { FRACTION = 20, BIAS = 1 << 12 };
        const int32_t ex0 = (b.y - c.y), ey0 = (c.x - b.x), ex1 = (c.y - a.y), ey1 = (a.x - c.x);
        const int32_t ex2 = (a.y - b.y), ey2 = (b.x - a.x);
        const int hx0 = min_x * scale, hx1 = (max_x + 1) * scale - 1, hy0 = min_y * scale, hy1 = (max_y + 1) * scale - 1;
        /* Everything fits 32 bits: an edge function is at most 8192 * 4096,
         * an attribute 255 << 20 plus its steps; the machine is 32-bit. */
        int32_t row0 = (int32_t)(-picture_edge(&c, &b, hx0, hy0)) + bias0 * scale;
        int32_t row1 = (int32_t)(-picture_edge(&a, &c, hx0, hy0)) + bias1 * scale;
        int32_t row2 = (int32_t)(-picture_edge(&b, &a, hx0, hy0)) + bias2 * scale;
        const int32_t sx0 = ex0 * scale, sx1 = ex1 * scale, sx2 = ex2 * scale; /* per picture pixel */
        const int32_t sy0 = ey0 * scale, sy1 = ey1 * scale, sy2 = ey2 * scale;
        const int values[5][3] = {{a.r, b.r, c.r}, {a.g, b.g, c.g}, {a.b, b.b, c.b}, {a.u, b.u, c.u}, {a.v, b.v, c.v}};
        int32_t step_x[5], step_y[5], row[5];
        int k, hy, hx;
        for (k = 0; k < 5; k++) {
            int64_t nx = (int64_t)ex0 * values[k][0] + (int64_t)ex1 * values[k][1] + (int64_t)ex2 * values[k][2];
            int64_t ny = (int64_t)ey0 * values[k][0] + (int64_t)ey1 * values[k][1] + (int64_t)ey2 * values[k][2];
            step_x[k] = (int32_t)((nx * (1 << FRACTION) + (nx < 0 ? -area / 2 : area / 2)) / area / scale);
            step_y[k] = (int32_t)((ny * (1 << FRACTION) + (ny < 0 ? -area / 2 : area / 2)) / area / scale);
            row[k] = (int32_t)(values[k][0] * (1 << FRACTION) + BIAS + (int64_t)step_x[k] * (hx0 - a.x * scale) +
                               (int64_t)step_y[k] * (hy0 - a.y * scale));
            if (k >= 3) {
                /* A word's texel is at its corner; where texels run
                 * backwards, its other picture pixels are moved back up to
                 * it (at most a texel), so a mirrored sprite shows the
                 * console's texels, not the next picture's column. */
                int64_t back_x = step_x[k] < 0 ? -(int64_t)step_x[k] * scale : 0;
                int64_t back_y = step_y[k] < 0 ? -(int64_t)step_y[k] * scale : 0;
                if (back_x > 1 << FRACTION) back_x = 1 << FRACTION;
                if (back_y > 1 << FRACTION) back_y = 1 << FRACTION;
                row[k] += (int32_t)((back_x + back_y) * (scale - 1) / scale);
            }
        }
        for (hy = hy0; hy <= hy1; hy++) {
            int32_t w0 = row0, w1 = row1, w2 = row2;
            int32_t r = row[0], g = row[1], blue = row[2], u = row[3], v = row[4];
            for (hx = hx0; hx <= hx1; hx++) {
                if ((w0 | w1 | w2) >= 0) {
                    picture_plot_in(hx, hy, r >> FRACTION, g >> FRACTION, blue >> FRACTION, u >> (FRACTION - 16),
                                    v >> (FRACTION - 16), flags);
                }
                w0 += sx0;
                w1 += sx1;
                w2 += sx2;
                r += step_x[0];
                g += step_x[1];
                blue += step_x[2];
                u += step_x[3];
                v += step_x[4];
            }
            row0 += sy0;
            row1 += sy1;
            row2 += sy2;
            for (k = 0; k < 5; k++) row[k] += step_y[k];
        }
    }
}

static int64_t edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t)(b->x - a->x) * (y - a->y) - (int64_t)(b->y - a->y) * (x - a->x);
}

/* Top-left rule: an edge owns its pixels when it is a top or a left edge. */
static int owns(const Vertex *a, const Vertex *b)
{
    int dx = b->x - a->x, dy = b->y - a->y;
    return dy < 0 || (dy == 0 && dx > 0);
}

/* Narrow first..last, pixel offsets along a row, to those at which an edge
 * function that is w at offset 0 and grows by d a pixel is >= 0. */
static inline __attribute__((always_inline)) void row_span(int32_t w, int32_t d, int *first, int *last)
{
    if (d > 0) {
        if (w < 0) {
            int64_t from = (-(int64_t)w + d - 1) / d;
            if (from > *first) *first = from > *last ? *last + 1 : (int)from;
        }
    } else if (w < 0) {
        *first = *last + 1; /* never inside: it only falls (or stays) */
    } else if (d < 0) {
        int64_t to = (int64_t)w / -(int64_t)d;
        if (to < *last) *last = (int)to;
    }
}

static inline __attribute__((always_inline)) void triangle_with(Vertex a, Vertex b, Vertex c, const int flags)
{
    int min_x, max_x, min_y, max_y, x, y, bias0, bias1, bias2;
    int64_t area = edge(&a, &b, c.x, c.y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        Vertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    min_x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    max_x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    min_y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    max_y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (max_x - min_x > 1023 || max_y - min_y > 511) {
        return;
    }
    if (min_x < gpu.clip_x1) { min_x = gpu.clip_x1; }
    if (min_y < gpu.clip_y1) { min_y = gpu.clip_y1; }
    if (max_x > gpu.clip_x2) { max_x = gpu.clip_x2; }
    if (max_y > gpu.clip_y2) { max_y = gpu.clip_y2; }
    /* With this winding, inside is edge >= 0; shared edges go to one side. */
    bias0 = owns(&b, &c) ? 0 : -1;
    bias1 = owns(&c, &a) ? 0 : -1;
    bias2 = owns(&a, &b) ? 0 : -1;
    {
        /* The edge functions are affine and, within the extent limit above,
         * fit 32 bits, so they are stepped rather than re-evaluated.
         * Attributes are stepped in 12.20 fixed point from vertex a, where
         * they are exact. The bias exceeds the accumulated rounding error
         * (under 2^-10 over the largest extent), so integer-valued samples
         * (1:1 texture mapping) never land a hair below their integer. */
        enum { FRACTION = 20, BIAS = 1 << 12 };
        const int32_t dx0 = b.y - c.y, dy0 = c.x - b.x, dx1 = c.y - a.y, dy1 = a.x - c.x;
        const int32_t dx2 = a.y - b.y, dy2 = b.x - a.x;
        int32_t row0 = (int32_t)-edge(&c, &b, min_x, min_y) + bias0;
        int32_t row1 = (int32_t)-edge(&a, &c, min_x, min_y) + bias1;
        int32_t row2 = (int32_t)-edge(&b, &a, min_x, min_y) + bias2;
        const int values[5][3] = {{a.r, b.r, c.r}, {a.g, b.g, c.g}, {a.b, b.b, c.b}, {a.u, b.u, c.u}, {a.v, b.v, c.v}};
        int32_t step_x[5], step_y[5], row[5];
        int k;
        const Plotter p = plotter();
        for (k = 0; k < 5; k++) {
            int64_t nx = (int64_t)dx0 * values[k][0] + (int64_t)dx1 * values[k][1] + (int64_t)dx2 * values[k][2];
            int64_t ny = (int64_t)dy0 * values[k][0] + (int64_t)dy1 * values[k][1] + (int64_t)dy2 * values[k][2];
            step_x[k] = (int32_t)((nx * (1 << FRACTION) + (nx < 0 ? -area / 2 : area / 2)) / area);
            step_y[k] = (int32_t)((ny * (1 << FRACTION) + (ny < 0 ? -area / 2 : area / 2)) / area);
            row[k] = (int32_t)(values[k][0] * (1 << FRACTION) + BIAS + (int64_t)step_x[k] * (min_x - a.x) +
                               (int64_t)step_y[k] * (min_y - a.y));
        }
        for (y = min_y; y <= max_y; y++) {
            /* The row's pixels inside are one run, from the first to the
             * last at which all three edge functions are >= 0: worked out
             * from them rather than tested across the whole box, which
             * half of a triangle's box is outside of. */
            int first = 0, last = max_x - min_x;
            row_span(row0, dx0, &first, &last);
            row_span(row1, dx1, &first, &last);
            row_span(row2, dx2, &first, &last);
            if (first <= last) {
                int32_t r = (int32_t)(row[0] + (int64_t)step_x[0] * first);
                int32_t g = (int32_t)(row[1] + (int64_t)step_x[1] * first);
                int32_t blue = (int32_t)(row[2] + (int64_t)step_x[2] * first);
                int32_t u = (int32_t)(row[3] + (int64_t)step_x[3] * first);
                int32_t v = (int32_t)(row[4] + (int64_t)step_x[4] * first);
                for (x = min_x + first; x <= min_x + last; x++) {
                    plot(&p, x, y, r >> FRACTION, g >> FRACTION, blue >> FRACTION, u >> FRACTION, v >> FRACTION, flags);
                    r += step_x[0];
                    g += step_x[1];
                    blue += step_x[2];
                    u += step_x[3];
                    v += step_x[4];
                }
            }
            row0 += dy0;
            row1 += dy1;
            row2 += dy2;
            for (k = 0; k < 5; k++) {
                row[k] += step_y[k];
            }
        }
    }
}

/* One copy of the loop per flag combination, so the per-pixel tests on
 * texturing, blending and dithering are resolved at compile time. */
static void triangle(Vertex a, Vertex b, Vertex c, int flags)
{
    switch (flags & 15) {
#define CASE(n) case n: triangle_with(a, b, c, n); break;
    CASE(0) CASE(1) CASE(2) CASE(3) CASE(4) CASE(5) CASE(6) CASE(7)
    CASE(8) CASE(9) CASE(10) CASE(11) CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    }
}

/* A line in the picture: the console's one-pixel line made scale times
 * thicker, as a quad in words ([0] and [1] one end, [2] and [3] the other,
 * in a polygon's order) that covers each picture pixel once, so a
 * semi-transparent line blends as often as on the console at any scale.
 * Along the major axis it runs from the first word's leading edge to the
 * last word's trailing edge. */
static void line_quad(const Vertex *a, const Vertex *b, Vertex quad[4])
{
    int dx = b->x - a->x, dy = b->y - a->y;
    int x_major = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
    const Vertex *first = (x_major ? dx : dy) < 0 ? b : a, *last = first == a ? b : a;
    quad[0] = quad[1] = *first;
    quad[2] = quad[3] = *last;
    if (x_major) {
        quad[1].y++;
        quad[2].x++;
        quad[3].x++;
        quad[3].y++;
    } else {
        quad[1].x++;
        quad[2].y++;
        quad[3].x++;
        quad[3].y++;
    }
}

static void line(Vertex a, Vertex b, int flags)
{
    int dx = b.x - a.x, dy = b.y - a.y;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int i;
    if ((dx < 0 ? -dx : dx) > 1023 || (dy < 0 ? -dy : dy) > 511) {
        return;
    }
    if (wide_picture || (target == vram && picture)) { /* first: see the polygon's note */
        Vertex quad[4];
        line_quad(&a, &b, quad);
        picture_triangle(quad[0], quad[1], quad[2], flags);
        picture_triangle(quad[1], quad[2], quad[3], flags);
    }
    const Plotter p = plotter();
    for (i = 0; i <= steps; i++) {
        int n = steps ? steps : 1;
        plot(&p, a.x + dx * i / n, a.y + dy * i / n, a.r + (b.r - a.r) * i / n,
             a.g + (b.g - a.g) * i / n, a.b + (b.b - a.b) * i / n, 0, 0, flags);
    }
}

static void set_color(Vertex *vertex, uint32_t word)
{
    vertex->r = word & 0xff;
    vertex->g = (word >> 8) & 0xff;
    vertex->b = (word >> 16) & 0xff;
}

static void set_position(Vertex *vertex, uint32_t word)
{
    vertex->x = (((int32_t)(word << 21)) >> 21) + gpu.offset_x;
    vertex->y = (((int32_t)(word << 5)) >> 21) + gpu.offset_y;
}

/* The texture coordinates a polygon's pixels read, for the deferred
 * raster (texture_rects): within its vertices' least and greatest. Each
 * pixel's are a mix of the vertices' (it is inside the triangle), stepped in
 * 12.20 with a bias of 2^-8 texel that outweighs the steps' rounding, so the
 * whole texel taken is never below the least nor above the greatest. Unless
 * the 32-bit steps overflow, which a sliver with its texture spread across
 * it can do (a coordinate's value at a corner of its box is at most
 * range * 4 * width * height / twice its area texels from a vertex's): then
 * its whole range, 0 to 256, so all of the page. */
static void polygon_uv(const Vertex *v, int vertices, int uv[4])
{
    int i, t;
    uv[0] = uv[2] = v[0].u;
    uv[1] = uv[3] = v[0].v;
    for (i = 1; i < vertices; i++) {
        if (v[i].u < uv[0]) uv[0] = v[i].u;
        if (v[i].v < uv[1]) uv[1] = v[i].v;
        if (v[i].u > uv[2]) uv[2] = v[i].u;
        if (v[i].v > uv[3]) uv[3] = v[i].v;
    }
    for (t = 0; t + 3 <= vertices; t++) { /* (0,1,2) and, for a quad, (1,2,3) */
        const Vertex *a = &v[t], *b = &v[t + 1], *c = &v[t + 2];
        int64_t twice = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
        int x0 = a->x, x1 = a->x, y0 = a->y, y1 = a->y, range = 0;
        if (twice < 0) twice = -twice;
        if (!twice) continue; /* drawn as nothing */
        for (i = 1; i < 3; i++) {
            const Vertex *p = &a[i];
            x0 = p->x < x0 ? p->x : x0;
            x1 = p->x > x1 ? p->x : x1;
            y0 = p->y < y0 ? p->y : y0;
            y1 = p->y > y1 ? p->y : y1;
        }
        for (i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                int du = a[i].u - a[j].u, dv = a[i].v - a[j].v;
                range = du > range ? du : dv > range ? dv : range;
            }
        if ((int64_t)range * (x1 - x0 + 2) * (y1 - y0 + 2) * 4 >= 768 * twice) {
            uv[0] = uv[1] = 0;
            uv[2] = uv[3] = 256; /* past 255: all of the page */
            return;
        }
    }
}

/* `polygon`: a polygon's page word, whose bit 10 names banks 16-31 (soft_gpu.h). */
static void set_page(uint32_t value, int polygon)
{
    int bank = (int)((value >> 11) & 15) | (polygon ? (int)((value >> 10) & 1) << 4 : 0);
    /* A held primitive sampled VRAM as it came (soft_gpu.c, `hold`), even
     * if a bank of its number has been made since. */
    texture_source = bank && banks[bank] && !replaying ? banks[bank] : vram;
    texture_bank = texture_source == vram ? 0 : bank;
    gpu.page_x = (value & 0xf) * 64;
    gpu.page_y = ((value >> 4) & 1) * 256;
    gpu.blend = (value >> 5) & 3;
    gpu.depth = (value >> 7) & 3;
    if (gpu.depth == 3) {
        gpu.depth = 2;
    }
}

static int panel_page(void);
static int panel_piece(int u0, int v0, int w, int h);
static void name_over_panel(int x, int y, int width, int height, int u0, int v0, int w, int h, const Vertex *color,
                            int flags);

/* SoftGpu_PictureOnlyClut */
enum { PICTURE_ONLY_CLUTS = 8 };
static struct { int x, y; } picture_only[PICTURE_ONLY_CLUTS];
static int picture_only_count;

void SoftGpu_PictureOnlyClut(int clut_x, int clut_y)
{
    int i;
    for (i = 0; i < picture_only_count; i++) {
        if (picture_only[i].x == clut_x && picture_only[i].y == clut_y) return;
    }
    if (i == PICTURE_ONLY_CLUTS) return;
    picture_only[i].x = clut_x;
    picture_only[i].y = clut_y;
    picture_only_count++;
}

static int picture_only_clut(int clut_x, int clut_y)
{
    if (!recorder || scale <= 1) return 0;
    for (int i = 0; i < picture_only_count; i++) {
        if (picture_only[i].x == clut_x && picture_only[i].y == clut_y) return 1;
    }
    return 0;
}

/* SoftGpu_PictureOnlyFrame */
static int picture_only_frame;

void SoftGpu_PictureOnlyFrame(int on) { picture_only_frame = on; }

static int picture_only_screen(void)
{
    return picture_only_frame && recorder && scale > 1 && gpu.clip_x2 - gpu.clip_x1 + 1 >= 256 &&
           gpu.clip_y2 - gpu.clip_y1 + 1 >= 192;
}

static size_t polygon(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int quad = command & 8, textured = command & 4, shaded = command & 0x10;
    int vertices = quad ? 4 : 3, i;
    size_t need = (size_t)vertices * (1 + (textured ? 1 : 0)) + (shaded ? (size_t)vertices : 1);
    size_t at = 0;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex v[4];
    if (count < need) {
        return 0;
    }
    if (shaded || (textured && !(command & 1))) {
        flags |= 8;
    }
    memset(v, 0, sizeof(v));
    for (i = 0; i < vertices; i++) {
        if (i == 0 || shaded) {
            set_color(&v[i], words[at++]);
        } else {
            v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b;
        }
        set_position(&v[i], words[at++]);
        if (textured) {
            uint32_t word = words[at++];
            v[i].u = word & 0xff;
            v[i].v = (word >> 8) & 0xff;
            if (i == 0) {
                gpu.clut_x = ((word >> 16) & 0x3f) * 16;
                gpu.clut_y = (word >> 22) & 0x1ff;
            } else if (i == 1) {
                set_page(word >> 16, 1);
            } else if (i == 2) {
                fade = (int)(word >> 16);
            }
        }
    }
    /* Only a primitive sampling a bank can fade: retail never names one. */
    fade = textured && texture_source != vram ? SoftGpu_FadeOf((uint32_t)fade) : 0;
    /* What a primitive does as it comes is done as it came, not again when
     * it is rastered later (the deferred raster's `hold`). */
    shadow_on = textured && TextureDump_Prepare && texture_source == vram && !replaying
                    ? TextureDump_Prepare(gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, v[0].u, v[0].v)
                    : 0;
    if (textured && TextureDump_Enabled && !replaying) {
        /* The texels the primitive covers: a quad's far edge is exclusive. */
        int u0 = v[0].u, u1 = v[0].u, v0 = v[0].v, v1 = v[0].v;
        for (i = 1; i < vertices; i++) {
            if (v[i].u < u0) u0 = v[i].u;
            if (v[i].u > u1) u1 = v[i].u;
            if (v[i].v < v0) v0 = v[i].v;
            if (v[i].v > v1) v1 = v[i].v;
        }
        /* It reads the texels as the primitive comes, not as it is drawn. */
        if (texture_source == vram) SoftGpu_SettleTexture(gpu.depth, gpu.page_x, gpu.page_y, gpu.clut_x, gpu.clut_y);
        TextureDump_Primitive(texture_source, gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, u0, v0,
                              u1 > u0 ? u1 - 1 : u1, v1 > v0 ? v1 - 1 : v1);
    }
    /* The picture's pass first: it reads the mask bits VRAM has before this
     * primitive, as the primitive's own pass does. */
    if (wide_picture || (target == vram && picture)) {
        picture_triangle(v[0], v[1], v[2], flags);
        if (quad) picture_triangle(v[1], v[2], v[3], flags);
    }
    if ((textured && picture_only_clut(gpu.clut_x, gpu.clut_y)) || picture_only_screen() ||
        hold(words, need, NULL, textured ? v : NULL, vertices, textured && panel_page())) {
        fade = 0;
        return need;
    }
    triangle(v[0], v[1], v[2], flags);
    if (quad) {
        triangle(v[1], v[2], v[3], flags);
    }
    fade = 0;
    /* A piece of the life-point panel a mod's "ui" draws at another size. */
    if (quad && textured && SoftGpu_PanelCut && !replaying) {
        int x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y, u0 = v[0].u, v0 = v[0].v, u1 = v[0].u, v1 = v[0].v;
        for (i = 1; i < 4; i++) {
            if (v[i].x < x0) x0 = v[i].x;
            if (v[i].y < y0) y0 = v[i].y;
            if (v[i].x > x1) x1 = v[i].x;
            if (v[i].y > y1) y1 = v[i].y;
            if (v[i].u < u0) u0 = v[i].u;
            if (v[i].v < v0) v0 = v[i].v;
            if (v[i].u > u1) u1 = v[i].u;
            if (v[i].v > v1) v1 = v[i].v;
        }
        if (panel_piece(u0, v0, u1 - u0, v1 - v0))
            name_over_panel(x0, y0, x1 - x0, y1 - y0, u0, v0, u1 - u0, v1 - v0, &v[0], (int)(command & 3) | 4);
    }
    return need;
}

const uint8_t *(*SoftGpu_PanelName)(int which, int *x, int *y, int *width, int *height, int *stride);
int SoftGpu_PanelCut;

/* The opponent's name over the life-point panel just drawn, and the
 * player's for YOU, at the console's resolution (the OpenGL picture draws
 * them above it): the boxes' palette indices through the panel's CLUT, as
 * the panel's own texels would be, so the inactive side's dimming applies.
 * The panel whole, or a piece of it drawn at any size (a mod's "ui"): its
 * texels u0, v0 on (w x h) over x, y to x + width, y + height; a box goes
 * with the piece that has its rows and the column it meets the panel at. */
static int panel_page(void)
{
    return SoftGpu_PanelName && scale == 1 && texture_source == vram && gpu.depth == 0 && gpu.page_x == 704 &&
           gpu.page_y == 0 && (gpu.clut_x == 736 || gpu.clut_x == 752) && gpu.clut_y == 252;
}

static int panel_piece(int u0, int v0, int w, int h)
{
    return panel_page() && w > 0 && h > 0 && u0 >= 128 && v0 >= 128 && u0 + w <= 192 && v0 + h <= 168;
}

static void name_over_panel(int x, int y, int width, int height, int u0, int v0, int w, int h, const Vertex *color,
                            int flags)
{
    int which, bx, by, bw, bh, stride, i, j;
    /* The panel's 4-bit palette, read below word by word. The panel is
     * always rastered at once (`hold`), which settled it, and the game's
     * (736 or 752, 252) lies outside the drawing areas; settled here all the
     * same, so no reader of VRAM in this file goes without. */
    before_read(gpu.clut_x, gpu.clut_y, 16, 1, SOFT_GPU_WHY_READ);
    for (which = 0; which < 2; which++) {
        const uint8_t *box = SoftGpu_PanelName(which, &bx, &by, &bw, &bh, &stride);
        int left, top, right, bottom;
        /* As ever, no player's box without the opponent's; but a mod's
         * label may be for YOU alone. */
        if (!box) {
            if (!SoftGpu_PanelCut) return;
            continue;
        }
        if (by < v0 - 128 || by + bh > v0 - 128 + h || bx + bw <= u0 - 128 || bx + bw > u0 - 128 + w) continue;
        left = x + (bx - (u0 - 128)) * width / w;
        top = y + (by - (v0 - 128)) * height / h;
        right = x + (bx + bw - (u0 - 128)) * width / w;
        bottom = y + (by + bh - (v0 - 128)) * height / h;
        const Plotter p = plotter();
        for (j = top; j < bottom; j++) {
            for (i = left; i < right; i++) {
                /* Each pixel the box texel under it, as a quad samples. */
                int tx = (i - left) * bw / (right - left), ty = (j - top) * bh / (bottom - top);
                uint16_t word = vram[gpu.clut_y * SOFT_GPU_WIDTH + gpu.clut_x + box[(size_t)ty * stride + tx]];
                plot(&p, i, j, color->r, color->g, color->b, word, 0, (flags & 3) | 4 | 16);
            }
        }
    }
}

static size_t rectangle(const uint32_t *words, size_t count)
{
    static const int sizes[4] = {0, 1, 8, 16};
    uint32_t command = words[0] >> 24;
    int textured = command & 4, kind = (command >> 3) & 3, w, h, i, j;
    size_t need = 2 + (textured ? 1u : 0u) + (kind == 0 ? 1u : 0u), at = 2;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex base;
    if (count < need) {
        return 0;
    }
    memset(&base, 0, sizeof(base));
    set_color(&base, words[0]);
    set_position(&base, words[1]);
    if (textured) {
        base.u = words[at] & 0xff;
        base.v = (words[at] >> 8) & 0xff;
        gpu.clut_x = ((words[at] >> 16) & 0x3f) * 16;
        gpu.clut_y = (words[at] >> 22) & 0x1ff;
        at++;
    }
    w = h = sizes[kind];
    if (kind == 0) {
        w = words[at] & 0x3ff;
        h = (words[at] >> 16) & 0x1ff;
    }
    shadow_on = textured && TextureDump_Prepare && texture_source == vram && !replaying
                    ? TextureDump_Prepare(gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, base.u, base.v)
                    : 0;
    if (textured && TextureDump_Enabled && w && h && !replaying) {
        if (texture_source == vram) SoftGpu_SettleTexture(gpu.depth, gpu.page_x, gpu.page_y, gpu.clut_x, gpu.clut_y);
        TextureDump_Primitive(texture_source, gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, base.u,
                              base.v, base.u + w - 1, base.v + h - 1);
    }
    if (wide_picture || (target == vram && picture)) { /* first: see the polygon's note */
        int hw = w * scale, hh = h * scale;
        for (j = 0; j < hh; j++) {
            for (i = 0; i < hw; i++) {
                picture_plot(base.x * scale + i, base.y * scale + j, base.r, base.g, base.b,
                             (base.u << 16) + (i << 16) / scale, (base.v << 16) + (j << 16) / scale, flags);
            }
        }
    }
    {
        const int uv[4] = {base.u, base.v, base.u + (w ? w - 1 : 0), base.v + (h ? h - 1 : 0)};
        if (!picture_only_screen() && hold(words, need, textured ? uv : NULL, NULL, 0, textured && panel_page()))
            return need;
    }
    const Plotter p = plotter();
    switch (picture_only_screen() ? -1 : flags) {
#define CASE(n) case n: \
        for (j = 0; j < h; j++) { \
            for (i = 0; i < w; i++) { \
                plot(&p, base.x + i, base.y + j, base.r, base.g, base.b, base.u + i, base.v + j, n); \
            } \
        } \
        break;
    CASE(0) CASE(1) CASE(2) CASE(3) CASE(4) CASE(5) CASE(6) CASE(7)
#undef CASE
    }
    /* The life-point panel (gl_picture.c's name_over_panel finds it so):
     * whole, or a piece of it a mod's "ui" draws apart. */
    if (textured && !replaying && panel_piece(base.u, base.v, w, h) &&
        ((base.u == 128 && base.v == 128 && w == 64 && h == 40) || SoftGpu_PanelCut)) {
        name_over_panel(base.x, base.y, w, h, base.u, base.v, w, h, &base, flags);
    }
    return need;
}

/* How many words `lines` takes: 0 if the buffer cuts it off. */
static size_t lines_length(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int shaded = command & 0x10, poly = command & 8;
    size_t at = 2;
    if (count < (shaded ? 4u : 3u)) return 0;
    for (;;) {
        if (poly && at < count && (words[at] & 0xf000f000u) == 0x50005000u) return at + 1;
        if (shaded) {
            if (at >= count) return 0;
            at++;
        }
        if (at >= count) return 0;
        at++;
        if (!poly) return at;
        if (at >= count) return 0;
    }
}

static size_t lines(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int shaded = command & 0x10, poly = command & 8, flags = (command & 2) | (shaded ? 8 : 0);
    size_t at = 0;
    Vertex previous, next;
    memset(&previous, 0, sizeof(previous));
    if (count < (shaded ? 4u : 3u)) {
        return 0;
    }
    if (!replaying && (holding() || held_any)) {
        size_t used = lines_length(words, count); /* 0: cut off, drawn as far as it goes */
        if (hold(words, used, NULL, NULL, 0, 0)) return used;
    }
    set_color(&previous, words[at++]);
    set_position(&previous, words[at++]);
    for (;;) {
        if (poly && at < count && (words[at] & 0xf000f000u) == 0x50005000u) {
            return at + 1;
        }
        next = previous;
        if (shaded) {
            if (at >= count) { return 0; }
            set_color(&next, words[at++]);
        }
        if (at >= count) { return 0; }
        set_position(&next, words[at++]);
        line(previous, next, flags);
        previous = next;
        if (!poly) {
            return at;
        }
        if (at >= count) { return 0; }
    }
}

static const PgxpVertex *precise;
static size_t precise_count;

void SoftGpu_SetPrecise(const PgxpVertex *vertices, size_t count)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    if (count) vertices = GuestRuntime_ResolveData((void *)vertices, count * sizeof(*vertices));
#endif
    precise = vertices;
    precise_count = count;
}

size_t SoftGpu_Gp0(const uint32_t *words, size_t count)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    if (count) words = GuestRuntime_ResolveData((void *)words, count * sizeof(*words));
#endif
    size_t at = 0;
    int clock_held = holding() || held_any; /* at 2x and above, with a recorder or a mod's VRAM: never */
    if (recorder && scale > 1) {
        if (precise_count && recorder->precise) recorder->precise(precise, precise_count);
        recorder->gp0(words, count);
    }
    precise_count = 0;
    if (clock_held) hold_clock(); /* the deferred raster's queues (`hold`) */
    in_batch++;
    while (at < count) {
        uint32_t word = words[at], command = word >> 24;
        size_t used = 1;
        if (command >= 0x20 && command < 0x80) {
            size_t (*draw)(const uint32_t *, size_t) = command < 0x40 ? polygon : command < 0x60 ? lines : rectangle;
            WideTarget *wt = NULL;
            used = draw(words + at, count - at);
            if (used && (wt = wide_target()) != NULL && wide_rastered()) {
                /* Again into the widescreen target, shifted. Polygons and
                 * lines, which is what the GTE projects, are unclipped at the
                 * sides, so the scene carries on past the 4:3 edges. Sprites
                 * keep the 4:3 clip: they are the 2D screens, and those park
                 * what they do not show just past the edge (the deck
                 * builder's column of card numbers), where the console's clip
                 * hides it. Full-width flat fills (fades and wipe bands)
                 * extend over the sides. State words are idempotent. */
                int clip_x1 = gpu.clip_x1, clip_x2 = gpu.clip_x2, offset_x = gpu.offset_x;
                uint32_t fill[3];
                int full_width = SoftGpu_WidenFill(words + at, count - at, clip_x1, clip_x2, offset_x, wt->margin, fill);
                if (command < 0x60 || full_width) {
                    gpu.clip_x2 += 2 * wt->margin;
                } else {
                    gpu.clip_x1 += wt->margin;
                    gpu.clip_x2 += wt->margin;
                }
                gpu.offset_x += wt->margin;
                target = wt->pixels;
                wide_picture = wt->picture;
                dither_shift = wt->margin;
                draw(full_width ? fill : words + at, full_width ? 3 : count - at);
                wide_picture = NULL;
                target = vram;
                dither_shift = 0;
                gpu.clip_x1 = clip_x1;
                gpu.clip_x2 = clip_x2;
                gpu.offset_x = offset_x;
                wt->drawn++;
            } else if (wt) {
                wt->drawn++; /* the recorder drew it there (SoftGpu_WideFrame) */
            }
        } else if (command == 0x02) {
            used = count - at >= 3 ? 3 : 0;
            if (used) {
                fill_words(words[at + 1] & 0x3f0, (words[at + 1] >> 16) & 0x1ff,
                             ((words[at + 2] & 0x3ff) + 15) & ~15, (words[at + 2] >> 16) & 0x1ff, word);
            }
        } else if (command >= 0x80 && command < 0xa0) {
            used = count - at >= 4 ? 4 : 0;
            if (used) {
                move_words(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff,
                           words[at + 2] & 0x3ff, (words[at + 2] >> 16) & 0x1ff,
                           ((words[at + 3] - 1) & 0x3ff) + 1, (((words[at + 3] >> 16) - 1) & 0x1ff) + 1);
            }
        } else if (command >= 0xa0 && command < 0xc0) {
            if (count - at < 3) {
                used = 0;
            } else {
                int w = (int)((words[at + 2] - 1) & 0x3ff) + 1;
                int h = (int)(((words[at + 2] >> 16) - 1) & 0x1ff) + 1;
                size_t data = ((size_t)w * (size_t)h + 1) / 2;
                used = count - at >= 3 + data ? 3 + data : 0;
                if (used) {
                    load_words(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, w, h,
                               (const uint16_t *)(words + at + 3));
                }
            }
        } else if (command >= 0xc0 && command < 0xe0) {
            used = count - at >= 3 ? 3 : 0;
        } else if (command == 0xe1) {
            set_page(word, 0);
            gpu.dither = (word >> 9) & 1;
        } else if (command == 0xe2) {
            gpu.window_mask_x = word & 0x1f;
            gpu.window_mask_y = (word >> 5) & 0x1f;
            gpu.window_x = (word >> 10) & 0x1f;
            gpu.window_y = (word >> 15) & 0x1f;
        } else if (command == 0xe3) {
            gpu.clip_x1 = word & 0x3ff;
            gpu.clip_y1 = (word >> 10) & 0x3ff;
        } else if (command == 0xe4) {
            gpu.clip_x2 = word & 0x3ff;
            gpu.clip_y2 = (word >> 10) & 0x3ff;
        } else if (command == 0xe5) {
            gpu.offset_x = ((int32_t)(word << 21)) >> 21;
            gpu.offset_y = ((int32_t)(word << 10)) >> 21;
        } else if (command == 0xe6) {
            gpu.mask_set = word & 1;
            gpu.mask_check = (word >> 1) & 1;
        }
        if (!used) {
            break;
        }
        at += used;
    }
    in_batch--;
    if (clock_held) release_clock();
    return at;
}

/* Save states: VRAM (index 0) and the drawing state (index 1). */
void *SoftGpu_StateData(int index, size_t *size)
{
#if defined(MEMORIES_TRANSLATED) && !defined(MEMORIES_INSTRUMENT_SOFTGPU)
    size = GuestRuntime_ResolveData(size, sizeof(*size));
#endif
    *size = index ? sizeof(gpu) : sizeof(vram);
    /* Saved as it is drawn; a load writes it whole (and then calls
     * SoftGpu_PictureFromVram). */
    if (!index) SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    return index ? (void *)&gpu : (void *)vram;
}
