/* The deferred raster (soft_gpu.h, SoftGpu_SetDeferred; notes/pc-build.md,
 * "The deferred raster"): kept back or not, everything the software GPU
 * ends up holding is the same.
 *
 * One random run of frames, as the game makes them, played three times:
 * rastered as it comes, kept back, and kept back with the poison colour
 * (which shows as magenta to any reader that does not settle). Each frame
 * clears one of two 320x240 drawing areas and draws polygons, sprites and
 * lines into it, some sampling the other area, VRAM pages, palettes, a
 * texture bank or the texture pack's shadow, between loads, moves, fills,
 * stores, settles of a rectangle (a host lookup), tags and shadow cells
 * written (a pack painting), save states and a mod binding VRAM. Every
 * store's words, every state's VRAM and drawing state, the final VRAM, the
 * tags and the shadow must be the same in all three. TextureDump_Prepare,
 * which a textured primitive calls as it comes, also loads and stores at
 * times, as the game clock's tick can in the middle of a batch. The three
 * runs must also have kept work back, dropped some, rastered some and taken
 * a state while some was held, or they prove nothing. */
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS (guarded_memory) */
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_dump.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

static uint32_t seed;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static int below(int n) { return (int)(rnd() % (unsigned)n); }

/* Three frames in four are calm, as most of the game's are: what they load,
 * move, fill, store, settle, tag and sample lies right of the drawing areas
 * (x 640 on; a texture page or 8-bit palette that would wrap past x 1023
 * into the first area's columns is taken from rows below the areas), so the
 * held work lives until its area's next clear and is dropped there. The
 * others put all of it anywhere. They come four or five at a time (two
 * cycles of each area or so), as the game's stretches do: a cycle given up
 * leaves the next one given up too at times, so calm frames one by one
 * would rarely hold. Four or five, so that a busy stretch starts on either
 * area, and either area's calm queue is held while a busy frame's loads,
 * moves, fills and the rest run (Outcome.over_held: four at a time, the
 * first area's never was). */
static int calm, calm_run;
static int column(int w) { return calm ? 640 + below(384 - w) : below(1024); }

/* The copy runs (below, the OpenGL picture's copy of VRAM): at 2x with a
 * recorder, which draws a few palettes into the picture alone
 * (SoftGpu_PictureOnlyClut, one in the first area's rows) and whole frames
 * now and then (SoftGpu_PictureOnlyFrame). */
static const uint32_t only_cluts[2] = {(100u << 6) | 2u, (490u << 6) | 41u};
static int copy_mode;

/* The "tick" inside TextureDump_Prepare draws its numbers from a stream of
 * its own, so that the run's own stream is the same whatever it does. */
static uint32_t tick_seed;
static uint32_t tick_rnd(void)
{
    tick_seed = tick_seed * 22695477u + 1u;
    return tick_seed >> 8;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    while (bytes--) hash = (hash ^ *p++) * 1099511628211ull;
    return hash;
}

typedef struct Outcome {
    uint64_t stores;   /* every store's words, every state, in order */
    uint64_t vram, tags, shadow, gpu;
    int states_held;   /* states taken while work was held */
    int held_by_mod;   /* primitives held while a mod bound VRAM (must be none) */
    int over_held[2];  /* a busy frame's loads, moves, fills, stores, settles, tags and states
                        * made while the other area (0,0 or 320,0) held work */
    uint64_t reads;    /* copy runs: what the recorder's replays read of its copy */
    int replays;
    SoftGpuDeferredStats stats;
} Outcome;

static Outcome *now_outcome;

/* What is allocated while the stand-in for the clock's tick runs (`in_tick`):
 * nothing may be, a settle there included (a patch is made without
 * allocating). Counted where the link wraps the allocator (CMakeLists.txt). */
static volatile int in_tick; /* volatile: a store around a malloc the compiler knows is kept */
static int tick_allocs;
#ifdef SOFT_GPU_TEST_WRAP_ALLOC
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
static int fail_calloc; /* the next callocs fail (a frozen copy with no memory for it) */
static int callocs;     /* every calloc: none for a primitive rastered at once (no frozen copy) */
void *__wrap_malloc(size_t size)
{
    tick_allocs += in_tick;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size)
{
    tick_allocs += in_tick;
    callocs++;
    if (fail_calloc) return NULL;
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size)
{
    tick_allocs += in_tick;
    return __real_realloc(pointer, size);
}
#endif

/* The pack's prepare: page 2 (128,0) is "replaced" (1, the shadow), page 3
 * read through another palette (2); now and then a load and a store, as an
 * interrupt-level LoadImage or StoreImage can come in the middle of a batch. */
static int prepare(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    (void)depth; (void)clut_x; (void)clut_y; (void)u; (void)v;
    if (tick_rnd() % 23 == 0) {
        uint16_t words[16];
        int x = (int)(tick_rnd() % 1024), y = (int)(tick_rnd() % 512);
        for (int i = 0; i < 16; i++) words[i] = (uint16_t)tick_rnd();
        in_tick = 1;
        if (tick_rnd() & 1) {
            SoftGpu_Load(x, y, 4, 4, words);
        } else {
            SoftGpu_Store(x, y, 4, 4, words);
            now_outcome->stores = fnv(now_outcome->stores, words, sizeof(words));
        }
        in_tick = 0;
    }
    if (tick_rnd() % 17 == 0) {
        /* The pack painting the shadow's page as it prepares, inside the
         * batch: the same cells mostly. (No bank is written inside a batch:
         * their writers run between batches, below.) */
        int x = 640 + (int)(tick_rnd() % 60), y = 256 + (int)(tick_rnd() % 250), change = tick_rnd() % 4 == 0;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                *TextureDump_Cell(x + i, y + j, (int)(tick_rnd() % 4)) ^= (uint16_t)(change ? 0x0421 : 0);
    }
    return page_y == 256 && page_x == 640 ? 1 : page_y == 256 && page_x == 704 ? 2 : 0;
}

/* The opponent's name over the life-point panel: none (only that the hook
 * is set matters to the deferred raster: the panel goes at once). */
static const uint8_t *no_name(int which, int *x, int *y, int *width, int *height, int *stride)
{
    (void)which; (void)x; (void)y; (void)width; (void)height; (void)stride;
    return NULL;
}

/* The opponent's name, found through code that writes a bank (the host's
 * labels): it turns the bank's 4x4 texels at 640,0 green, then has none. */
static uint16_t *renamed_bank;
static const uint8_t *bank_name(int which, int *x, int *y, int *width, int *height, int *stride)
{
    (void)which; (void)x; (void)y; (void)width; (void)height; (void)stride;
    for (int j = 0; renamed_bank && j < 4; j++)
        for (int i = 0; i < 4; i++) renamed_bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x03e0;
    return NULL;
}

/* The pack's prepare for the fixed cases: the 16-bit page at 640,256 is
 * replaced (the shadow), nothing else, and no tick. */
static int shadow_prepare(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    (void)depth; (void)clut_x; (void)clut_y; (void)u; (void)v;
    return page_x == 640 && page_y == 256;
}

/* The platform's SoftGpu_InTick: the stand-in for the tick is running. */
static int test_in_tick(void) { return in_tick; }

static int holds, releases, depth, nested;
/* A tick stand-in armed to come at the next record (record_point): at once
 * if the clock is free then, else as it is let go, as SIGALRM would. */
static void (*armed_tick)(void), (*pending_tick)(void);
static void hold_clock(int on)
{
    if (on) {
        holds++;
        if (depth++) nested = 1;
    } else {
        releases++;
        if (!--depth && pending_tick) {
            void (*tick)(void) = pending_tick;
            pending_tick = NULL;
            tick();
        }
    }
}

static void record_point(void)
{
    void (*tick)(void) = armed_tick;
    if (!tick) return;
    armed_tick = NULL;
    if (depth) pending_tick = tick;
    else tick();
}

static uint32_t position(int x0, int y0)
{
    int x = x0 - 40 + below(400), y = y0 - 30 + below(300);
    if (below(3) == 0) { /* small ones, as models are made of */
        x = x0 + 100 + below(40);
        y = y0 + 100 + below(40);
    }
    return ((uint32_t)(y - y0) & 0x7ff) << 16 | ((uint32_t)(x - x0) & 0x7ff);
}

/* A texture page word: mostly VRAM pages away from the areas, some inside
 * them (the other area: what a frame samples of the last one), the pack's
 * pages (prepare), and bank 3. Calm frames too take the shadow's page and
 * bank 3, which are held (from their frozen copies, soft_gpu.c). */
static uint32_t page_word(void)
{
    int pick = below(16);
    uint32_t page = pick < 6 ? (uint32_t)(10 + below(6)) | (uint32_t)below(2) << 4 /* x 640..1023 */
                  : pick < 8 && !calm ? (uint32_t)below(10)                         /* in an area */
                  : pick < 10 ? 10u | 16u                                           /* the shadow's */
                  : pick < 11 ? 11u | 16u
                  : calm ? (uint32_t)(10 + below(6)) | 16u
                  : (uint32_t)below(16) | 16u;                                       /* the lower half */
    if (calm) { /* nothing that rasters at once: the area keeps holding */
        page = (uint32_t)(12 + below(4)) | (uint32_t)below(2) << 4;
        if (below(2)) page = (uint32_t)(10 + below(6));
        if (below(6) == 0) page = 10u | 16u; /* the shadow's (prepare) */
    }
    page |= (uint32_t)below(4) << 5 | (uint32_t)below(3) << 7;
    /* A page whose texels run past x 1023 wraps into the first area's
     * columns (16 bits from 832 on, 8 from 960): a calm frame's takes the
     * lower half, so the wrap lands below the areas. */
    if (calm && (page & 15) * 64 + (64 << ((page >> 7) & 3)) > 1024) page |= 16;
    if (below(calm ? 6 : 12) == 0) page |= SOFT_GPU_BANK_TPAGE(3);
    return page;
}

static uint32_t clut_word(void)
{
    int x, y;
    if (copy_mode && below(8) == 0) return only_cluts[below(2)];
    x = below(4) == 0 && !calm ? below(40) : 40 + below(24), y = below(4) == 0 ? below(240) : 480 + below(32);
    /* A calm frame's 8-bit palette from x 768 on wraps past VRAM's edge
     * into the first area's columns: only in rows below the areas. */
    if (calm && y < 240 && x > 48) x = 40 + below(9);
    return (uint32_t)(y << 6 | x);
}

/* One primitive into the area at x0,y0, in `out`. Returns its words. */
static size_t primitive(uint32_t *out, int x0, int y0)
{
    int kind = below(10);
    size_t at = 0;
    if (kind < 6) {
        uint32_t command = 0x20 + (uint32_t)below(0x20);
        int quad = command & 8, textured = command & 4, shaded = command & 0x10;
        for (int v = 0; v < (quad ? 4 : 3); v++) {
            if (v == 0) out[at++] = command << 24 | (rnd() & 0xffffff);
            else if (shaded) out[at++] = rnd() & 0xffffff;
            out[at++] = position(x0, y0);
            if (textured) {
                /* The third's upper half: a bank polygon's fade (SoftGpu_FadeOf), now and then. */
                uint32_t extra = v == 0 ? clut_word() : v == 1 ? page_word() : below(3) == 0 ? 0x8000u | (uint32_t)below(256) : 0;
                out[at++] = extra << 16 | (rnd() & 0xffff);
            }
        }
    } else if (kind < 9) {
        uint32_t command = 0x60 + (uint32_t)below(0x20);
        out[at++] = command << 24 | (rnd() & 0xffffff);
        out[at++] = position(x0, y0);
        if (command & 4) out[at++] = clut_word() << 16 | (rnd() & 0xffff);
        if (((command >> 3) & 3) == 0) out[at++] = (uint32_t)(1 + below(120)) << 16 | (uint32_t)(1 + below(160));
    } else {
        uint32_t command = 0x40 + (uint32_t)below(0x20);
        int shaded = command & 0x10, poly = command & 8, points = poly ? 2 + below(4) : 2;
        for (int p = 0; p < points; p++) {
            if (p == 0 || shaded) out[at++] = (p == 0 ? command << 24 : 0) | (rnd() & 0xffffff);
            out[at++] = position(x0, y0);
        }
        if (poly) out[at++] = 0x55555555u;
    }
    return at;
}

static void noise(uint16_t *words, int count)
{
    for (int i = 0; i < count; i++) words[i] = (uint16_t)rnd();
}

/* --- The OpenGL picture's copy of VRAM, as a model (gl_picture.c) -------
 * At 2x and above a recorder draws the picture. Its copy of VRAM is taken
 * raw after each replay (upload_vram) and changed by the record's loads,
 * moves and fills, cut at VRAM's edges as gl_picture.c cuts them; its
 * primitives draw only into the picture, and sample the copy. So while the
 * deferred raster holds work across a take, the copy misses it, and gets it
 * back as patches (soft_gpu.h, SoftGpu_CopyTaken). The model replays the
 * record at the end of most frames, taking the patches first and before
 * every read (as flush_runs does: its host lookups settle too, which the
 * model stands in for with settles of its own in the middle of a replay),
 * and hashes every word of the copy that a textured primitive or a move
 * reads inside the drawing areas: kept back or not, they must be the same. */
