/* Settling (soft_gpu.h, notes/pc-build.md "Settling VRAM before it is
 * read"): every reader of rastered words inside the software GPU settles
 * what it reads, before it reads it, and drawing alone settles nothing.
 *
 * The raster is immediate, so the observer stands in for one that keeps
 * work back: `hold` puts words aside, and the first settle that crosses
 * them writes them into VRAM, as such a raster would draw what it held. A
 * reader that settles after it reads, or not at all, then reads the old
 * words, and the check fails. The host lookups outside this library are
 * checked from their source (tools/pc/check_vram_readers.py). */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_dump.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

#define SEEN_MAX 32
static int seen[SEEN_MAX][4], seen_count;
/* The state at the first settle since the last clear(). */
static int scale_then, rastered_then;
static struct { int on, x, y, w, h; uint16_t value; } held;

static void hold(int x, int y, int w, int h, uint16_t value)
{
    held.on = 1;
    held.x = x;
    held.y = y;
    held.w = w;
    held.h = h;
    held.value = value;
}

static void observe(int x, int y, int w, int h)
{
    if (!seen_count) {
        scale_then = SoftGpu_Scale();
        rastered_then = SoftGpu_WideRastered();
    }
    if (seen_count < SEEN_MAX) {
        seen[seen_count][0] = x;
        seen[seen_count][1] = y;
        seen[seen_count][2] = w;
        seen[seen_count][3] = h;
    }
    seen_count++;
    if (held.on && x < held.x + held.w && held.x < x + w && y < held.y + held.h && held.y < y + h) {
        uint16_t *vram = (uint16_t *)SoftGpu_VramRaw(); /* the test's stand-in raster */
        for (int j = 0; j < held.h; j++)
            for (int i = 0; i < held.w; i++) vram[(held.y + j) * SOFT_GPU_WIDTH + held.x + i] = held.value;
        held.on = 0;
    }
}

/* Whether the rectangle was among those settled since the last clear. */
static int settled(int x, int y, int w, int h)
{
    for (int i = 0; i < seen_count && i < SEEN_MAX; i++) {
        if (seen[i][0] == x && seen[i][1] == y && seen[i][2] == w && seen[i][3] == h) return 1;
    }
    return 0;
}

static void clear(void) { seen_count = 0; held.on = 0; }

static uint16_t at(int x, int y) { return SoftGpu_VramRaw()[y * SOFT_GPU_WIDTH + x]; }

#define WHOLE 0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT

