/* Settling (soft_gpu.h, notes/pc-build.md "Settling VRAM before it is
 * read"): every reader of rastered words inside the software GPU settles
 * exactly what it reads, and drawing alone settles nothing. The raster is
 * immediate, so this watches SoftGpu_SettleObserver; the host lookups
 * outside this library are checked from their source
 * (tools/pc/check_vram_readers.py). */
#include "pc/render/soft_gpu.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

#define SEEN_MAX 16
static int seen[SEEN_MAX][4], seen_count;

static void observe(int x, int y, int w, int h)
{
    if (seen_count < SEEN_MAX) {
        seen[seen_count][0] = x;
        seen[seen_count][1] = y;
        seen[seen_count][2] = w;
        seen[seen_count][3] = h;
    }
    seen_count++;
}

/* Whether the rectangle was among those settled since the last clear. */
static int settled(int x, int y, int w, int h)
{
    for (int i = 0; i < seen_count && i < SEEN_MAX; i++) {
        if (seen[i][0] == x && seen[i][1] == y && seen[i][2] == w && seen[i][3] == h) return 1;
    }
    return 0;
}

static void clear(void) { seen_count = 0; }

#define WHOLE 0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT

static void record_gp0(const uint32_t *words, size_t count) { (void)words; (void)count; }
static void record_load(int x, int y, int w, int h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
static void record_move(int x, int y, int dx, int dy, int w, int h)
{ (void)x; (void)y; (void)dx; (void)dy; (void)w; (void)h; }
static void record_fill(int x, int y, int w, int h, uint32_t c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
static void record_resync(int scale, const uint32_t state[6]) { (void)scale; (void)state; }
static const SoftGpuRecorder recording = {record_gp0, record_load, record_move, record_fill, record_resync};

int main(void)
{
    const uint32_t area[] = {0xe3000000u, 0xe4000000u | 319 | (239 << 10), 0xe5000000u};
    /* A 4-bit sprite from page 1 (64,0) through the palette at 256,1. */
    const uint32_t sprite[] = {0xe1000001u, 0x65808080u, 0x00200020u, 0x00500000u, 0x00020004u};
    const uint32_t mask_on[] = {0xe6000002u}, mask_off[] = {0xe6000000u}, mask_set[] = {0xe6000001u};
    const uint32_t gp0_move[] = {0x80000000u, 0x00030002u, 0x00410040u, 0x00050004u}; /* 2,3 -> 64,65, 4x5 */
    const uint32_t gp0_load[] = {0xa0000000u, 0x00080007u, 0x00010002u, 0x00000000u}; /* 2x1 at 7,8 */
    const uint32_t gp0_fill[] = {0x02000000u, 0x00000000u, 0x00100010u};
    uint16_t words[64] = {0}, *bank;
    size_t size;
    const uint16_t *pixels;
    int x, w;

    SoftGpu_Reset();
    SoftGpu_SettleObserver = observe;

    /* Drawing reads nothing outside the raster: no settling. */
    CHECK(SoftGpu_Gp0(area, 3) == 3);
    clear();
    CHECK(SoftGpu_Gp0(sprite, 5) == 5 && seen_count == 0);
    CHECK(SoftGpu_Gp0(gp0_fill, 3) == 3 && seen_count == 0);
    SoftGpu_Fill(0, 0, 8, 8, 0x00ff00);
    SoftGpu_Load(4, 4, 2, 2, words);
    CHECK(seen_count == 0);

    /* Whole-VRAM readers. */
    clear();
    CHECK(SoftGpu_Vram() && settled(WHOLE) && seen_count == 1);
    clear();
    pixels = SoftGpu_VramRaw(); /* the raw one settles nothing */
    CHECK(seen_count == 0 && pixels == SoftGpu_Vram() && seen_count == 1);
    clear();
    CHECK(SoftGpu_StateData(0, &size) && size == SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * 2 && settled(WHOLE));
    clear();
    CHECK(SoftGpu_StateData(1, &size) && seen_count == 0); /* the drawing state alone */
    clear();
    SoftGpu_PictureFromVram();
    CHECK(settled(WHOLE));

    /* Region readers: StoreImage, a move's source (both ways in), the
     * capture after a StoreImage. */
    clear();
    SoftGpu_Store(10, 20, 4, 3, words);
    CHECK(settled(10, 20, 4, 3) && seen_count == 1);
    clear();
    SoftGpu_Move(1, 2, 100, 200, 5, 6);
    CHECK(settled(1, 2, 5, 6) && seen_count == 1);
    clear();
    CHECK(SoftGpu_Gp0(gp0_move, 4) == 4 && settled(2, 3, 4, 5) && seen_count == 1);
    clear();
    SoftGpu_Move(0, 0, 0, 0, 2, 1); /* onto itself: the game's every frame */
    CHECK(seen_count == 0);
    clear();
    CHECK(SoftGpu_Gp0(mask_set, 1) == 1);
    SoftGpu_Move(0, 0, 0, 0, 2, 1); /* ... but with the mask bit set it changes them */
    CHECK(settled(0, 0, 2, 1));
    CHECK(SoftGpu_Gp0(mask_off, 1) == 1);
    clear();
    SoftGpu_Capture(30, 40, 320, 256, 8, 9);
    CHECK(settled(30, 40, 8, 9) && seen_count == 1);

    /* Writes that keep the mask bits read the words under them. */
    CHECK(SoftGpu_Gp0(mask_on, 1) == 1);
    clear();
    SoftGpu_Load(50, 60, 2, 2, words);
    CHECK(settled(50, 60, 2, 2) && seen_count == 1);
    clear();
    CHECK(SoftGpu_Gp0(gp0_load, 4) == 4 && settled(7, 8, 2, 1) && seen_count == 1);
    clear();
    SoftGpu_Move(1, 2, 100, 200, 5, 6);
    CHECK(settled(1, 2, 5, 6) && settled(100, 200, 5, 6) && seen_count == 2);
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
    {
        const uint32_t lower[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10), 0xe5000000u | (256 << 11)};
        SoftGpu_SetWidescreen(1);
        CHECK(SoftGpu_Gp0(lower, 3) == 3 && SoftGpu_Gp0(sprite, 5) == 5); /* makes the target */
        clear();
        CHECK(SoftGpu_WideFrameView(0, 256, 320, 240, &pixels, &x, &w) && settled(0, 256, 320, 240));
        clear();
        CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) && settled(0, 256, 320, 240));
        SoftGpu_SetWidescreen(0);
    }

    /* The software picture is drawn with the words: read, it settles them;
     * a recorder draws it instead (no picture here), and its resync takes
     * VRAM whole. */
    CHECK(SoftGpu_SetScale(2));
    clear();
    CHECK(SoftGpu_Picture() && settled(WHOLE));
    clear();
    SoftGpu_SetRecorder(&recording);
    CHECK(settled(WHOLE));
    clear();
    CHECK(!SoftGpu_Picture() && seen_count == 0);
    SoftGpu_SetRecorder(NULL);
    CHECK(SoftGpu_SetScale(1));

    SoftGpu_SettleObserver = NULL;
    puts("Software GPU settling checks passed");
    return 0;
}