static uint16_t copy_vram[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
static uint32_t *ops;
static size_t ops_used, ops_room;
static uint64_t copy_reads;    /* every word read in the drawing areas, in order */
static uint32_t window_seed;    /* the replay's own settles (host lookups): a stream of their own */
static int window_settles = 1;  /* off for the fixed cases */

static uint32_t *op_room(size_t n)
{
    if (ops_used + n > ops_room) {
        size_t room = ops_room ? ops_room : 1 << 16;
        while (room < ops_used + n) room *= 2;
        ops = realloc(ops, room * sizeof(uint32_t));
        if (!ops) abort();
        ops_room = room;
    }
    ops_used += n;
    return ops + ops_used - n;
}

static void rec_gp0(const uint32_t *words, size_t count)
{
    uint32_t *at = op_room(2 + count);
    at[0] = 1;
    at[1] = (uint32_t)count;
    memcpy(at + 2, words, count * 4);
    record_point();
}

static void rec_load(int x, int y, int w, int h, const uint16_t *pixels)
{
    uint32_t *at = op_room(5 + ((size_t)w * h + 1) / 2);
    at[0] = 2, at[1] = (uint32_t)x, at[2] = (uint32_t)y, at[3] = (uint32_t)w, at[4] = (uint32_t)h;
    memcpy(at + 5, pixels, (size_t)w * h * 2);
    record_point();
}

static void rec_move(int sx, int sy, int dx, int dy, int w, int h)
{
    uint32_t *at = op_room(7);
    at[0] = 3, at[1] = (uint32_t)sx, at[2] = (uint32_t)sy, at[3] = (uint32_t)dx, at[4] = (uint32_t)dy;
    at[5] = (uint32_t)w, at[6] = (uint32_t)h;
    record_point();
}

static void rec_fill(int x, int y, int w, int h, uint32_t rgb24)
{
    uint32_t *at = op_room(6);
    at[0] = 4, at[1] = (uint32_t)x, at[2] = (uint32_t)y, at[3] = (uint32_t)w, at[4] = (uint32_t)h, at[5] = rgb24;
    record_point();
}

static void rec_resync(int scale, const uint32_t state[6])
{
    uint32_t *at = op_room(2);
    (void)state;
    at[0] = 5;
    at[1] = (uint32_t)scale;
}

static void rec_patch(int x, int y, int w, int h, const uint16_t *words)
{
    for (int j = 0; j < h; j++) memcpy(copy_vram + (y + j) * SOFT_GPU_WIDTH + x, words + (size_t)j * w, (size_t)w * 2);
}

static const SoftGpuRecorder model = {rec_gp0, rec_load, rec_move, rec_fill, rec_resync, NULL, NULL, rec_patch};

/* A mod's recorder, made with a header from before `patch` (SoftGpu_SetRecorder
 * is lent to mods): its struct ends at `capture`. guarded_recorder() puts one
 * at the very end of memory that may be read, before a page that may not:
 * reading its `patch` faults. NULL where no such page can be had. */
static int foreign_patches;
static void foreign_patch(int x, int y, int w, int h, const uint16_t *words)
{
    (void)x, (void)y, (void)w, (void)h, (void)words;
    foreign_patches++;
}

#define GUARD_STEP 65536 /* a multiple of every page size */
#ifdef _WIN32
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long type, unsigned long protect);
__declspec(dllimport) int __stdcall VirtualProtect(void *address, size_t size, unsigned long protect,
                                                   unsigned long *old);
static char *guarded_memory(void)
{
    unsigned long old;
    char *base = VirtualAlloc(NULL, 2 * GUARD_STEP, 0x1000 | 0x2000, 4); /* committed, reserved; read and write */
    if (!base || !VirtualProtect(base + GUARD_STEP, GUARD_STEP, 1, &old)) return NULL; /* no access */
    return base;
}
#else
#include <sys/mman.h>
static char *guarded_memory(void)
{
    char *base = mmap(NULL, 2 * GUARD_STEP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED || mprotect(base + GUARD_STEP, GUARD_STEP, PROT_NONE)) return NULL;
    return base;
}
#endif

static SoftGpuRecorder *guarded_recorder(void)
{
    static const SoftGpuRecorder older = {rec_gp0, rec_load, rec_move, rec_fill, rec_resync, NULL, NULL, NULL};
    static char *base;
    size_t size = offsetof(SoftGpuRecorder, patch);
    if (!base && !(base = guarded_memory())) return NULL;
    memcpy(base + GUARD_STEP - size, &older, size);
    return (SoftGpuRecorder *)(void *)(base + GUARD_STEP - size);
}

static int in_areas(int x, int y)
{
    return (y < 240 && x < 640) || (y >= 256 && y < 496 && x < 320);
}

static void read_copy(int x, int y)
{
    x &= SOFT_GPU_WIDTH - 1;
    y &= SOFT_GPU_HEIGHT - 1;
    if (in_areas(x, y)) copy_reads = fnv(copy_reads, &copy_vram[y * SOFT_GPU_WIDTH + x], 2);
}

/* Before a draw: the patches, as flush_runs takes them. */
static void model_flush(void)
{
    SoftGpu_CopyPatches();
}

/* Where the model takes the patches and reads what its primitives sample
 * (`gl_placement`): 0 before every read (the patches, then the read, as the
 * primitive is parsed); 1 as gl_picture.c does: a primitive's lookups as it
 * is parsed, its reads when its run is drawn (flush_runs, before every
 * transfer, a resync and the replay's end), the patches at the replay's
 * start and at every flush, whether it draws or not. */
static int gl_placement;
static void model_flush_runs(void);

static int clip_cut(int *x, int *y, int *w, int *h)
{
    *x &= SOFT_GPU_WIDTH - 1;
    *y &= SOFT_GPU_HEIGHT - 1;
    if (*x + *w > SOFT_GPU_WIDTH) *w = SOFT_GPU_WIDTH - *x;
    if (*y + *h > SOFT_GPU_HEIGHT) *h = SOFT_GPU_HEIGHT - *y;
    return *w > 0 && *h > 0;
}

static void model_load(int x, int y, int w, int h, const uint16_t *pixels)
{
    int cw = w, ch = h;
    if (gl_placement) model_flush_runs();
    if (!clip_cut(&x, &y, &cw, &ch)) return;
    for (int j = 0; j < ch; j++) memcpy(copy_vram + (y + j) * SOFT_GPU_WIDTH + x, pixels + (size_t)j * w, (size_t)cw * 2);
}

static void model_move(int sx, int sy, int dx, int dy, int w, int h)
{
    static uint16_t scratch[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
    int sw = w, sh = h, dw = w, dh = h;
    if (gl_placement) model_flush_runs();
    if (!clip_cut(&sx, &sy, &sw, &sh) || !clip_cut(&dx, &dy, &dw, &dh)) return;
    w = sw < dw ? sw : dw;
    h = sh < dh ? sh : dh;
    if (sx == dx && sy == dy) return;
    if (!gl_placement) model_flush();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            read_copy(sx + i, sy + j);
            scratch[j * w + i] = copy_vram[(sy + j) * SOFT_GPU_WIDTH + sx + i];
        }
    for (int j = 0; j < h; j++) memcpy(copy_vram + (dy + j) * SOFT_GPU_WIDTH + dx, scratch + j * w, (size_t)w * 2);
}

static void model_fill(int x, int y, int w, int h, uint32_t rgb24)
{
    uint16_t word = (uint16_t)(((rgb24 >> 3) & 0x1f) | (((rgb24 >> 11) & 0x1f) << 5) | (((rgb24 >> 19) & 0x1f) << 10));
    if (gl_placement) model_flush_runs();
    if (!clip_cut(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) copy_vram[(y + j) * SOFT_GPU_WIDTH + x + i] = word;
}

/* A host lookup in the middle of the replay settles a rectangle of the
 * drawing areas (HdText_Hud a page or a palette), then a draw follows. */
static void window_settle(void)
{
    if (!window_settles) return;
    window_seed = window_seed * 69069u + 7u;
    if ((window_seed >> 24) % 97) return; /* now and then: one that crosses held work rasters it */
    SoftGpu_Settle((int)((window_seed >> 4) % 640), (int)((window_seed >> 12) % 240), 1 + (int)(window_seed % 64), 1 + (int)((window_seed >> 20) % 16));
}

/* What a textured primitive samples: its texels' words and, below 16 bits,
 * its palette (an 8-bit one past VRAM's edge up to the greatest index its
 * texels hold, as far as the software GPU settles it). */
typedef struct Sampled {
    int page_x, page_y, depth, clut_x, clut_y, u0, v0, u1, v1, wx, wy;
} Sampled;
static Sampled *pending;
static size_t pending_count, pending_room;

/* How far past a primitive's last u and v the model reads (texture xBR,
 * gl_picture.c: one texel without anti-aliasing, anywhere in the page with
 * it), told to the software GPU at each replay as gl_picture.c tells it. */
static int model_reach;

/* The texels u0..u1 x v0..v1 and, with model_reach, the ones past the last
 * u and v (wrapping in the page as texel_word does): their words, and the
 * palette entries they index. */
static void model_read(const Sampled *s)
{
    int page_x = s->page_x, page_y = s->page_y, depth = s->depth, clut_x = s->clut_x, clut_y = s->clut_y;
    int u0 = s->u0, v0 = s->v0, u1 = s->u1, v1 = s->v1, shift = 2 - depth, entries, nx = 0, ny = 0;
    int xs[257], ys[257];
    if (s->wx || u1 > 255 || model_reach == SOFT_GPU_REACH_PAGE) u0 = 0, u1 = 255;
    if (s->wy || v1 > 255 || model_reach == SOFT_GPU_REACH_PAGE) v0 = 0, v1 = 255;
    for (int x = u0 >> shift; x <= u1 >> shift; x++) xs[nx++] = page_x + x;
    for (int y = v0; y <= v1; y++) ys[ny++] = page_y + y;
    if (model_reach == 1 && u1 - u0 < 255) xs[nx++] = page_x + (((u1 + 1) & 255) >> shift);
    if (model_reach == 1 && v1 - v0 < 255) ys[ny++] = page_y + ((v1 + 1) & 255);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) read_copy(xs[i], ys[j]);
    if (depth >= 2) return;
    entries = depth == 0 ? 16 : 256;
    if (clut_x + entries > SOFT_GPU_WIDTH) {
        int greatest = 0;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                uint16_t word = copy_vram[(ys[j] & 511) * SOFT_GPU_WIDTH + (xs[i] & 1023)];
                greatest = word >> 8 > greatest ? word >> 8 : (word & 0xff) > greatest ? word & 0xff : greatest;
            }
        entries = greatest + 1;
    }
    for (int i = 0; i < entries; i++) read_copy(clut_x + i, clut_y);
}

static void model_sample(int page_x, int page_y, int depth, int clut_x, int clut_y, int u0, int v0, int u1, int v1,
                         int wx, int wy)
{
    const Sampled s = {page_x, page_y, depth, clut_x, clut_y, u0, v0, u1, v1, wx, wy};
    window_settle(); /* its lookups, as it is parsed */
    if (!gl_placement) {
        model_flush();
        model_read(&s);
        return;
    }
    if (pending_count == pending_room) {
        pending_room = pending_room ? pending_room * 2 : 1024;
        pending = realloc(pending, pending_room * sizeof(*pending));
        if (!pending) abort();
    }
    pending[pending_count++] = s;
}

/* flush_runs: the patches, then what the primitives gathered since sample. */
static void model_flush_runs(void)
{
    model_flush();
    for (size_t i = 0; i < pending_count; i++) model_read(&pending[i]);
    pending_count = 0;
}

/* A batch as gl_picture.c's gp0 reads it, with the state it leaves. */
static int model_fresh = 1; /* a run begins: the state as SoftGpu_Reset leaves it */
static void model_gp0(const uint32_t *words, size_t count)
{
    static int page_x, page_y, depth, bank, wx, wy;
    size_t at = 0;
    if (model_fresh) page_x = page_y = depth = bank = wx = wy = model_fresh = 0;
    while (at < count) {
        uint32_t word = words[at], command = word >> 24;
        size_t used = 1;
        if (command >= 0x20 && command < 0x40) {
            int quad = command & 8, textured = command & 4, shaded = command & 0x10, n = quad ? 4 : 3;
            int u0 = 255, v0 = 255, u1 = 0, v1 = 0, clut_x = 0, clut_y = 0;
            size_t k = at + 1;
            used = (size_t)n * (1 + (textured ? 1 : 0)) + (shaded ? (size_t)n : 1);
            if (count - at < used) break;
            for (int i = 0; i < n; i++) {
                if (i && shaded) k++;
                k++; /* the position */
                if (textured) {
                    uint32_t t = words[k++];
                    int u = t & 0xff, v = (t >> 8) & 0xff;
                    u0 = u < u0 ? u : u0, u1 = u > u1 ? u : u1, v0 = v < v0 ? v : v0, v1 = v > v1 ? v : v1;
                    if (i == 0) clut_x = (int)((t >> 16) & 0x3f) * 16, clut_y = (int)((t >> 22) & 0x1ff);
                    if (i == 1) {
                        uint32_t p = t >> 16;
                        page_x = (int)(p & 15) * 64, page_y = (int)((p >> 4) & 1) * 256, depth = (int)((p >> 7) & 3);
                        if (depth == 3) depth = 2;
                        bank = (int)((p >> 11) & 15) | (int)((p >> 10) & 1) << 4;
                    }
                }
            }
            if (textured && !(bank && SoftGpu_BankPixels(bank)))
                model_sample(page_x, page_y, depth, clut_x, clut_y, u0, v0, u1, v1, wx, wy);
        } else if (command >= 0x40 && command < 0x60) {
            int shaded = command & 0x10, poly = command & 8;
            size_t k = at + 2;
            for (;;) {
                if (poly && k < count && (words[k] & 0xf000f000u) == 0x50005000u) { k++; break; }
                if (shaded) k++;
                k++;
                if (!poly || k >= count) break;
            }
            if (k > count) break;
            used = k - at;
        } else if (command >= 0x60 && command < 0x80) {
            static const int sizes[4] = {0, 1, 8, 16};
            int textured = command & 4, kind = (command >> 3) & 3, w = sizes[kind], h = sizes[kind];
            used = 2 + (textured ? 1u : 0u) + (kind == 0 ? 1u : 0u);
            if (count - at < used) break;
            if (kind == 0) w = words[at + used - 1] & 0x3ff, h = (words[at + used - 1] >> 16) & 0x1ff;
            if (textured && w && h && !(bank && SoftGpu_BankPixels(bank))) {
                uint32_t t = words[at + 2];
                int u = t & 0xff, v = (t >> 8) & 0xff;
                model_sample(page_x, page_y, depth, (int)((t >> 16) & 0x3f) * 16, (int)((t >> 22) & 0x1ff), u, v,
                             u + w - 1, v + h - 1, wx, wy);
            }
        } else if (command == 0x02) {
            used = 3;
            if (count - at < 3) break;
            model_fill(words[at + 1] & 0x3f0, (words[at + 1] >> 16) & 0x1ff, ((words[at + 2] & 0x3ff) + 15) & ~15,
                       (words[at + 2] >> 16) & 0x1ff, word);
        } else if (command >= 0x80 && command < 0xa0) {
            used = 4;
            if (count - at < 4) break;
            model_move(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, words[at + 2] & 0x3ff,
                       (words[at + 2] >> 16) & 0x1ff, (int)((words[at + 3] - 1) & 0x3ff) + 1,
                       (int)(((words[at + 3] >> 16) - 1) & 0x1ff) + 1);
        } else if (command >= 0xa0 && command < 0xc0) {
            int w, h;
            if (count - at < 3) break;
            w = (int)((words[at + 2] - 1) & 0x3ff) + 1;
            h = (int)(((words[at + 2] >> 16) - 1) & 0x1ff) + 1;
            used = 3 + ((size_t)w * h + 1) / 2;
            if (count - at < used) break;
            model_load(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, w, h, (const uint16_t *)(words + at + 3));
        } else if (command >= 0xc0 && command < 0xe0) {
            used = 3;
        } else if (command == 0xe1) {
            page_x = (int)(word & 15) * 64, page_y = (int)((word >> 4) & 1) * 256, depth = (int)((word >> 7) & 3);
            if (depth == 3) depth = 2;
            bank = (int)((word >> 11) & 15);
        } else if (command == 0xe2) {
            wx = word & 0x1f, wy = (word >> 5) & 0x1f;
        }
        if (count - at < used) break;
        at += used;
    }
}