/* The recorder's resync reads VRAM whole: what it saw at 500,300. */
static uint16_t resync_saw;
static void record_gp0(const uint32_t *words, size_t count) { (void)words; (void)count; }
static void record_load(int x, int y, int w, int h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
static void record_move(int x, int y, int dx, int dy, int w, int h)
{ (void)x; (void)y; (void)dx; (void)dy; (void)w; (void)h; }
static void record_fill(int x, int y, int w, int h, uint32_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
static void record_resync(int scale, const uint32_t state[6])
{
    (void)scale;
    (void)state;
    resync_saw = at(500, 300);
}
static const SoftGpuRecorder recording = {record_gp0, record_load, record_move, record_fill, record_resync};

int main(void)
{
    const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u};
    const uint32_t lower[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10), 0xe5000000u | (256 << 11)};
    /* A 4-bit sprite from page 1 (64,0) through the palette at 256,1; a
     * 4-bit triangle from the same page and palette (page word 0x0001). */
    const uint32_t sprite[] = {0xe1000001u, 0x65808080u, 0x00200020u, 0x00500000u, 0x00020004u};
    const uint32_t triangle[] = {0x24808080u, 0x00400040u, 0x00500000u, 0x00400044u, 0x00010004u,
                                 0x00440040u, 0x00000400u};
    const uint32_t mask_check[] = {0xe6000002u}, mask_off[] = {0xe6000000u}, mask_set[] = {0xe6000001u};
    const uint32_t gp0_move[] = {0x80000000u, 0x00030002u, 0x00410040u, 0x00050004u}; /* 2,3 -> 64,65, 4x5 */
    const uint32_t gp0_load[] = {0xa0000000u, 0x00080007u, 0x00010002u, 0x00000000u}; /* 2x1 at 7,8 */
    const uint32_t gp0_fill[] = {0x02000000u, 0x00000000u, 0x00100010u};
    uint16_t words[64] = {0}, *bank;
    size_t size;
    const uint16_t *pixels;
    int x, w, i;

    SoftGpu_Reset();
    SoftGpu_SettleObserver = observe;

    /* Drawing reads nothing outside the raster: no settling (texture dump
     * off; it is on at the end). */
    CHECK(SoftGpu_Gp0(area, 3) == 3);
    clear();
    CHECK(SoftGpu_Gp0(sprite, 5) == 5 && SoftGpu_Gp0(triangle, 7) == 7 && seen_count == 0);
    CHECK(SoftGpu_Gp0(gp0_fill, 3) == 3 && seen_count == 0);
    SoftGpu_Fill(0, 0, 8, 8, 0x00ff00);
    SoftGpu_Load(4, 4, 2, 2, words);
    CHECK(seen_count == 0);

    /* Whole-VRAM readers see what was held. */
    clear();
    pixels = SoftGpu_VramRaw(); /* the raw one settles nothing */
    CHECK(seen_count == 0);
    hold(600, 400, 1, 1, 0x1234);
    pixels = SoftGpu_Vram();
    CHECK(settled(WHOLE) && pixels[400 * SOFT_GPU_WIDTH + 600] == 0x1234);
    clear();
    hold(601, 400, 1, 1, 0x2345);
    pixels = SoftGpu_StateData(0, &size);
    CHECK(size == SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * 2 && pixels[400 * SOFT_GPU_WIDTH + 601] == 0x2345);
    clear();
    CHECK(SoftGpu_StateData(1, &size) && seen_count == 0); /* the drawing state alone */

    /* StoreImage reads what was held there. */
    clear();
    hold(10, 20, 4, 3, 0x0bad);
    SoftGpu_Store(10, 20, 4, 3, words);
    CHECK(settled(10, 20, 4, 3) && seen_count == 1);
    for (i = 0; i < 12; i++) CHECK(words[i] == 0x0bad);

    /* A move's source, both ways in; onto itself without the mask it
     * changes nothing and settles nothing. */
    clear();
    hold(1, 2, 5, 6, 0x0c0d);
    SoftGpu_Move(1, 2, 100, 200, 5, 6);
    CHECK(settled(1, 2, 5, 6) && seen_count == 1 && at(100, 200) == 0x0c0d && at(104, 205) == 0x0c0d);
    clear();
    hold(2, 3, 4, 5, 0x0e0f);
    CHECK(SoftGpu_Gp0(gp0_move, 4) == 4 && settled(2, 3, 4, 5) && seen_count == 1 && at(64, 65) == 0x0e0f);
    clear();
    SoftGpu_Move(0, 0, 0, 0, 2, 1);
    CHECK(seen_count == 0);
    CHECK(SoftGpu_Gp0(mask_set, 1) == 1);
    clear();
    SoftGpu_Move(0, 0, 0, 0, 2, 1); /* with the mask bit set it changes them */
    CHECK(settled(0, 0, 2, 1));
    CHECK(SoftGpu_Gp0(mask_off, 1) == 1);

    /* A capture's source (the copy stands for the StoreImage before it). */
    clear();
    SoftGpu_Capture(30, 40, 320, 256, 8, 9);
    CHECK(settled(30, 40, 8, 9) && seen_count == 1);

    /* Writes that keep the mask bits read the words under them: a held word
     * with its mask bit must survive the write. */
    CHECK(SoftGpu_Gp0(mask_check, 1) == 1);
    clear();
    hold(50, 60, 2, 2, 0x8123);
    SoftGpu_Load(50, 60, 2, 2, words);
    CHECK(settled(50, 60, 2, 2) && seen_count == 1 && at(50, 60) == 0x8123 && at(51, 61) == 0x8123);
    clear();
    hold(7, 8, 2, 1, 0x8124);
    CHECK(SoftGpu_Gp0(gp0_load, 4) == 4 && settled(7, 8, 2, 1) && at(7, 8) == 0x8124);
    clear();
    hold(100, 200, 5, 6, 0x8125);
    SoftGpu_Move(1, 2, 100, 200, 5, 6);
    CHECK(settled(1, 2, 5, 6) && settled(100, 200, 5, 6) && at(102, 203) == 0x8125);
    clear();
    SoftGpu_Fill(0, 0, 4, 4, 0); /* a fill ignores the mask */
    CHECK(seen_count == 0);
    CHECK(SoftGpu_Gp0(mask_off, 1) == 1);

    /* A primitive's texture, for host lookups keyed by it. */
    clear();
    SoftGpu_SettleTexture(0, 64, 256, 32, 480);
    CHECK(settled(64, 256, 64, 256) && settled(32, 480, 16, 1) && seen_count == 2);
    clear();
    SoftGpu_SettleTexture(1, 128, 0, 0, 500);
    CHECK(settled(128, 0, 128, 256) && settled(0, 500, 256, 1) && seen_count == 2);
    clear();
    SoftGpu_SettleTexture(2, 512, 256, 0, 0);
    CHECK(settled(512, 256, 256, 256) && seen_count == 1);
    clear();
    SoftGpu_Settle(0, 0, 0, 5); /* empty: nothing */
    CHECK(seen_count == 0);

    /* Banks are not drawn into: handing one out settles nothing. */
    clear();
    bank = SoftGpu_Bank(3);
    CHECK(bank && SoftGpu_BankPixels(3) == bank && seen_count == 0);

    /* The widened targets are rastered with their area. */
    SoftGpu_SetWidescreen(1);
    CHECK(SoftGpu_Gp0(lower, 3) == 3 && SoftGpu_Gp0(sprite, 5) == 5); /* makes the target */
    clear();
    CHECK(SoftGpu_WideFrameView(0, 256, 320, 240, &pixels, &x, &w) && settled(0, 256, 320, 240));
    clear();
    CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) && settled(0, 256, 320, 240));

    /* A new scale settles before the picture and the scale change, and the
     * new picture shows what was held. */
    SoftGpu_Fill(500, 300, 1, 1, 0);
    clear();
    hold(500, 300, 1, 1, 0x7fff);
    CHECK(SoftGpu_SetScale(2) && seen_count > 0 && scale_then == 1);
    CHECK(SoftGpu_Picture()[(300 * 2) * SOFT_GPU_WIDTH * 2 + 500 * 2] != 0);
    /* The software picture and the widened one, read, settle. */
    clear();
    CHECK(SoftGpu_Picture() && settled(WHOLE));
    CHECK(SoftGpu_Gp0(lower, 3) == 3 && SoftGpu_Gp0(sprite, 5) == 5);
    clear();
    CHECK(SoftGpu_WidePicture(0, 256, 320, 240) && settled(0, 256, 320, 240));
    /* A recorder settles before it is set, and its resync sees the held
     * words; while it draws the picture there is nothing of ours to read. */
    SoftGpu_Fill(500, 300, 1, 1, 0);
    clear();
    hold(500, 300, 1, 1, 0x1111);
    SoftGpu_SetRecorder(&recording);
    CHECK(seen_count > 0 && rastered_then == 1 && resync_saw == 0x1111);
    clear();
    CHECK(!SoftGpu_Picture() && seen_count == 0);
    CHECK(SoftGpu_Gp0(lower, 3) == 3 && SoftGpu_Gp0(sprite, 5) == 5);
    clear();
    CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) && seen_count == 0);
    /* PictureFromVram (a state load) settles before the resync. */
    clear();
    hold(500, 300, 1, 1, 0x2222);
    SoftGpu_PictureFromVram();
    CHECK(settled(WHOLE) && resync_saw == 0x2222);
    SoftGpu_SetRecorder(NULL);
    SoftGpu_SetWidescreen(0);
    CHECK(SoftGpu_SetScale(1));

    /* With the texture dump on, a textured primitive's page and palette are
     * settled before the dump reads them, polygon and sprite alike. */
    {
        /* Into the temporary folder, not wherever the test is run from. */
        const char *temp = getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : ".";
        char dump[1024];
        snprintf(dump, sizeof(dump), "%s/memories-soft-gpu-settle-dump", temp);
        CHECK(setenv("MEMORIES_DUMP_TEXTURES", dump, 1) == 0);
    }
    SoftGpu_Reset(); /* TextureDump_Init */
    CHECK(TextureDump_Enabled);
    CHECK(SoftGpu_Gp0(area, 3) == 3);
    clear();
    CHECK(SoftGpu_Gp0(sprite, 5) == 5 && settled(64, 0, 64, 256) && settled(256, 1, 16, 1));
    clear();
    CHECK(SoftGpu_Gp0(triangle, 7) == 7 && settled(64, 0, 64, 256) && settled(256, 1, 16, 1));
    TextureDump_Enabled = 0;

    SoftGpu_SettleObserver = NULL;
    puts("Software GPU settling checks passed");
    return 0;
}
