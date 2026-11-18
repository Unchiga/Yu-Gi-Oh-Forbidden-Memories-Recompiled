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
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_dump.h"
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
 * cycles of each area or so), as the game's stretches do: a cycle that met
 * a bank leaves the next one given up (`again`), so calm frames one by one
 * would rarely hold. Four or five, so that a busy stretch starts on either
 * area, and either area's calm queue is held while a busy frame's loads,
 * moves, fills and the rest run (Outcome.over_held: four at a time, the
 * first area's never was). */
static int calm, calm_run;
static int column(int w) { return calm ? 640 + below(384 - w) : below(1024); }

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
    SoftGpuDeferredStats stats;
} Outcome;

static Outcome *now_outcome;

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
        if (tick_rnd() & 1) {
            SoftGpu_Load(x, y, 4, 4, words);
        } else {
            SoftGpu_Store(x, y, 4, 4, words);
            now_outcome->stores = fnv(now_outcome->stores, words, sizeof(words));
        }
    }
    return page_y == 256 && page_x == 640 ? 1 : page_y == 256 && page_x == 704 ? 2 : 0;
}

static int holds, releases, depth, nested;
static void hold_clock(int on)
{
    if (on) {
        holds++;
        if (depth++) nested = 1;
    } else {
        releases++;
        depth--;
    }
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
 * pages (prepare), and bank 3. */
static uint32_t page_word(void)
{
    int pick = below(16);
    uint32_t page = pick < 6 ? (uint32_t)(10 + below(6)) | (uint32_t)below(2) << 4 /* x 640..1023 */
                  : pick < 8 && !calm ? (uint32_t)below(10)                         /* in an area */
                  : pick < 10 ? 10u | 16u                                           /* the shadow's */
                  : pick < 11 ? 11u | 16u
                  : calm ? (uint32_t)(10 + below(6)) | 16u
                  : (uint32_t)below(16) | 16u;                                       /* the lower half */
    if (calm) { /* nothing that rasters at once (the pack's pages, the bank): the area keeps holding */
        page = (uint32_t)(12 + below(4)) | (uint32_t)below(2) << 4;
        if (below(2)) page = (uint32_t)(10 + below(6));
    }
    page |= (uint32_t)below(4) << 5 | (uint32_t)below(3) << 7;
    /* A page whose texels run past x 1023 wraps into the first area's
     * columns (16 bits from 832 on, 8 from 960): a calm frame's takes the
     * lower half, so the wrap lands below the areas. */
    if (calm && (page & 15) * 64 + (64 << ((page >> 7) & 3)) > 1024) page |= 16;
    if (below(12) == 0 && !calm) page |= SOFT_GPU_BANK_TPAGE(3);
    return page;
}

static uint32_t clut_word(void)
{
    int x = below(4) == 0 && !calm ? below(40) : 40 + below(24), y = below(4) == 0 ? below(240) : 480 + below(32);
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
                uint32_t extra = v == 0 ? clut_word() : v == 1 ? page_word() : 0;
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
    SoftGpu_SetDeferred(deferred, poison);
    for (int frame = 0, run_end = 0; frame < frames; frame++) {
        int area = frame & 1, x0 = area ? 320 : 0, y0 = 0, count = 0, busy;
        size_t at = 0;
        if (frame == run_end) { /* four or five: a run may start on either area */
            calm_run = below(4) != 0;
            run_end = frame + 4 + below(2);
        }
        calm = calm_run;
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
                batch[at++] = 0xe1000000u | (page_word() & 0x7ff);
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
        if (below(4) == 0) {
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
            if (!SoftGpu_SetScale(2) || !SoftGpu_SetScale(1)) return 1;
        }
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
    if (!TextureDump_EnableShadow()) {
        fprintf(stderr, "no memory for the tags\n");
        return 1;
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
         * widescreen: a mod's VRAM or a scale raster at once without a look). */
        for (int why = 0; why < SOFT_GPU_NOW_COUNT; why++)
            if (why != SOFT_GPU_NOW_PANEL && why != SOFT_GPU_NOW_OTHER) CHECK(held.stats.now[why] > 0);
        CHECK(now.stores == held.stores && now.stores == poisoned.stores);
        CHECK(now.vram == held.vram && now.vram == poisoned.vram);
        CHECK(now.tags == held.tags && now.tags == poisoned.tags);
        CHECK(now.shadow == held.shadow && now.shadow == poisoned.shadow);
        CHECK(now.gpu == held.gpu && now.gpu == poisoned.gpu);
    }

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

    /* An area a bank made go at once stays given up past its next clear
     * (twice, as the game clears) while each cycle meets a bank, and holds
     * again after a cycle that met none; one given up for a texture in its
     * own area (any reason but a bank, the shadow or the panel) holds again
     * after its next clear. */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u, 0xe6000000u};
        const uint32_t plain[] = {0x60ffffffu, 0x00100010u, 0x00040004u}; /* white, 4x4 at 16,16 */
        const uint32_t banked[] = {0x25808080u, 0x00000000u, 0x00000000u, 0x00000010u, /* raw textured triangle */
                                   (uint32_t)(10 | (2u << 7) | SOFT_GPU_BANK_TPAGE(3)) << 16 | 0x0010u,
                                   0x00100000u, 0x1000u};
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
            /* Cycle 0 holds its sprite; 1 (after a bank) and 2 (after a
             * bank again) hold nothing; 3 (after a cycle without), 4, 5 and
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
    puts("Software GPU deferred raster checks passed");
    return 0;
}