/* The take: VRAM raw into the copy, then SoftGpu_CopyTaken. */
static void model_take(void)
{
    memcpy(copy_vram, SoftGpu_VramRaw(), sizeof(copy_vram));
    SoftGpu_CopyTaken(&model);
}

/* GlPicture_Replay: the patches, the record in order (model_play), the take. */
static void model_play(void)
{
    size_t at = 0;
    uint64_t replay_reads = copy_reads; /* what was read before this replay */
    SoftGpu_SetCopyReach(model_reach); /* before the patches, as gl_picture.c */
    model_flush();
    while (at < ops_used) {
        const uint32_t *op = ops + at;
        switch (op[0]) {
        case 1: model_gp0(op + 2, op[1]); at += 2 + op[1]; break;
        case 2: model_load((int)op[1], (int)op[2], (int)op[3], (int)op[4], (const uint16_t *)(op + 5));
                at += 5 + ((size_t)op[3] * op[4] + 1) / 2; break;
        case 3: model_move((int)op[1], (int)op[2], (int)op[3], (int)op[4], (int)op[5], (int)op[6]); at += 7; break;
        case 4: model_fill((int)op[1], (int)op[2], (int)op[3], (int)op[4], op[5]); at += 6; break;
        default: /* a resync: VRAM settled whole, then taken */
            if (gl_placement) model_flush_runs();
            /* gl_picture.c's resync uploads VRAM whole and draws the picture
             * again from it: nothing the record read before it reaches the
             * picture or the copy (a scale change drops the copy's patches,
             * forget_copy, and the resync makes them needless). */
            copy_reads = replay_reads;
            SoftGpu_Settle(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
            model_take();
            at += 2;
            break;
        }
    }
    if (gl_placement) model_flush_runs(); /* the replay's last flush, before the take */
    ops_used = 0;
}

static void model_replay(void)
{
    model_play();
    window_settle(); /* after the last draw: its patch is not taken, and the take makes it needless */
    model_take();
}

/* The tick (record_point) clearing the first area whole, as the game's
 * clear does. */
static void tick_clear(void) { SoftGpu_Fill(0, 0, 320, 240, 0x00ff00); }

/* A fixed case at 2x with the model recorder, deferral `mode`, its copy
 * taken; copy_end replays what came since and returns what it read. */
static int copy_begin(int mode, int poison)
{
    SoftGpu_SetDeferred(0, 0);
    SoftGpu_SetRecorder(NULL);
    if (!SoftGpu_SetScale(1)) return 1;
    SoftGpu_Reset();
    SoftGpu_Fill(0, 0, 1024, 512, 0x800000);
    ops_used = 0;
    model_fresh = 1;
    copy_reads = 1469598103934665603ull;
    SoftGpu_SetRecorder(&model);
    if (!SoftGpu_SetScale(2)) return 1;
    model_replay();
    SoftGpu_SetDeferred(mode, poison);
    return 0;
}

static uint64_t copy_end(void)
{
    model_replay();
    SoftGpu_SetDeferred(0, 0);
    SoftGpu_SetScale(1);
    SoftGpu_SetRecorder(NULL);
    ops_used = 0;
    return copy_reads;
}

/* The run, from `start`, in the mode given. */
static int run(uint32_t start, int frames, int deferred, int poison, Outcome *out)
{
    static uint16_t words[1024 * 512];
    static uint32_t batch[1 << 16];
    const uint16_t *vram;
    size_t size;
    memset(out, 0, sizeof(*out));
    now_outcome = out;
    seed = start;
    tick_seed = start ^ 0x9e3779b9u;
    holds = releases = depth = nested = 0;
    SoftGpu_SetModVramUsers(0);
    SoftGpu_SetDeferred(0, 0);
    SoftGpu_Reset();
    memset(TextureDump_Tags, 0, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Tags));
    memset(TextureDump_Shadow, 0, (size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Shadow));
    memset(TextureDump_Clean, 0, sizeof(TextureDump_Clean));
    noise(words, 1024 * 512);
    SoftGpu_Load(0, 0, 1024, 512, words);
    noise(SoftGpu_Bank(3), 1024 * 512);
    if (copy_mode) { /* 2x, the model recorder; its first replay takes the copy */
        ops_used = 0;
        model_fresh = 1;
        copy_reads = 1469598103934665603ull;
        window_seed = start;
        SoftGpu_SetRecorder(&model);
        if (!SoftGpu_SetScale(2)) return 1;
        for (int i = 0; i < 2; i++) SoftGpu_PictureOnlyClut((int)(only_cluts[i] & 0x3f) * 16, (int)(only_cluts[i] >> 6));
        model_replay();
    }
    SoftGpu_SetDeferred(deferred, poison);
    for (int frame = 0, run_end = 0; frame < frames; frame++) {
        int area = frame & 1, x0 = area ? 320 : 0, y0 = 0, count = 0, busy;
        size_t at = 0;
        if (frame == run_end) { /* four or five: a run may start on either area */
            calm_run = below(4) != 0;
            run_end = frame + 4 + below(2);
        }
        calm = calm_run;
        if (copy_mode) { /* widescreen (whose targets the recorder draws), now and then a frame for the picture alone */
            SoftGpu_SetWidescreen(frame % 3 != 0);
            SoftGpu_PictureOnlyFrame(below(6) == 0);
        }
        if (frame % 97 == 50 || frame % 97 == 60) { /* a mod binds VRAM for a while: nothing is held */
            SoftGpuDeferredStats s;
            static unsigned long long held_then;
            SoftGpu_DeferredStats(&s);
            if (frame % 97 == 60) out->held_by_mod += (int)(s.held - held_then);
            held_then = s.held;
            SoftGpu_SetModVramUsers(frame % 97 == 50);
        }
        if (frame % 41 == 7) { /* a drawing area of another shape for a frame */
            x0 = 0;
            y0 = 256;
        }
        /* The drawing environment and the clear (twice, as the game does). */
        batch[at++] = 0xe3000000u | (uint32_t)y0 << 10 | (uint32_t)x0;
        batch[at++] = 0xe4000000u | (uint32_t)(y0 + 239) << 10 | (uint32_t)(x0 + 319);
        batch[at++] = 0xe5000000u | (uint32_t)y0 << 11 | (uint32_t)x0;
        batch[at++] = 0xe6000000u;
        SoftGpu_Gp0(batch, at);
        at = 0;
        if (below(8)) {
            SoftGpu_Fill(x0, y0, 320, 240, rnd() & 0xffffff);
            if (below(2)) SoftGpu_Fill(x0, y0, 320, 240, rnd() & 0xffffff);
        }
        /* One frame in five is busy, as a duel's is: a few hundred
         * primitives and state words, more texels than an area keeps apart. */
        busy = below(5) == 0;
        count = busy ? 150 + below(250) : 20 + below(80);
        for (int n = 0; n < count; n++) {
            int op = below(busy ? 74 : 100);
            if (at > (sizeof(batch) / sizeof(batch[0])) - 64) {
                SoftGpu_Gp0(batch, at);
                at = 0;
            }
            if (op < 70) {
                at += primitive(batch + at, x0, y0);
            } else if (op < 74) { /* state words in the batch */
                batch[at++] = 0xe1000000u | (page_word() & 0x7fff); /* bank 3 too (bits 11-14) */
                if (below(3) == 0) batch[at++] = 0xe2000000u | (rnd() & 0xfffff);
                if (below(4) == 0) batch[at++] = 0xe2000000u;
                if (below(5) == 0) batch[at++] = 0xe6000000u | (uint32_t)below(4);
                if (below(3) == 0) batch[at++] = 0xe6000000u;
            } else if (op < 76) { /* a fill inside the batch */
                batch[at++] = 0x02000000u | (rnd() & 0xffffff);
                batch[at++] = (uint32_t)below(512) << 16 | (uint32_t)(column(128) & ~15);
                batch[at++] = (uint32_t)(1 + below(64)) << 16 | (uint32_t)(1 + below(128));
            } else if (op < 78) { /* a move inside the batch */
                batch[at++] = 0x80000000u;
                batch[at++] = (uint32_t)below(512) << 16 | (uint32_t)column(32);
                batch[at++] = (uint32_t)below(512) << 16 | (uint32_t)column(32);
                batch[at++] = (uint32_t)(1 + below(32)) << 16 | (uint32_t)(1 + below(32));
            } else {
                /* Outside the batch: what is drawn so far is drawn first,
                 * as libgpu.c flushes before any transfer. */
                SoftGpu_Gp0(batch, at);
                at = 0;
                /* The picture-only switch, set before each flush (libgpu.c),
                 * may change while work is held. */
                if (copy_mode && below(4) == 0) SoftGpu_PictureOnlyFrame(below(3) == 0);
                if (!calm && y0 == 0) {
                    SoftGpuDeferredStats s;
                    SoftGpu_DeferredStats(&s);
                    for (int q = 0; q < SOFT_GPU_QUEUES; q++)
                        if (s.queued[q] && s.queue_y[q] == 0 && s.queue_x[q] == (area ? 0 : 320))
                            out->over_held[!area]++;
                }
                if (op < 80) {
                    int w = 1 + below(64), h = 1 + below(40);
                    noise(words, w * h);
                    SoftGpu_Load(column(w), below(512), w, h, words);
                } else if (op < 82) {
                    /* The same texels uploaded again, as the game does every
                     * frame, now and then with one word changed, under
                     * whatever mask state the last batch left; first as a
                     * move onto itself. */
                    int w = 1 + below(64), h = 1 + below(40), x = column(w), y = below(512);
                    SoftGpu_Move(x, y, x, y, w, h);
                    SoftGpu_Store(x, y, w, h, words);
                    if (below(4) == 0) words[below(w * h)] ^= (uint16_t)(1 + below(0xffff));
                    SoftGpu_Load(x, y, w, h, words);
                } else if (op < 85) {
                    int w = 1 + below(64), h = 1 + below(40), x = column(w), y = below(512);
                    SoftGpu_Store(x, y, w, h, words);
                    out->stores = fnv(out->stores, words, (size_t)w * h * 2);
                } else if (op < 88) {
                    if (below(3) == 0) SoftGpu_Move(0, 0, 0, 0, 2, 1); /* the game's own, every frame */
                    else SoftGpu_Move(column(48), below(512), column(48), below(512), 1 + below(48), 1 + below(48));
                } else if (op < 90) {
                    SoftGpu_Fill(column(128) & ~15, below(512), 16 * (1 + below(8)), 1 + below(64), rnd() & 0xffffff);
                } else if (op < 93) { /* a host lookup settles a rectangle, then reads it */
                    int w = 1 + below(64), h = 1 + below(64), x = column(w), y = below(512);
                    SoftGpu_Settle(x, y, w, h);
                    vram = SoftGpu_VramRaw();
                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++)
                            out->stores = fnv(out->stores, &vram[((y + j) & 511) * 1024 + ((x + i) & 1023)], 2);
                } else if (op < 94 && below(2)) {
                    /* A bank's writer (a mod's, a palette copied in each
                     * frame): bank 3's texels or palette rows, the same
                     * words mostly, now and then changed; or the shadow's
                     * cells on its page, repainted alike. */
                    int w = 1 + below(32), h = 1 + below(16), x = 640 + below(384 - w), y = below(2) ? 480 + below(32 - h) : below(512 - h);
                    int change = below(4) == 0, shadow = below(3) == 0;
                    uint16_t *bank = SoftGpu_Bank(3);
                    if (shadow) x = 640 + below(64 - (w < 64 ? w : 63)), y = 256 + below(256 - h);
                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++) {
                            uint16_t *at = shadow ? TextureDump_Cell(x + i, y + j, below(4)) : &bank[(y + j) * SOFT_GPU_WIDTH + x + i];
                            *at = change ? (uint16_t)(rnd() | (shadow ? 0x8000 : 0)) : *at;
                        }
                } else if (op < 95) { /* the pack tags and paints some words */
                    int w = 1 + below(64), h = 1 + below(16), x = calm ? column(w) : below(1024 - w), y = below(512 - h);
                    for (int j = 0; j < h; j++)
                        for (int i = 0; i < w; i++) {
                            TextureDump_Tags[(y + j) * SOFT_GPU_WIDTH + x + i] = rnd() | 1;
                            *TextureDump_Cell(x + i, y + j, below(4)) = (uint16_t)(rnd() | 0x8000);
                        }
                    TextureDump_Touched(x, y, w, h);
                } else if (op < 97 && !calm) { /* a save state */
                    SoftGpuDeferredStats s;
                    SoftGpu_DeferredStats(&s);
                    if (s.held > s.dropped + s.rastered) out->states_held++;
                    vram = SoftGpu_StateData(0, &size);
                    out->stores = fnv(out->stores, vram, size);
                    vram = SoftGpu_StateData(1, &size); /* size first: not in the same call */
                    out->stores = fnv(out->stores, vram, size);
                } else {
                    SoftGpu_SettleTexture(below(3), column(256) & ~63, below(2) * 256, column(256) & ~15, below(512));
                }
            }
        }
        SoftGpu_Gp0(batch, at);
        if (copy_mode) { /* the OpenGL picture's present settles nothing; most frames replay */
            if (below(4)) {
                model_replay();
                out->replays++;
            }
        } else if (below(4) == 0) {
            /* A present, as the window at the console's resolution does: the
             * area just drawn, settled and read, with its tags; it sees what
             * a raster gone wrong left there before the next clear hides it. */
            vram = (SoftGpu_Settle(x0, y0, 320, 240), SoftGpu_VramRaw());
            for (int j = 0; j < 240; j++) {
                out->stores = fnv(out->stores, vram + (y0 + j) * SOFT_GPU_WIDTH + x0, 320 * 2);
                out->stores = fnv(out->stores, TextureDump_Tags + (y0 + j) * SOFT_GPU_WIDTH + x0, 320 * 4);
            }
        }
        if (frame % 211 == 100) { /* the scale changes and back: everything settles */
            if (!SoftGpu_SetScale(copy_mode ? 4 : 2) || !SoftGpu_SetScale(copy_mode ? 2 : 1)) return 1;
        }
    }
    if (copy_mode) {
        model_replay();
        out->reads = copy_reads;
        SoftGpu_PictureOnlyFrame(0);
        SoftGpu_SetWidescreen(0);
        if (!SoftGpu_SetScale(1)) return 1;
        SoftGpu_SetRecorder(NULL);
        ops_used = 0;
    }
    vram = SoftGpu_Vram();
    SoftGpu_DeferredStats(&out->stats);
    out->vram = fnv(1469598103934665603ull, vram, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * 2);
    out->tags = fnv(1469598103934665603ull, TextureDump_Tags, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * 4);
    out->shadow = fnv(1469598103934665603ull, TextureDump_Shadow, (size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT * 2);
    vram = SoftGpu_StateData(1, &size);
    out->gpu = fnv(1469598103934665603ull, vram, size);
    SoftGpu_SetDeferred(0, 0);
    return 0;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    Outcome now, held, poisoned;
    SoftGpu_HoldClock = hold_clock;
    SoftGpu_InTick = test_in_tick;
#ifdef SOFT_GPU_TEST_WRAP_ALLOC
    { /* the count works: an allocation in the tick is seen (else the checks below would pass for nothing) */
        void *volatile made; /* volatile: else the compiler may leave out the pair */
        in_tick = 1;
        made = malloc(16);
        in_tick = 0;
        free(made);
        CHECK(tick_allocs == 1);
        tick_allocs = 0;
    }
#endif
    if (!TextureDump_EnableShadow()) {
        fprintf(stderr, "no memory for the tags\n");
        return 1;
    }
    /* First of all, as the patches' buffer is made once: the clock's tick
     * (a StoreImage there) settles held work the copy missed, which makes a
     * patch, and nothing is allocated in it. */
    {
        static const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        static const uint32_t red[] = {0x600000ffu, 0x00100004u, 0x00080008u}; /* 8x8 at 4,16 */
        uint16_t word = 0;
        SoftGpuDeferredStats s;
        window_settles = 0;
        CHECK(copy_begin(SOFT_GPU_DEFERRED_AUTO, 0) == 0);
        CHECK(SoftGpu_Gp0(area, 4) == 4);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(red, 3) == 3);
        model_replay(); /* the take, the sprite held across it */
        in_tick = 1;
        SoftGpu_Store(4, 16, 1, 1, &word);
        in_tick = 0;
        SoftGpu_DeferredStats(&s);
        CHECK(s.held == 1 && s.patches == 1 && word == 0x001f && tick_allocs == 0);
        (void)copy_end();
        window_settles = 1;
    }
    TextureDump_Prepare = prepare;
    for (uint32_t start = 1; start <= 3; start++) {
        CHECK(run(start * 2654435761u, frames, 0, 0, &now) == 0);
        CHECK(holds == 0); /* off: the clock is never held */
        CHECK(run(start * 2654435761u, frames, 1, 0, &held) == 0);
        CHECK(holds > 0 && holds == releases && depth == 0 && !nested);
        CHECK(run(start * 2654435761u, frames, 1, 1, &poisoned) == 0);
        CHECK(holds > 0 && holds == releases && depth == 0 && !nested);
        printf("run %u: held %llu (dropped %llu, rastered %llu), at once %llu, states while held %d, "
               "busy over held 0,0 %d, 320,0 %d\n", start,
               (unsigned long long)held.stats.held, (unsigned long long)held.stats.dropped,
               (unsigned long long)held.stats.rastered, (unsigned long long)held.stats.immediate, held.states_held,
               held.over_held[0], held.over_held[1]);
        CHECK(held.stats.held > (unsigned)frames && held.stats.dropped > (unsigned)frames / 40 && held.stats.rastered > (unsigned)frames);
        CHECK(held.stats.held == held.stats.dropped + held.stats.rastered);
        CHECK(held.states_held > 0);
        CHECK(held.over_held[0] > 0 && held.over_held[1] > 0); /* both areas' held work met a busy frame */
        CHECK(held.held_by_mod == 0 && poisoned.held_by_mod == 0);
        /* Every rule was met but the panel's and OTHER's (memory, 4M words,
         * widescreen: the fixed cases below take the last two; a mod's VRAM
         * or a scale raster at once without a look), and a bank's and the
         * shadow's, which are held now (no memory for a frozen copy: a fixed
         * case below). Held, and some held work settled for a change in
         * what it read of them (why WRITE counts loads too: see below). */
        for (int why = 0; why < SOFT_GPU_NOW_COUNT; why++)
            if (why != SOFT_GPU_NOW_PANEL && why != SOFT_GPU_NOW_OTHER && why != SOFT_GPU_NOW_BANK &&
                why != SOFT_GPU_NOW_SHADOW)
                CHECK(held.stats.now[why] > 0);
        CHECK(held.stats.now[SOFT_GPU_NOW_BANK] == 0 && held.stats.now[SOFT_GPU_NOW_SHADOW] == 0);
        CHECK(now.stores == held.stores && now.stores == poisoned.stores);
        CHECK(now.vram == held.vram && now.vram == poisoned.vram);
        CHECK(now.tags == held.tags && now.tags == poisoned.tags);
        CHECK(now.shadow == held.shadow && now.shadow == poisoned.shadow);
        CHECK(now.gpu == held.gpu && now.gpu == poisoned.gpu);
    }

    /* At 2x with a recorder (the OpenGL picture) and its copy of VRAM: kept
     * back by AUTO (as the game runs it by default) or not, with poison or
     * not, the copy's words its replays read, and everything else, are the
     * same. Held work must have outlived takes of the copy, and patches been
     * made, or this proves nothing. */
    copy_mode = 1;
    for (uint32_t start = 1; start <= 4; start++) {
        Outcome off, automatic, poisoned_auto;
        gl_placement = start > 2; /* the last two as gl_picture.c reads and patches */
        /* Texture xBR's reach: off, without anti-aliasing (one texel), with it */
        model_reach = start == 2 || start == 3 ? 1 : start == 4 ? SOFT_GPU_REACH_PAGE : 0;
        CHECK(run(start * 2246822519u, frames, 0, 0, &off) == 0);
        CHECK(holds == 0);
        CHECK(run(start * 2246822519u, frames, SOFT_GPU_DEFERRED_AUTO, 0, &automatic) == 0);
        CHECK(holds > 0 && holds == releases && depth == 0 && !nested);
        CHECK(run(start * 2246822519u, frames, SOFT_GPU_DEFERRED_AUTO, 1, &poisoned_auto) == 0);
        CHECK(holds > 0 && holds == releases && depth == 0 && !nested);
        printf("copy run %u%s, reach %d: held %llu (dropped %llu, rastered %llu), at once %llu, %d replays, the copy missed %llu "
               "areas, %llu patches\n", start, gl_placement ? " (flush_runs)" : "", model_reach,
               (unsigned long long)automatic.stats.held,
               (unsigned long long)automatic.stats.dropped, (unsigned long long)automatic.stats.rastered,
               (unsigned long long)automatic.stats.immediate, automatic.replays,
               (unsigned long long)automatic.stats.copy_missed, (unsigned long long)automatic.stats.patches);
        CHECK(automatic.stats.held > (unsigned)frames && automatic.stats.dropped > (unsigned)frames / 40);
        CHECK(automatic.stats.copy_missed > (unsigned)frames / 10 && automatic.stats.patches > (unsigned)frames / 40);
        CHECK(off.reads == automatic.reads && off.reads == poisoned_auto.reads);
        CHECK(off.stores == automatic.stores && off.stores == poisoned_auto.stores);
        CHECK(off.vram == automatic.vram && off.vram == poisoned_auto.vram);
        CHECK(off.tags == automatic.tags && off.tags == poisoned_auto.tags);
        CHECK(off.shadow == automatic.shadow && off.shadow == poisoned_auto.shadow);
        CHECK(off.gpu == automatic.gpu && off.gpu == poisoned_auto.gpu);
    }
    copy_mode = 0;
    gl_placement = 0;
    model_reach = 0;
    CHECK(tick_allocs == 0); /* the tick's loads and stores, settles and patches included */

    /* The fixed cases below, without the pack's prepare (and its tick). */
    TextureDump_Prepare = NULL;

    /* A sprite whose texture coordinates run past 255 wraps to the page's
     * first texels: a 16-bit sprite from page 9 (576,0) at u 250, 20 wide,
     * reads 826-831 and then 576-589, inside the area at 320,0 whose
     * drawing is held. It must see that drawing, as when it is rastered at
     * once. */
    {
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t right[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                  0xe6000000u};
        const uint32_t white[] = {0x60ffffffu, 0x00000100u, 0x00100100u}; /* 256x16 at 256,0 of the area */
        const uint32_t page[] = {0xe1000000u | 9 | (2u << 7)};               /* page 9, 16-bit */
        const uint32_t sprite[] = {0x65808080u, 0x00000000u, 0x000000fau, 0x00040014u}; /* u 250, 20x4 at 0,0 */
        uint16_t first[20 * 4], then[20 * 4];
        for (int deferred = 0; deferred < 2; deferred++) {
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(right, 4) == 4 && SoftGpu_Gp0(white, 3) == 3);
            CHECK(SoftGpu_Gp0(left, 4) == 4 && SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4);
            SoftGpu_Store(0, 0, 20, 4, deferred ? then : first);
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[19] == 0x7fff); /* u 13: 589, white */
        SoftGpu_SetDeferred(0, 0);
    }

    /* More texels apart than an area keeps: 60 one-texel sprites from 60
     * places of the 16-bit page at 640,0, then the last one's texel
     * uploaded anew. The area keeps the last ones merged into others, and
     * the upload must still raster them first. */
    {
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u,
                                 0xe1000000u | 10 | (2u << 7)};
        uint32_t sprites[60 * 4];
        uint16_t first[60], then[60], texel = 0x1234;
        for (int i = 0; i < 60; i++) {
            sprites[i * 4] = 0x6d808080u;                          /* raw textured 1x1 */
            sprites[i * 4 + 1] = (uint32_t)(i * 4);                /* at 4i,0 */
            sprites[i * 4 + 2] = (uint32_t)(i << 8 | (i * 3));    /* u 3i, v i */
            sprites[i * 4 + 3] = 0;                               /* (no size word: 1x1) */
        }
        for (int deferred = 0; deferred < 2; deferred++) {
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0x0000ff);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(left, 5) == 5);
            for (int i = 0; i < 60; i++) CHECK(SoftGpu_Gp0(sprites + i * 4, 3) == 3);
            SoftGpu_Load(640 + 59 * 3, 59, 1, 1, &texel);
            for (int i = 0; i < 60; i++) SoftGpu_Store(i * 4, 0, 1, 1, (deferred ? then : first) + i);
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[59] == 0x001f);
        SoftGpu_SetDeferred(0, 0);
    }

    /* The texels a polygon reads are within its vertices' coordinates
     * (texture_rects): every texel of the 16-bit page at 768,256 outside
     * them is uploaded anew after it is held (and half the time the
     * greatest u's column and v's row too, which it reads), and it must
     * come out as when it is rastered at once. Mirrored quads read their greatest u and v at
     * their first pixels, turned ones (a model's faces) anywhere between;
     * slivers with their texture spread across them
     * overflow the raster's steps and read anywhere in the page. */
    {
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        static uint16_t page[256 * 256], first[320 * 240], then[320 * 240];
        int differ = 0;
        seed = 77;
        for (int n = 0; n < 900; n++) {
            uint32_t quad[9];
            int u[4], v[4], x[4], y[4], uv[4] = {255, 255, 0, 0}, kind = n % 3;
            for (int k = 0; k < 4; k++) {
                u[k] = below(256);
                v[k] = below(256);
            }
            if (kind == 2) { /* turned, as a model's faces are: a rectangle of texels on a turned rectangle */
                /* Turned by a Pythagorean triple's angle, either way: cos a/r, sin b/r. */
                static const int triples[6][3] = {{3, 4, 5}, {5, 12, 13}, {8, 15, 17}, {7, 24, 25}, {20, 21, 29}, {1, 0, 1}};
                const int *t = triples[below(6)];
                int c = below(2) ? t[0] : t[1], sn = (c == t[0] ? t[1] : t[0]) * (below(2) ? 1 : -1), r = t[2];
                int hw = 2 + below(40), hh = 2 + below(40), du = below(80), dv = below(80);
                int u0 = below(256 - du), v0 = below(256 - dv);
                for (int k = 0; k < 4; k++) {
                    int sx = k & 1 ? hw : -hw, sy = k & 2 ? hh : -hh;
                    x[k] = 150 + (sx * c - sy * sn) / r;
                    y[k] = 110 + (sx * sn + sy * c) / r;
                    u[k] = u0 + (k & 1 ? du : 0);
                    v[k] = v0 + (k & 2 ? dv : 0);
                }
            } else if (!kind) { /* mirrored: u and v fall as x and y grow */
                int w = 2 + below(40), h = 2 + below(40), u0 = w + below(256 - w), v0 = h + below(256 - h);
                x[0] = x[2] = 0, x[1] = x[3] = w, y[0] = y[1] = 0, y[2] = y[3] = h;
                u[0] = u[2] = u0, u[1] = u[3] = u0 - w, v[0] = v[1] = v0, v[2] = v[3] = v0 - h;
            } else {
                for (int k = 0; k < 4; k++) {
                    x[k] = k < 2 ? below(300) : x[k - 2] + below(3) - 1;
                    y[k] = k < 2 ? below(200) : y[k - 2] + below(3) - 1;
                }
            }
            for (int k = 0; k < 4; k++) {
                uv[0] = u[k] < uv[0] ? u[k] : uv[0], uv[2] = u[k] > uv[2] ? u[k] : uv[2];
                uv[1] = v[k] < uv[1] ? v[k] : uv[1], uv[3] = v[k] > uv[3] ? v[k] : uv[3];
                quad[1 + k * 2] = ((uint32_t)y[k] & 0x7ff) << 16 | ((uint32_t)x[k] & 0x7ff);
                quad[2 + k * 2] = (uint32_t)(v[k] << 8 | u[k]) |
                                  (k == 1 ? (uint32_t)(12 | 16 | (2u << 7)) << 16 : 0); /* page 768,256, 16-bit */
            }
            quad[0] = 0x2d808080u; /* raw textured quad */
            for (int deferred = 0; deferred < 2; deferred++) {
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 320, 240, 0);
                for (int i = 0; i < 256 * 256; i++) page[i] = (uint16_t)(i * 2654435761u >> 16) | 1;
                SoftGpu_Load(768, 256, 256, 256, page);
                SoftGpu_SetDeferred(deferred, 0);
                CHECK(SoftGpu_Gp0(left, 4) == 4 && SoftGpu_Gp0(quad, 9) == 9);
                /* Everything outside uv[0..2] x uv[1..3], in four bands. */
                for (int i = 0; i < 256 * 256; i++) page[i] = (uint16_t)(i * 40503u) | 1;
                if (uv[1] > 0) SoftGpu_Load(768, 256, 256, uv[1], page);
                if (uv[3] < 255) SoftGpu_Load(768, 256 + uv[3] + 1, 256, 255 - uv[3], page);
                if (uv[0] > 0) SoftGpu_Load(768, 256 + uv[1], uv[0], uv[3] - uv[1] + 1, page);
                if (uv[2] < 255) SoftGpu_Load(768 + uv[2] + 1, 256 + uv[1], 255 - uv[2], uv[3] - uv[1] + 1, page);
                if (n % 4 < 2) { /* and, half the time, the greatest u's column and v's row, which are read */
                    SoftGpu_Load(768 + uv[2], 256 + uv[1], 1, uv[3] - uv[1] + 1, page + 7);
                    SoftGpu_Load(768 + uv[0], 256 + uv[3], uv[2] - uv[0] + 1, 1, page + 11);
                }
                SoftGpu_Store(0, 0, 320, 240, deferred ? then : first);
            }
            differ |= memcmp(first, then, sizeof(first)) != 0;
        }
        CHECK(!differ);
        SoftGpu_SetDeferred(0, 0);
    }

    /* 48 one-texel sprites from the strip below the buffers (16-bit page
     * 0,256), then one from 700,10, which no strip rectangle can take in
     * without reaching into a buffer: two strip ones are merged to make
     * room. Each strip texel uploaded anew, one run each, must still raster
     * the sprites first. */
    {
        const uint32_t right[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                  0xe6000000u, 0xe1000000u | 0 | 16 | (2u << 7)}; /* page 0,256, 16-bit */
        const uint32_t far[] = {0xe1000000u | 10 | (2u << 7), 0x6d808080u, (uint32_t)(60 << 16), (10 << 8) | 60};
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t dot[] = {0x60ffffffu, 0x00640064u, 0x00010001u};
        uint32_t sprites[48 * 3];
        uint16_t first[64], then[64], texel = 0x1234;
        for (int i = 0; i < 48; i++) {
            sprites[i * 3] = 0x6d808080u;                       /* raw textured 1x1 */
            sprites[i * 3 + 1] = (uint32_t)i;                   /* at i,0 of the area */
            sprites[i * 3 + 2] = (uint32_t)(i << 8 | (i * 5));  /* u 5i, v i */
        }
        for (int t = 0; t < 48; t++) {
            for (int deferred = 0; deferred < 2; deferred++) {
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 1024, 512, 0x0000ff);
                SoftGpu_SetDeferred(deferred, 0);
                SoftGpu_Fill(0, 0, 320, 240, 0);
                CHECK(SoftGpu_Gp0(left, 4) == 4 && SoftGpu_Gp0(dot, 3) == 3); /* the first buffer holds too */
                CHECK(SoftGpu_Gp0(right, 5) == 5);
                for (int i = 0; i < 48; i++) CHECK(SoftGpu_Gp0(sprites + i * 3, 3) == 3);
                CHECK(SoftGpu_Gp0(far, 4) == 4);
                SoftGpu_Load(t * 5, 256 + t, 1, 1, &texel);
                SoftGpu_Store(320, 0, 64, 1, deferred ? then : first);
            }
            CHECK(!memcmp(first, then, sizeof(first)) && first[t] == 0x001f);
        }
        SoftGpu_SetDeferred(0, 0);
    }

    /* An 8-bit palette at 896 runs past VRAM's edge into the first buffer's
     * columns: a sprite whose texels hold index 140 reads 12,100, which is
     * uploaded anew while it is held in the other buffer. */
    {
        const uint32_t right[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                  0xe6000000u, 0xe1000000u | 12 | 16 | (1u << 7)}; /* page 768,256, 8-bit */
        const uint32_t sprite[] = {0x65808080u, 0x00000000u, (uint32_t)((100 << 6) | (896 / 16)) << 16, 0x00040004u};
        static uint16_t texels[64 * 64], palette[128], first[16], then[16];
        uint16_t entry = 0x1111;
        for (int i = 0; i < 64 * 64; i++) texels[i] = 140 | (140 << 8);
        for (int i = 0; i < 128; i++) palette[i] = (uint16_t)(0x0400 + i);
        for (int deferred = 0; deferred < 2; deferred++) {
            uint16_t wrapped[13];
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0);
            SoftGpu_Load(768, 256, 64, 64, texels);
            SoftGpu_Load(896, 100, 128, 1, palette);
            for (int i = 0; i < 13; i++) wrapped[i] = (uint16_t)(0x2000 + i);
            SoftGpu_Load(0, 100, 13, 1, wrapped);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(right, 5) == 5 && SoftGpu_Gp0(sprite, 4) == 4);
            SoftGpu_Load(12, 100, 1, 1, &entry);
            SoftGpu_Store(320, 0, 4, 4, deferred ? then : first);
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[0] == 0x200c);
        SoftGpu_SetDeferred(0, 0);
    }

    /* An area the life-point panel made go at once stays given up past its
     * next clear (twice, as the game clears) while each cycle meets it, and
     * holds again after a cycle that met none; one given up for a texture in
     * its own area (any reason but the panel, or a bank or the shadow with no
     * memory for its frozen copy) holds again after its next clear. */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t plain[] = {0x60ffffffu, 0x00100010u, 0x00040004u}; /* white, 4x4 at 16,16 */
        /* A raw textured triangle from the panel's page (704,0, 4-bit, palette 736,252). */
        const uint32_t banked[] = {0x25808080u, 0x00000000u, (uint32_t)((252 << 6) | 46) << 16, 0x00000010u,
                                   11u << 16 | 0x0010u, 0x00100000u, 0x1000u};
        SoftGpu_PanelName = no_name;
        const uint32_t own[] = {0xe1000000u | (2u << 7), 0x65808080u, 0x00200020u, 0x00000000u,
                                0x00040004u}; /* 16-bit page 0: the area's own words */
        static const unsigned long long held_after[7] = {1, 1, 1, 2, 3, 4, 5};
        SoftGpu_Reset();
        SoftGpu_SetDeferred(1, 0);
        CHECK(SoftGpu_Gp0(area, 4) == 4);
        for (int cycle = 0; cycle < 7; cycle++) {
            SoftGpuDeferredStats s;
            int before = holds;
            SoftGpu_Fill(0, 0, 320, 240, 0);
            /* Cycles 1-4 start given up with nothing held: the clear changes
             * given_up[] with the clock held all the same (the tick's fills
             * change it too). */
            CHECK(cycle < 1 || cycle > 4 || holds > before);
            SoftGpu_Fill(0, 0, 320, 240, 0);
            CHECK(SoftGpu_Gp0(plain, 3) == 3);
            if (cycle < 2) CHECK(SoftGpu_Gp0(banked, 7) == 7);
            if (cycle == 5) CHECK(SoftGpu_Gp0(own, 5) == 5 && SoftGpu_Gp0(plain, 3) == 3); /* given up: not held */
            SoftGpu_DeferredStats(&s);
            /* Cycle 0 holds its sprite; 1 (after the panel) and 2 (after
             * the panel again) hold nothing; 3 (after a cycle without), 4, 5 and
             * 6 (after a cycle given up for its own texture) do. */
            CHECK(s.held == held_after[cycle]);
        }
        {
            SoftGpuDeferredStats s;
            SoftGpu_DeferredStats(&s);
            CHECK(s.now[SOFT_GPU_NOW_TEXTURE] == 1 && s.now[SOFT_GPU_NOW_AFTER] == 3);
        }
        /* A state load (SoftGpu_PictureFromVram) or a reset forgets what was
         * given up: the next primitive holds, with no clear first. */
        for (int how = 0; how < 2; how++) {
            SoftGpuDeferredStats s;
            unsigned long long held_before;
            CHECK(SoftGpu_Gp0(banked, 7) == 7 && SoftGpu_Gp0(plain, 3) == 3); /* given up: not held */
            SoftGpu_DeferredStats(&s);
            held_before = s.held;
            if (how) {
                SoftGpu_Reset();
                CHECK(SoftGpu_Gp0(area, 4) == 4);
            } else {
                SoftGpu_PictureFromVram();
            }
            CHECK(SoftGpu_Gp0(plain, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == held_before + 1);
        }
        SoftGpu_SetDeferred(0, 0);
        SoftGpu_PanelName = NULL;
    }

    /* The same texels uploaded again with the mask bit set (E6 bit 0) are
     * not the same: zeros become 0x8000, which a sprite draws as black where
     * 0 was transparent. A held sprite reading them must be rastered before
     * the upload, as it was drawn before it. */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u,
                                 0xe1000000u | 10 | (2u << 7)}; /* page 10 (640,0), 16-bit */
        const uint32_t sprite[] = {0x65808080u, 0x00100010u, 0x00000000u, 0x00040004u}; /* 4x4 at 16,16 */
        const uint32_t mask_set[] = {0xe6000001u};
        const uint16_t zeros[16] = {0};
        uint16_t first[16], then[16];
        for (int deferred = 0; deferred < 2; deferred++) {
            SoftGpuDeferredStats s;
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0xffffff);
            SoftGpu_Load(640, 0, 4, 4, zeros);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(area, 5) == 5 && SoftGpu_Gp0(sprite, 4) == 4 && SoftGpu_Gp0(mask_set, 1) == 1);
            SoftGpu_DeferredStats(&s);
            CHECK(!deferred || s.held == 1); /* the sprite is held */
            SoftGpu_Load(640, 0, 4, 4, zeros);
            SoftGpu_Store(16, 16, 4, 4, deferred ? then : first);
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[0] == 0x7fff);
        SoftGpu_SetDeferred(0, 0);
    }

    /* An area that starts holding where another held area samples: the
     * other is rastered first. The area at 320,0 holds a sprite reading
     * 0..3,0..3, in the area at 0,0, which then holds a white sprite over
     * those words with no clear before; that one is settled first, then the
     * first: it must read the words as they were. */
    {
        const uint32_t right[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                  0xe6000000u, 0xe1000000u | (2u << 7)};          /* page 0, 16-bit */
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t reader[] = {0x65808080u, 0x00000000u, 0x00000000u, 0x00040004u}; /* u, v 0: 4x4 at 0,0 */
        const uint32_t white[] = {0x60ffffffu, 0x00000000u, 0x00040004u};
        uint16_t first[16], then[16], pattern[16];
        for (int i = 0; i < 16; i++) pattern[i] = (uint16_t)(0x0421 * (i + 1));
        for (int deferred = 0; deferred < 2; deferred++) {
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0);
            SoftGpu_Load(0, 0, 4, 4, pattern);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(right, 5) == 5 && SoftGpu_Gp0(reader, 4) == 4);
            CHECK(SoftGpu_Gp0(left, 4) == 4 && SoftGpu_Gp0(white, 3) == 3);
            SoftGpu_Settle(0, 0, 4, 4);
            SoftGpu_Store(320, 0, 4, 4, deferred ? then : first);
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[5] == pattern[5]);
        SoftGpu_SetDeferred(0, 0);
    }

    /* The pack's tags. Painted over held work after it is held, they stay:
     * the primitive drew (and cleared the tags under it) before they were
     * painted, so its replay clears none. Painted in the area before a
     * primitive comes, they make it go at once, and it clears them as it
     * draws. */
    {
        const uint32_t left[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t white[] = {0x60ffffffu, 0x00100010u, 0x00040004u}; /* 4x4 at 16,16 */
        uint32_t first[2][16], then[2][16];
        for (int deferred = 0; deferred < 2; deferred++) {
            for (int before = 0; before < 2; before++) {
                SoftGpuDeferredStats s;
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                memset(TextureDump_Tags, 0, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Tags));
                SoftGpu_SetDeferred(deferred, 0);
                CHECK(SoftGpu_Gp0(left, 4) == 4);
                SoftGpu_Fill(0, 0, 320, 240, 0); /* its blocks clean */
                for (int pass = 0; pass < 2; pass++) {
                    if (pass == 1 - before) { /* the pack tags the sprite's words */
                        for (int j = 0; j < 4; j++)
                            for (int i = 0; i < 4; i++) TextureDump_Tags[(16 + j) * SOFT_GPU_WIDTH + 16 + i] = 0x1234567u;
                        TextureDump_Touched(16, 16, 4, 4);
                    } else {
                        CHECK(SoftGpu_Gp0(white, 3) == 3);
                    }
                }
                SoftGpu_DeferredStats(&s);
                if (deferred) CHECK(before ? s.held == 0 && s.now[SOFT_GPU_NOW_TAGS] == 1 : s.held == 1);
                SoftGpu_Settle(0, 0, 320, 240);
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++)
                        (deferred ? then : first)[before][j * 4 + i] = TextureDump_Tags[(16 + j) * SOFT_GPU_WIDTH + 16 + i];
            }
        }
        CHECK(!memcmp(first, then, sizeof(first)) && first[0][0] == 0x1234567u && first[1][0] == 0);
        SoftGpu_SetDeferred(0, 0);
    }

    /* Widescreen with deferral on, at 1x: a full-screen area has a target
     * beside it that the primitive is drawn into a second time, so it is
     * rastered at once (SOFT_GPU_NOW_OTHER), and the target and VRAM are what
     * rastering it as it comes makes. With widescreen off the same two
     * primitives are held (the control: the case could not tell otherwise). */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t sprite[] = {0x60ffffffu, 0x00100010u, 0x00040004u};  /* white, 4x4 at 16,16 */
        const uint32_t quad[] = {0x28ffffffu, 0x000a07ecu, 0x000a000au, 0x000c07ecu, 0x000c000au}; /* from x -20 */
        uint64_t seen[2][2][2]; /* [wide][deferred][VRAM, the widened picture] */
        for (int wide = 0; wide < 2; wide++) {
            for (int deferred = 0; deferred < 2; deferred++) {
                SoftGpuDeferredStats s;
                const uint16_t *pixels;
                int x, w;
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_SetWidescreen(wide);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 1024, 512, 0);
                SoftGpu_SetDeferred(deferred, 1);
                CHECK(SoftGpu_Gp0(area, 4) == 4);
                SoftGpu_Fill(0, 0, 320, 240, 0);
                CHECK(SoftGpu_Gp0(sprite, 3) == 3 && SoftGpu_Gp0(quad, 5) == 5);
                SoftGpu_DeferredStats(&s);
                if (deferred && wide) {
                    CHECK(s.held == 0 && s.immediate == 2 && s.now[SOFT_GPU_NOW_OTHER] == 2);
                } else if (deferred) {
                    CHECK(s.held == 2 && s.immediate == 0 && s.now[SOFT_GPU_NOW_OTHER] == 0);
                }
                /* Before any settle: rastered at once, or magenta (the poison). */
                CHECK(SoftGpu_VramRaw()[17 * SOFT_GPU_WIDTH + 17] == (deferred && !wide ? 0x7c1f : 0x7fff));
                CHECK(SoftGpu_VramRaw()[11 * SOFT_GPU_WIDTH + 5] == (deferred && !wide ? 0x7c1f : 0x7fff));
                SoftGpu_Settle(0, 0, 320, 240);
                seen[wide][deferred][0] = fnv(1469598103934665603ull, SoftGpu_Vram(), (size_t)SOFT_GPU_WIDTH * 512 * 2);
                seen[wide][deferred][1] = 0;
                if (wide) {
                    CHECK(SoftGpu_WideFrame(0, 0, 320, 240, &pixels, &x, &w) && x == 0 && w == 428);
                    for (int row = 0; row < 240; row++)
                        seen[wide][deferred][1] = fnv(seen[wide][deferred][1] + 1, pixels + row * SOFT_GPU_WIDTH, 428 * 2);
                    CHECK(pixels[11 * SOFT_GPU_WIDTH + 54 - 10] == 0x7fff); /* the quad's left side, in the target */
                }
            }
            CHECK(seen[wide][0][0] == seen[wide][1][0] && seen[wide][0][1] == seen[wide][1][1]);
        }
        SoftGpu_SetWidescreen(0);
        SoftGpu_SetDeferred(0, 0);
    }

    /* An area whose queue would pass HELD_WORDS_MAX (4M words) rasters what
     * it holds and goes on holding. 2.2 million dots of two words each pass
     * it once: the first, written to a place no later one touches, is in
     * VRAM before any settle, and the many overwrites after it end as they
     * do rastered as they come. (No hook lowers the limit; the run takes
     * about a second.) */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        enum { DOTS = 2200000 };
        uint64_t seen[2];
        for (int deferred = 0; deferred < 2; deferred++) {
            SoftGpuDeferredStats s;
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_Fill(0, 0, 1024, 512, 0);
            SoftGpu_SetDeferred(deferred, 0);
            CHECK(SoftGpu_Gp0(area, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0);
            for (uint32_t k = 0; k < DOTS; k++) {
                uint32_t dot[2];
                if (k == 0) { /* green at 300,230: no later dot reaches x 250 or y 200 */
                    dot[0] = 0x6800ff00u;
                    dot[1] = 230u << 16 | 300u;
                } else {
                    dot[0] = 0x68000000u | ((k * 0x010305u) & 0xffffffu);
                    dot[1] = ((k * 13u) % 200u) << 16 | ((k * 7u) % 250u);
                }
                CHECK(SoftGpu_Gp0(dot, 2) == 2);
            }
            SoftGpu_DeferredStats(&s);
            if (deferred) {
                CHECK(s.held == DOTS && s.immediate == 0 && s.why[SOFT_GPU_WHY_ALL] == 1);
                CHECK(SoftGpu_VramRaw()[230 * SOFT_GPU_WIDTH + 300] == 0x03e0); /* settled on the way, not by a read */
            }
            seen[deferred] = fnv(1469598103934665603ull, SoftGpu_Vram(), (size_t)SOFT_GPU_WIDTH * 512 * 2);
        }
        CHECK(seen[0] == seen[1]);
        SoftGpu_SetDeferred(0, 0);
    }

    /* The poison: held work shows as magenta to a reader that does not
     * settle, and as drawn to one that does. */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t sprite[] = {0x60ffffffu, 0x00100010u, 0x00040004u}; /* white, 4x4 at 16,16 */
        SoftGpu_Reset();
        SoftGpu_SetDeferred(1, 1);
        CHECK(SoftGpu_Gp0(area, 4) == 4);
        SoftGpu_Fill(0, 0, 320, 240, 0);
        CHECK(SoftGpu_Gp0(sprite, 3) == 3);
        CHECK(SoftGpu_VramRaw()[17 * SOFT_GPU_WIDTH + 17] == 0x7c1f && SoftGpu_VramRaw()[200 * SOFT_GPU_WIDTH] == 0x7c1f);
        SoftGpu_Settle(0, 0, 1, 1);
        CHECK(SoftGpu_VramRaw()[17 * SOFT_GPU_WIDTH + 17] == 0x7fff && SoftGpu_VramRaw()[200 * SOFT_GPU_WIDTH] == 0);
        /* A mod that binds VRAM, or a scaled picture: nothing is kept back,
         * and a batch does not even look (no clock held, nothing counted). */
        SoftGpu_SetDeferred(0, 0);
        SoftGpu_SetDeferred(1, 1);
        for (int why = 0; why < 2; why++) {
            SoftGpuDeferredStats s;
            if (why) CHECK(SoftGpu_SetScale(2));
            else SoftGpu_SetModVramUsers(1);
            SoftGpu_Fill(0, 0, 320, 240, 0);
            holds = 0;
            CHECK(SoftGpu_Gp0(sprite, 3) == 3);
            CHECK(SoftGpu_VramRaw()[17 * SOFT_GPU_WIDTH + 17] == 0x7fff && holds == 0);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 0 && s.immediate == 0);
            if (why) CHECK(SoftGpu_SetScale(1));
            else SoftGpu_SetModVramUsers(0);
        }
        SoftGpu_SetDeferred(0, 0);
    }
    /* The copy's fixed cases: at 2x, the model recorder, in three modes:
     * off, AUTO and AUTO with poison. */
    {
        static const uint32_t area_a[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        static const uint32_t area_b[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                          0xe6000000u, 0xe1000000u | (2u << 7)}; /* page 0, 16-bit */
        static const uint32_t red[] = {0x600000ffu, 0x00100004u, 0x00080008u};   /* 8x8 at 4,16 */
        static const uint32_t green[] = {0x6000ff00u, 0x00100004u, 0x00080008u}; /* the same place */
        static const uint32_t reader[] = {0x65808080u, 0x00000000u, 0x00001000u, 0x00080010u}; /* u 0-15, v 16-23 */
        static const uint32_t far_reader[] = {0x65808080u, 0x00000000u, 0x00001000u, 0x00080020u}; /* u 0-31 */
        const int modes[3][2] = {{0, 0}, {SOFT_GPU_DEFERRED_AUTO, 0}, {SOFT_GPU_DEFERRED_AUTO, 1}};
        uint64_t prefix[3], wrapped[3], window[3], only_reads[3];
        uint16_t flip[3][16 * 8], clut_flip[3][8 * 8];
        SoftGpuDeferredStats s;
        window_settles = 0;
        for (int m = 0; m < 3; m++) {
            /* The take falls between two sprites at one place: a patch is
             * the area as the copy missed it (the first), not as it is now. */
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            model_replay();
            CHECK(SoftGpu_Gp0(green, 3) == 3);
            CHECK(SoftGpu_Gp0(area_b, 5) == 5 && SoftGpu_Gp0(reader, 4) == 4);
            SoftGpu_DeferredStats(&s);
            if (m) CHECK(s.held == 2 && s.patches == 1);
            prefix[m] = copy_end();

            /* A fill that covers the area only by wrapping past x 1023 (16
             * on, 1024 wide): the recorder cuts it at the edge, so the area
             * it missed is settled (its patch), not dropped. */
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            model_replay();
            SoftGpu_Fill(16, 0, 1024, 240, 0xff0000);
            CHECK(SoftGpu_Gp0(area_b, 5) == 5 && SoftGpu_Gp0(reader, 4) == 4);
            SoftGpu_DeferredStats(&s);
            if (m) CHECK(s.dropped == 0 && s.patches == 1);
            wrapped[m] = copy_end();

            /* A host lookup in a replay settles held work the copy missed,
             * after the replay's last draw: the take that follows has it, and
             * the patch must not come back at the next replay. */
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            model_replay();
            CHECK(SoftGpu_Gp0(green, 3) == 3);
            model_flush();                /* a replay with nothing recorded: */
            SoftGpu_Settle(4, 16, 1, 1);  /* its lookup, */
            model_take();                 /* and the take */
            CHECK(SoftGpu_Gp0(area_b, 5) == 5 && SoftGpu_Gp0(reader, 4) == 4);
            window[m] = copy_end();

            /* A primitive only the recorder draws (a picture-only frame)
             * reads held work the copy missed: it is settled, so the copy
             * gets it. */
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            model_replay();
            SoftGpu_PictureOnlyFrame(1);
            CHECK(SoftGpu_Gp0(area_b, 5) == 5 && SoftGpu_Gp0(far_reader, 4) == 4);
            SoftGpu_PictureOnlyFrame(0);
            only_reads[m] = copy_end();

            /* Held, then the frame (or the palette) turns picture-only before
             * it is settled: what was held is still rastered. */
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            SoftGpu_PictureOnlyFrame(1);
            SoftGpu_Store(4, 16, 16, 8, flip[m]);
            SoftGpu_PictureOnlyFrame(0);
            {
                /* A raw 4-bit triangle from the page at 640,256
                 * through a palette of its own in each mode (row 300 + m),
                 * the palette made picture-only while it is held. */
                const uint32_t tri[] = {0x25808080u, 0x00200020u, ((300u + (uint32_t)m) << 22 | 63u << 16) | 0x0000u,
                                        0x00200028u, (uint32_t)(10 | 1 << 4) << 16 | 0x0007u,
                                        0x00280020u, 0x00000700u};
                uint16_t palette[16], texels[8 * 8];
                for (int i = 0; i < 16; i++) palette[i] = (uint16_t)(0x1111 * (i + 1));
                for (int i = 0; i < 64; i++) texels[i] = (uint16_t)(0x1234 + i * 0x0101);
                SoftGpu_Load(1008, 300 + m, 16, 1, palette);
                SoftGpu_Load(640, 256, 8, 8, texels);
                CHECK(SoftGpu_Gp0(tri, 7) == 7);
                SoftGpu_PictureOnlyClut(1008, 300 + m);
                SoftGpu_Store(32, 32, 8, 8, clut_flip[m]);
            }
            (void)copy_end();
        }
        CHECK(prefix[0] == prefix[1] && prefix[0] == prefix[2]);
        CHECK(wrapped[0] == wrapped[1] && wrapped[0] == wrapped[2]);
        CHECK(window[0] == window[1] && window[0] == window[2]);
        CHECK(only_reads[0] == only_reads[1] && only_reads[0] == only_reads[2]);
        CHECK(!memcmp(flip[0], flip[1], sizeof(flip[0])) && !memcmp(flip[0], flip[2], sizeof(flip[0])));
        CHECK(flip[0][0] == 0x001f && flip[0][15] == 0x2108); /* red was drawn, over the clear */
        CHECK(!memcmp(clut_flip[0], clut_flip[1], sizeof(clut_flip[0])) &&
              !memcmp(clut_flip[0], clut_flip[2], sizeof(clut_flip[0])));

        /* What lets work be held: AUTO holds nothing at 1x, nor at 2x with
         * a recorder that takes no patches, nor with one that has not taken
         * its copy yet; once it has, a sprite is held, widescreen on too. */
        {
            static const SoftGpuRecorder bare = {rec_gp0, rec_load, rec_move, rec_fill, rec_resync};
            SoftGpu_SetRecorder(NULL);
            CHECK(SoftGpu_SetScale(1));
            SoftGpu_Reset();
            SoftGpu_SetDeferred(SOFT_GPU_DEFERRED_AUTO, 0);
            holds = 0;
            CHECK(SoftGpu_Gp0(area_a, 4) == 4 && SoftGpu_Gp0(red, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 0 && s.immediate == 0 && holds == 0);
            SoftGpu_SetRecorder(&bare);
            CHECK(SoftGpu_SetScale(2));
            SoftGpu_CopyTaken(&bare);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 0);
            ops_used = 0;
            SoftGpu_SetRecorder(&model);
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 0);
            model_take();
            CHECK(SoftGpu_Gp0(red, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 1);
            SoftGpu_SetWidescreen(1);
            CHECK(SoftGpu_Gp0(green, 3) == 3);
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 2);
            SoftGpu_SetWidescreen(0);
            SoftGpu_SetDeferred(0, 0);
            CHECK(SoftGpu_SetScale(1));
            SoftGpu_SetRecorder(NULL);
            ops_used = 0;
        }

        /* A mod's recorder (SoftGpu_SetRecorder is lent to mods) keeps no
         * copy, even when an upload names another (the OpenGL picture's,
         * which the mod's replaced), so it holds nothing at 2x and its
         * `patch` is never called; and one made with an older header, whose
         * struct ends before `patch`, is never read that far. */
        {
            static const SoftGpuRecorder foreign = {rec_gp0, rec_load, rec_move, rec_fill, rec_resync,
                                                    NULL, NULL, foreign_patch};
            SoftGpuRecorder *older = guarded_recorder();
            CHECK(older != NULL);
            for (int which = 0; which < 2; which++) {
                SoftGpu_SetRecorder(NULL);
                CHECK(SoftGpu_SetScale(1));
                SoftGpu_Reset();
                SoftGpu_SetDeferred(SOFT_GPU_DEFERRED_AUTO, 0);
                SoftGpu_SetRecorder(which ? older : &foreign);
                CHECK(SoftGpu_SetScale(2));
                foreign_patches = 0;
                for (int take = 0; take < 2; take++) { /* SoftGpu_CopyTaken is not lent: never the mod's own */
                    SoftGpu_CopyTaken(take ? NULL : &model);
                    CHECK(SoftGpu_Gp0(area_a, 4) == 4);
                    SoftGpu_Fill(0, 0, 320, 240, 0x404040);
                    CHECK(SoftGpu_Gp0(red, 3) == 3);
                    SoftGpu_Settle(4, 16, 1, 1);
                    CHECK(SoftGpu_CopyPatches() == 0);
                }
                SoftGpu_DeferredStats(&s);
                CHECK(s.held == 0 && s.patches == 0 && foreign_patches == 0);
                SoftGpu_SetDeferred(0, 0);
                CHECK(SoftGpu_SetScale(1));
                SoftGpu_SetRecorder(NULL);
                ops_used = 0;
            }
        }

        /* The tick comes between a transfer's record and the transfer (a
         * move out of the area, a load or a fill into it, a batch's move)
         * and clears the area, whose held work the copy missed. Held work
         * or not, record and transfer are one: the tick waits for both, so
         * the copy, replayed, ends as VRAM (the area and the move's
         * target). Unheld, as ever, the tick may come between, and both
         * change order at once (not checked: the record's order is not
         * VRAM's then either). */
        for (int m = 1; m < 3; m++) {
            for (int kind = 0; kind < 4; kind++) {
                static const uint32_t moved[] = {0x80000000u, 4u | 16u << 16, 700u | 16u << 16, 8u | 8u << 16};
                uint16_t pixels[8 * 8], seen[320 * 240];
                for (int i = 0; i < 64; i++) pixels[i] = (uint16_t)(0x03e0 + i);
                CHECK(copy_begin(SOFT_GPU_DEFERRED_AUTO, m == 2) == 0);
                CHECK(SoftGpu_Gp0(area_a, 4) == 4);
                SoftGpu_Fill(0, 0, 320, 240, 0x404040);
                CHECK(SoftGpu_Gp0(red, 3) == 3);
                model_replay(); /* the take: the sprite held across it */
                armed_tick = tick_clear;
                if (kind == 0) SoftGpu_Move(4, 16, 700, 16, 8, 8);
                else if (kind == 1) SoftGpu_Load(4, 16, 8, 8, pixels);
                else if (kind == 2) SoftGpu_Fill(0, 16, 16, 8, 0xffffff);
                else CHECK(SoftGpu_Gp0(moved, 4) == 4);
                CHECK(!armed_tick && !pending_tick);
                model_play();
                SoftGpu_Store(0, 0, 320, 240, seen);
                for (int j = 0; j < 240; j++)
                    CHECK(!memcmp(seen + j * 320, copy_vram + j * SOFT_GPU_WIDTH, 320 * 2));
                SoftGpu_Store(700, 16, 8, 8, pixels);
                for (int j = 0; j < 8; j++)
                    CHECK(!memcmp(pixels + j * 8, copy_vram + (16 + j) * SOFT_GPU_WIDTH + 700, 8 * 2));
                if (kind == 0 || kind == 3) CHECK(pixels[0] == 0x001f); /* the red, moved before the clear */
                model_take();
                (void)copy_end();
            }
        }

        /* A new scale: the copy is made again (a resync), so, as with a new
         * recorder, a patch made for the old one is not handed over, and
         * nothing is held until the copy is taken again. */
        CHECK(copy_begin(SOFT_GPU_DEFERRED_AUTO, 0) == 0);
        CHECK(SoftGpu_Gp0(area_a, 4) == 4);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(red, 3) == 3);
        model_replay();                    /* the take: red held across it */
        CHECK(SoftGpu_Gp0(green, 3) == 3); /* held, after the take */
        CHECK(SoftGpu_SetScale(4));        /* settled: red's patch, then green */
        SoftGpu_DeferredStats(&s);
        CHECK(s.held == 2 && s.patches == 1);
        CHECK(SoftGpu_CopyPatches() == 0);
        CHECK(SoftGpu_Gp0(red, 3) == 3);
        SoftGpu_DeferredStats(&s);
        CHECK(s.held == 2); /* not held: no copy at 4x yet */
        model_replay();
        CHECK(SoftGpu_Gp0(green, 3) == 3);
        SoftGpu_DeferredStats(&s);
        CHECK(s.held == 3); /* held again once it is taken */
        (void)copy_end();

        /* Texture xBR reads one texel past the last u and v without
         * anti-aliasing (wrapping in the page), anywhere in the page with
         * it (SoftGpu_SetCopyReach): what it reads there of held work the
         * copy missed is settled, so the copy gets it; and work held under
         * a shorter reach is settled when the reach grows. Red is held
         * across a take next to the texels a 16-bit sprite from page 4
         * (256,0) reads: in the area at 0,0, which the sprite's u 64-255
         * reach only by wrapping to the page's first (and its u 64-127 only
         * with anti-aliasing); in the area at 320,0 just past u 0-63; in an
         * area at 0,16 just past v 0-15. */
        {
            static const uint32_t area_a16[] = {0xe3000000u | (16u << 10), 0xe4000000u | 319 | (239 << 10),
                                                0xe5000000u | (16u << 11), 0xe6000000u}; /* 320x224 at 0,16 */
            static const uint32_t red_far[] = {0x600000ffu, 0x00100100u, 0x00080008u}; /* 8x8 at 256,16 of 0,0 */
            static const uint32_t red_b[] = {0x600000ffu, 0x00100000u, 0x00080008u};   /* at 0,16 of 320,0 */
            static const uint32_t red_top[] = {0x600000ffu, 0x00000100u, 0x00080008u}; /* at 256,0 of 0,16 */
            static const uint32_t far_page[] = {0xe1000000u | 4 | (2u << 7)};          /* page 4, 16-bit */
            static const uint32_t to_end[] = {0x65808080u, 0x00000000u, 0x00001040u, 0x000800c0u};   /* u 64-255 */
            static const uint32_t short_of[] = {0x65808080u, 0x00000000u, 0x00001040u, 0x00080040u}; /* u 64-127 */
            static const uint32_t left_of[] = {0x65808080u, 0x00000000u, 0x00001000u, 0x00080040u};  /* u 0-63 */
            static const uint32_t above[] = {0x65808080u, 0x00000000u, 0x00000000u, 0x00100040u};    /* v 0-15 */
            static const struct {
                const uint32_t *held, *red, *drawn, *sprite;
                int fill[4], reach_held, reach_read;
            } cases[] = {
                {area_a, red_far, area_b, to_end, {0, 0, 320, 240}, 1, 1},
                {area_a, red_far, area_b, short_of, {0, 0, 320, 240}, SOFT_GPU_REACH_PAGE, SOFT_GPU_REACH_PAGE},
                {area_a, red_far, area_b, to_end, {0, 0, 320, 240}, 0, 1}, /* held under a shorter reach */
                {area_b, red_b, area_a, left_of, {320, 0, 320, 240}, 1, 1},
                {area_a16, red_top, area_b, above, {0, 16, 320, 224}, 1, 1},
            };
            const int n = (int)(sizeof(cases) / sizeof(cases[0]));
            for (int c = 0; c < n; c++) {
                uint64_t reads[3];
                for (int m = 0; m < 3; m++) {
                    model_reach = cases[c].reach_held;
                    CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
                    CHECK(SoftGpu_Gp0(cases[c].held, 4) == 4);
                    SoftGpu_Fill(cases[c].fill[0], cases[c].fill[1], cases[c].fill[2], cases[c].fill[3], 0x404040);
                    CHECK(SoftGpu_Gp0(cases[c].red, 3) == 3);
                    model_replay(); /* the take: red held across it */
                    SoftGpu_DeferredStats(&s);
                    if (m) CHECK(s.held == 1);
                    CHECK(SoftGpu_Gp0(cases[c].drawn, 4) == 4 && SoftGpu_Gp0(far_page, 1) == 1);
                    CHECK(SoftGpu_Gp0(cases[c].sprite, 4) == 4);
                    model_reach = cases[c].reach_read;
                    reads[m] = copy_end();
                }
                CHECK(reads[0] == reads[1] && reads[0] == reads[2]);
            }
            model_reach = 0;
        }
        window_settles = 1;
    }
    /* Banks and the pack's shadow (soft_gpu.c, frozen_sync): a held
     * primitive reads them as they were when it came, from a frozen copy,
     * whatever is written to them before it is rastered; a rewrite of the
     * same words rasters nothing; a change rasters the held work that read
     * them before the next primitive that reads them is held. Each case is
     * run rastered at once and kept back, and both must store the same. */
    {
        static const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        static const uint32_t bank_page[] = {0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(3)}; /* E1: bank 3, 16-bit 640,0 */
        static const uint32_t shadow_page[] = {0xe1000000u | 10 | 16 | (2u << 7)};                  /* 16-bit 640,256 */
        static const uint32_t wrap_page[] = {0xe1000000u | 10 | (1u << 7) | SOFT_GPU_BANK_TPAGE(3)}; /* 8-bit, bank 3 */
        static uint16_t out[2][64 * 24];
        /* 1, 2: sprites whose bank comes from the E1 word (bank 3), and ones
         * whose cells come from the shadow: A held; the same words written
         * again; B held (nothing rastered); a change; C held (A and B
         * rastered first, from the copy); a change again (C keeps its). */
        for (int which = 0; which < 2; which++) {
            for (int deferred = 0; deferred < 2; deferred++) {
                SoftGpuDeferredStats st;
                uint16_t *bank = SoftGpu_Bank(3), word = 0x1234;
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 1024, 512, 0);
                SoftGpu_Load(640, 256, 1, 1, &word);
                TextureDump_Prepare = which ? shadow_prepare : NULL;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) {
                        bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x001f;
                        *TextureDump_Cell(640 + i, 256 + j, 0) = 0x8000 | 0x001f;
                    }
                SoftGpu_SetDeferred(deferred, 0);
                CHECK(SoftGpu_Gp0(area, 4) == 4);
                SoftGpu_Fill(0, 0, 320, 240, 0x404040);
                CHECK(SoftGpu_Gp0(which ? shadow_page : bank_page, 1) == 1);
                for (int k = 0; k < 3; k++) {
                    const uint32_t sprite[] = {0x65808080u, (16u << 16) | (uint32_t)(16 + 16 * k), 0, 0x00040004u};
                    CHECK(SoftGpu_Gp0(sprite, 4) == 4);
                    SoftGpu_DeferredStats(&st);
                    if (deferred) CHECK(st.held == (unsigned)k + 1 && st.settled == (k == 2 ? 1u : 0u) &&
                                        st.why[SOFT_GPU_WHY_WRITE] == (k == 2 ? 1u : 0u) && st.immediate == 0);
                    for (int j = 0; j < 4; j++)
                        for (int i = 0; i < 4; i++) {
                            uint16_t *at = which ? TextureDump_Cell(640 + i, 256 + j, 0) : &bank[j * SOFT_GPU_WIDTH + 640 + i];
                            *at = k == 0 ? *at : (uint16_t)((which ? 0x8000 : 0) | (k == 1 ? 0x03e0 : 0x7c00));
                        }
                }
                SoftGpu_Store(0, 0, 64, 24, out[deferred]);
            }
            CHECK(!memcmp(out[0], out[1], sizeof(out[0])));
            CHECK(out[0][16 * 64 + 16] == 0x001f && out[0][16 * 64 + 32] == 0x001f && out[0][16 * 64 + 48] == 0x03e0);
        }
        TextureDump_Prepare = NULL;
        /* A bank's texels at coordinates inside the drawing area, where work
         * is held: no VRAM word is read, so it is held all the same. */
        {
            static const uint32_t inside[] = {0xe1000000u | (2u << 7) | SOFT_GPU_BANK_TPAGE(3), /* 16-bit 0,0 */
                                              0x65808080u, 0x00100010u, 0x00001010u, 0x00040004u};
            SoftGpuDeferredStats st;
            SoftGpu_SetDeferred(0, 0);
            SoftGpu_Reset();
            SoftGpu_SetDeferred(1, 0);
            CHECK(SoftGpu_Gp0(area, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(inside, 5) == 5 && SoftGpu_Gp0(inside, 5) == 5);
            SoftGpu_DeferredStats(&st);
            CHECK(st.held == 2 && st.immediate == 0);
        }
        /* The panel's name rewriting a bank inside the batch: the host's
         * answer may come through code that writes one (soft_gpu.c,
         * name_over_panel). In one batch, a sprite from bank 9 is held in the
         * second area; the panel, drawn in the first, gets its name, whose
         * callback changes the texels that sprite read; a second sprite from
         * the same texels is held in the second area. What the batch
         * remembered of bank 9 is forgotten at the name, so the change is
         * seen: the first sprite is rastered from the copy (red), the second
         * reads the new words (green), as rastered at once. */
        {
            static const uint32_t batch[] = {
                0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320, 0xe6000000u,
                0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(9),      /* bank 9, 16-bit 640,0 */
                0x65808080u, 0x00100010u, 0x00000000u, 0x00040004u,        /* 4x4 at 16,16 of the second area */
                0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u,
                0xe1000000u | 11,                                           /* the panel's page: 4-bit 704,0 */
                0x64808080u, 0x00200020u, (uint32_t)((252 << 6) | 46) << 16 | 0x8080u, 0x00280040u, /* 64x40 from 128,128 */
                0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320, 0xe6000000u,
                0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(9),
                0x65808080u, 0x00100020u, 0x00000000u, 0x00040004u};      /* 4x4 at 32,16, the same texels */
            const size_t words = sizeof(batch) / sizeof(batch[0]);
            SoftGpuDeferredStats st;
            for (int deferred = 0; deferred < 2; deferred++) {
                uint16_t *bank = SoftGpu_Bank(9);
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 1024, 512, 0);
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x001f;
                renamed_bank = bank;
                SoftGpu_PanelName = bank_name;
                SoftGpu_SetDeferred(deferred, 0);
                SoftGpu_Fill(0, 0, 640, 240, 0x404040);
                CHECK(SoftGpu_Gp0(batch, words) == words);
                SoftGpu_PanelName = NULL;
                renamed_bank = NULL;
                SoftGpu_DeferredStats(&st);
                SoftGpu_Store(320, 0, 64, 24, out[deferred]);
            }
            /* The pixels first: they are what forgetting is for. */
            CHECK(!memcmp(out[0], out[1], sizeof(out[0])));
            CHECK(out[0][16 * 64 + 16] == 0x001f && out[0][16 * 64 + 32] == 0x03e0);
            CHECK(st.held == 2 && st.why[SOFT_GPU_WHY_WRITE] == 1 && st.now[SOFT_GPU_NOW_PANEL] == 1);
        }
        /* 3: a polygon naming bank 17 (bit 10 of its page word) with a fade,
         * 4-bit, its palette in the bank: the palette entry its texels index
         * changed after it is held. 4: 8-bit sprites from bank 3 whose
         * palette at x 960 runs past 1023: the entry their texels index
         * (100) lies at x 36; it changes between two held sprites. */
        for (int which = 0; which < 2; which++) {
            for (int deferred = 0; deferred < 2; deferred++) {
                SoftGpuDeferredStats st;
                uint16_t *bank = SoftGpu_Bank(which ? 3 : 17);
                const uint32_t quad[] = {0x2d808080u, 0x00100010u, (uint32_t)((300 << 6) | 48) << 16,
                                         0x00100018u, (uint32_t)(10 | SOFT_GPU_BANK_TPAGE(17)) << 16 | 0x0007u,
                                         0x00180010u, (0x8000u | 100u) << 16 | 0x0700u, 0x00180018u, 0x0707u};
                SoftGpu_SetDeferred(0, 0);
                SoftGpu_Reset();
                SoftGpu_Fill(0, 0, 1024, 512, 0);
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 4; i++) bank[j * SOFT_GPU_WIDTH + 640 + i] = which ? 0x6464 : 0x1111;
                bank[300 * SOFT_GPU_WIDTH + 768 + 1] = 0x001f;
                bank[301 * SOFT_GPU_WIDTH + 36] = 0x001f;
                SoftGpu_SetDeferred(deferred, 0);
                CHECK(SoftGpu_Gp0(area, 4) == 4);
                SoftGpu_Fill(0, 0, 320, 240, 0x404040);
                if (!which) {
                    CHECK(SoftGpu_Gp0(quad, 9) == 9);
                    bank[300 * SOFT_GPU_WIDTH + 768 + 1] = 0x03e0;
                } else {
                    CHECK(SoftGpu_Gp0(wrap_page, 1) == 1);
                    for (int k = 0; k < 2; k++) {
                        const uint32_t sprite[] = {0x65808080u, (16u << 16) | (uint32_t)(16 + 16 * k),
                                                   (uint32_t)((301 << 6) | 60) << 16, 0x00040004u};
                        CHECK(SoftGpu_Gp0(sprite, 4) == 4);
                        bank[301 * SOFT_GPU_WIDTH + 36] = 0x03e0;
                    }
                }
                SoftGpu_DeferredStats(&st);
                if (deferred) CHECK(st.held == (which ? 2u : 1u) && st.why[SOFT_GPU_WHY_WRITE] == (which ? 1u : 0u));
                SoftGpu_Store(0, 0, 64, 24, out[deferred]);
            }
            CHECK(!memcmp(out[0], out[1], sizeof(out[0])));
            CHECK(which ? out[0][16 * 64 + 16] == 0x001f && out[0][16 * 64 + 32] == 0x03e0
                        : (out[0][18 * 64 + 18] & 0x1f) > 16 && (out[0][18 * 64 + 18] >> 5 & 0x1f) < 8); /* red, faded over grey */
        }
        SoftGpu_SetDeferred(0, 0);
    }

    /* At 2x with the model recorder: a sprite from bank 3 held across a take
     * of the copy; the bank changes; another from it is held, which rasters
     * the first (a patch for the copy, made from the frozen copy); the
     * other area then samples what the first drew. The copy's reads are the
     * same whether held or not, poisoned or not. */
    {
        static const int modes[3][2] = {{0, 0}, {SOFT_GPU_DEFERRED_AUTO, 0}, {SOFT_GPU_DEFERRED_AUTO, 1}};
        static const uint32_t area_a[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        static const uint32_t area_b[] = {0xe3000000u | 320, 0xe4000000u | 639 | (239 << 10), 0xe5000000u | 320,
                                          0xe6000000u};
        static const uint32_t bank_page[] = {0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(3)};
        static const uint32_t sprite_a[] = {0x65808080u, 0x00100010u, 0, 0x00040004u};
        static const uint32_t sprite_b[] = {0x65808080u, 0x00100020u, 0, 0x00040004u};
        static const uint32_t own_page[] = {0xe1000000u | (2u << 7)};                          /* 16-bit page 0 */
        static const uint32_t reader[] = {0x65808080u, 0x00000000u, 0x00001010u, 0x00040020u}; /* u 16-47, v 16-19 */
        uint64_t reads[3];
        window_settles = 0;
        for (int m = 0; m < 3; m++) {
            SoftGpuDeferredStats st;
            uint16_t *bank = SoftGpu_Bank(3);
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x001f;
            CHECK(copy_begin(modes[m][0], modes[m][1]) == 0);
            CHECK(SoftGpu_Gp0(area_a, 4) == 4);
            SoftGpu_Fill(0, 0, 320, 240, 0x404040);
            CHECK(SoftGpu_Gp0(bank_page, 1) == 1 && SoftGpu_Gp0(sprite_a, 4) == 4);
            model_replay(); /* the take: sprite A held across it */
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x03e0;
            CHECK(SoftGpu_Gp0(sprite_b, 4) == 4);
            SoftGpu_DeferredStats(&st);
            if (m) CHECK(st.held == 2 && st.patches == 1 && st.why[SOFT_GPU_WHY_WRITE] == 1);
            CHECK(SoftGpu_Gp0(area_b, 4) == 4 && SoftGpu_Gp0(own_page, 1) == 1 && SoftGpu_Gp0(reader, 4) == 4);
            reads[m] = copy_end();
        }
        CHECK(reads[0] == reads[1] && reads[0] == reads[2]);
        window_settles = 1;
    }

    /* The clock's tick flushing a batch (an interrupt-level LoadImage,
     * libgpu.c's flush_drawing), where nothing may be allocated: a primitive
     * from bank 3 or the shadow whose frozen copy is made holds there; one
     * whose copy would be made there is rastered at once (NOW_BANK,
     * NOW_SHADOW), as it would be with no memory for it. */
    for (int which = 0; which < 2; which++) {
        static const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t page[] = {which ? 0xe1000000u | 10 | 16 | (2u << 7)
                                       : 0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(3)};
        static const uint32_t sprite[] = {0x65808080u, 0x00100010u, 0, 0x00040004u};
        SoftGpuDeferredStats st;
        uint16_t *bank = SoftGpu_Bank(3), word = 0x1234, got;
        int allocs = tick_allocs;
        SoftGpu_SetDeferred(0, 0); /* the copies go */
        SoftGpu_Reset();
        SoftGpu_Fill(0, 0, 1024, 512, 0);
        SoftGpu_Load(640, 256, 1, 1, &word);
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                bank[j * SOFT_GPU_WIDTH + 640 + i] = 0x001f;
                *TextureDump_Cell(640 + i, 256 + j, 0) = 0x8000 | 0x001f;
            }
        TextureDump_Prepare = which ? shadow_prepare : NULL;
        SoftGpu_SetDeferred(1, 0);
        CHECK(SoftGpu_Gp0(area, 4) == 4);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4); /* held: the copy made here */
        in_tick = 1;
        CHECK(SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4); /* held from the tick too */
        in_tick = 0;
        SoftGpu_DeferredStats(&st);
        CHECK(st.held == 2 && st.immediate == 0);
        SoftGpu_SetDeferred(0, 0); /* the copies go again */
        SoftGpu_SetDeferred(1, 0);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        in_tick = 1;
        CHECK(SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4); /* at once: no copy made in the tick */
        in_tick = 0;
        SoftGpu_DeferredStats(&st);
        CHECK(st.held == 0 && st.now[which ? SOFT_GPU_NOW_SHADOW : SOFT_GPU_NOW_BANK] == 1);
        CHECK(tick_allocs == allocs);
        SoftGpu_Store(16, 16, 1, 1, &got);
        CHECK(got == 0x001f);
        TextureDump_Prepare = NULL;
        SoftGpu_SetDeferred(0, 0);
    }

