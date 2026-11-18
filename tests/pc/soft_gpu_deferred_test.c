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

/* Two frames in three are calm, as most of the game's are: what they load,
 * move, fill, store, settle, tag and sample lies right of the drawing areas
 * (x 640 on), so the held work lives until its area's next clear and is
 * dropped there. The others put all of it anywhere. */
static int calm;
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
    page |= (uint32_t)below(4) << 5 | (uint32_t)below(3) << 7;
    if (below(12) == 0) page |= SOFT_GPU_BANK_TPAGE(3);
    return page;
}

static uint32_t clut_word(void)
{
    int x = below(4) == 0 && !calm ? below(40) : 40 + below(24), y = below(4) == 0 ? below(240) : 480 + below(32);
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
    for (int frame = 0; frame < frames; frame++) {
        int area = frame & 1, x0 = area ? 320 : 0, y0 = 0, count = 0;
        size_t at = 0;
        calm = below(3) != 0;
        if (frame % 97 == 50) SoftGpu_SetModVramUsers(1); /* a mod binds VRAM for a while */
        if (frame % 97 == 60) SoftGpu_SetModVramUsers(0);
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
        count = 20 + below(80);
        for (int n = 0; n < count; n++) {
            int op = below(100);
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
    int frames = argc > 1 ? atoi(argv[1]) : 1000;
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
        printf("run %u: held %llu (dropped %llu, rastered %llu), at once %llu, states while held %d\n", start,
               (unsigned long long)held.stats.held, (unsigned long long)held.stats.dropped,
               (unsigned long long)held.stats.rastered, (unsigned long long)held.stats.immediate, held.states_held);
        CHECK(held.stats.held > (unsigned)frames && held.stats.dropped > (unsigned)frames / 20 && held.stats.rastered > (unsigned)frames);
        CHECK(held.stats.held == held.stats.dropped + held.stats.rastered);
        CHECK(held.states_held > 0);
        for (int why = 0; why < SOFT_GPU_NOW_COUNT; why++)
            if (why != SOFT_GPU_NOW_PANEL) CHECK(held.stats.now[why] > 0); /* every rule was met */
        CHECK(now.stores == held.stores && now.stores == poisoned.stores);
        CHECK(now.vram == held.vram && now.vram == poisoned.vram);
        CHECK(now.tags == held.tags && now.tags == poisoned.tags);
        CHECK(now.shadow == held.shadow && now.shadow == poisoned.shadow);
        CHECK(now.gpu == held.gpu && now.gpu == poisoned.gpu);
    }

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
        /* A mod that binds VRAM: nothing is kept back. */
        SoftGpu_SetDeferred(0, 0);
        SoftGpu_SetDeferred(1, 0);
        SoftGpu_SetModVramUsers(1);
        CHECK(SoftGpu_Gp0(sprite, 3) == 3);
        {
            SoftGpuDeferredStats s;
            SoftGpu_DeferredStats(&s);
            CHECK(s.held == 0 && s.immediate == 1 && s.now[SOFT_GPU_NOW_OTHER] == 1);
        }
        SoftGpu_SetModVramUsers(0);
        SoftGpu_SetDeferred(0, 0);
    }
    puts("Software GPU deferred raster checks passed");
    return 0;
}