#ifdef SOFT_GPU_TEST_WRAP_ALLOC
    /* No memory for a frozen copy: the primitive is rastered at once
     * (NOW_BANK, NOW_SHADOW) and its area given up past its next clear (the
     * game's two in a row count as one), held again after the one after.
     * A primitive rastered at once (given up here) makes no frozen copy. */
    for (int which = 0; which < 2; which++) {
        static const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t page[] = {which ? 0xe1000000u | 10 | 16 | (2u << 7)
                                       : 0xe1000000u | 10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(3)};
        static const uint32_t sprite[] = {0x65808080u, 0x00100010u, 0, 0x00040004u};
        SoftGpuDeferredStats st;
        SoftGpu_SetDeferred(0, 0); /* the copies go */
        SoftGpu_Reset();
        TextureDump_Prepare = which ? shadow_prepare : NULL;
        SoftGpu_SetDeferred(1, 0);
        CHECK(SoftGpu_Gp0(area, 4) == 4);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(page, 1) == 1);
        (void)SoftGpu_Bank(3); /* made: the callocs below are the copies' */
        fail_calloc = 1;
        CHECK(SoftGpu_Gp0(sprite, 4) == 4);
        fail_calloc = 0;
        callocs = 0;
        CHECK(SoftGpu_Gp0(sprite, 4) == 4); /* given up: at once */
        SoftGpu_DeferredStats(&st);
        CHECK(st.held == 0 && st.now[which ? SOFT_GPU_NOW_SHADOW : SOFT_GPU_NOW_BANK] == 1 &&
              st.now[SOFT_GPU_NOW_AFTER] == 1);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4); /* still given up: at once */
        SoftGpu_DeferredStats(&st);
        CHECK(st.held == 0 && st.now[SOFT_GPU_NOW_AFTER] == 2 && callocs == 0);
        SoftGpu_Fill(0, 0, 320, 240, 0x404040);
        CHECK(SoftGpu_Gp0(page, 1) == 1 && SoftGpu_Gp0(sprite, 4) == 4); /* held, its copy made now */
        SoftGpu_DeferredStats(&st);
        CHECK(st.held == 1 && st.now[SOFT_GPU_NOW_AFTER] == 2 && callocs == 1);
        TextureDump_Prepare = NULL;
        SoftGpu_SetDeferred(0, 0);
    }
#endif
    puts("Software GPU deferred raster checks passed");
    return 0;
}
