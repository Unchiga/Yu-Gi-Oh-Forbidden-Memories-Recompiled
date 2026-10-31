#include "pc/render/soft_gpu.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
#define AT(x, y) (SoftGpu_Vram()[(y) * SOFT_GPU_WIDTH + (x)])

static int count(uint16_t color)
{
    int n = 0, i;
    for (i = 0; i < SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT; i++) {
        n += SoftGpu_Vram()[i] == color;
    }
    return n;
}

/* A recorder suppresses the normal software picture, as the GL backend
 * does. Widescreen must still have its own scaled rendering in that mode. */
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
    /* Flat quad (2,3)-(12,13): top-left rule gives exactly 10x10 pixels, and
     * the shared diagonal is drawn once (semi-transparency would show twice). */
    const uint32_t quad[] = {0xe1000000u, 0x2a0000f8u, 0x00030002u, 0x0003000cu, 0x000d0002u, 0x000d000cu};
    /* 4bpp sprite through a CLUT, texel 0 transparent, raw colors. */
    const uint32_t sprite[] = {0xe1000001u, 0x65808080u, 0x00200020u, 0x00500000u, 0x00020004u};
    const uint16_t clut[16] = {0, 0x001f, 0x03e0, 0x7c00};
    const uint16_t texture[2] = {0x3210, 0x0123};
    const uint32_t clip[] = {0xe3000000u | 100 | (100 << 10), 0xe4000000u | 103 | (101 << 10),
                             0xe5000000u | 90 | (90 << 11), 0x60ffffffu, 0x00000000u, 0x00400040u};
    const uint32_t cut[] = {0x2c808080u, 0, 0};
    SoftGpu_Reset();
    CHECK(SoftGpu_Gp0(quad, 6) == 6);
    /* averaged with black: 31 >> 1 = 15; a doubly drawn diagonal would be 23 */
    CHECK(count(15) == 100 && AT(2, 3) == 15 && AT(11, 12) == 15 && AT(12, 12) == 0 && AT(11, 13) == 0);

    SoftGpu_Load(16 * 16, 1, 16, 1, clut);
    SoftGpu_Load(64, 0, 2, 1, texture);
    CHECK(SoftGpu_Gp0(sprite, 5) == 5);
    CHECK(AT(32, 32) == 0 && AT(33, 32) == 0x001f && AT(34, 32) == 0x03e0 && AT(35, 32) == 0x7c00);
    CHECK(AT(32, 33) == 0 && AT(36, 32) == 0);

    CHECK(SoftGpu_Gp0(clip, 6) == 6);
    CHECK(count(0x7fff) == 8 && AT(100, 100) == 0x7fff && AT(103, 101) == 0x7fff);
    CHECK(SoftGpu_Gp0(cut, 3) == 0); /* truncated FT4 is not drawn */
    SoftGpu_Fill(0, 0, 4, 4, 0xff0000);
    SoftGpu_Move(0, 0, 1020, 510, 4, 4); /* wraps both axes */
    CHECK(AT(1023, 511) == 0x7c00 && AT(1, 1) == 0x7c00);

    {
        /* Widescreen: a 320x240 drawing area at (0,256) gets a target 54
         * wider each side. A sprite from x=-20 keeps the 4:3 clip in the
         * target too (2D screens park things past the edge), while a quad
         * from x=-20 lands in the target's left side; a full-width fill
         * fills the sides too. */
        const uint32_t area[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10),
                                 0xe5000000u | (256 << 11)};
        const uint32_t fill[] = {0x020000ffu, 0x01000000u, (240u << 16) | 320};
        const uint32_t wide_sprite[] = {0x60ffffffu, 0x000507ecu, 0x0002001eu};
        const uint32_t wide_quad[] = {0x28ffffffu, 0x000a07ecu, 0x000a000au, 0x000c07ecu, 0x000c000au};
        const uint16_t *pixels;
        int x, w;
        SoftGpu_SetWidescreen(1);
        CHECK(SoftGpu_Gp0(area, 3) == 3);
        CHECK(SoftGpu_Gp0(wide_sprite, 3) == 3); /* makes the target */
        CHECK(SoftGpu_Gp0(fill, 3) == 3);
        CHECK(SoftGpu_Gp0(wide_sprite, 3) == 3);
        CHECK(SoftGpu_Gp0(wide_quad, 5) == 5);
        CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) && x == 0 && w == 428);
        CHECK(AT(0, 261) == 0x7fff && AT(9, 261) == 0x7fff && AT(10, 261) == 0x001f);
        CHECK(pixels[261 * SOFT_GPU_WIDTH + 54] == 0x7fff && pixels[261 * SOFT_GPU_WIDTH + 53] == 0x001f);
        CHECK(AT(0, 266) == 0x7fff && AT(10, 266) == 0x001f);
        CHECK(pixels[266 * SOFT_GPU_WIDTH + 34] == 0x7fff && pixels[266 * SOFT_GPU_WIDTH + 33] == 0x001f);
        CHECK(pixels[300 * SOFT_GPU_WIDTH + 0] == 0x001f && pixels[300 * SOFT_GPU_WIDTH + 427] == 0x001f);
        CHECK(pixels[300 * SOFT_GPU_WIDTH + 54 + 100] == AT(100, 300));
        /* Shown again with nothing drawn since: the stale sides go black. */
        CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) && pixels[300 * SOFT_GPU_WIDTH] == 0);
        SoftGpu_SetWidescreen(0);
        CHECK(!SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w));
    }
    for (int recorded = 0; recorded < 2; recorded++) {
        SoftGpu_SetRecorder(recorded ? &recording : NULL);
        SoftGpu_SetWidescreen(1);
        for (int scale = 2; scale <= 4; scale *= 2) {
            const uint32_t area[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10),
                                     0xe5000000u | (256 << 11)};
            /* Sloped triangle extending into the left border. Its diagonal
             * must resolve within a console pixel, not just enlarge 1x. */
            const uint32_t triangle[] = {0x200000ffu, 0x000a07ecu, 0x000a000au, 0x002807ecu};
            const uint16_t *words;
            const uint32_t *pic;
            int x, w, stride = SOFT_GPU_WIDTH * scale;
            CHECK(SoftGpu_SetScale(scale));
            SoftGpu_Reset();
            CHECK(SoftGpu_Gp0(area, 3) == 3);
            /* Make the target, clear it, then draw against black. */
            CHECK(SoftGpu_Gp0(triangle, 4) == 4);
            SoftGpu_Fill(0, 256, 320, 240, 0);
            CHECK(SoftGpu_Gp0(triangle, 4) == 4);
            CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &words, &x, &w) && w == 428);
            pic = SoftGpu_WidePicture(0, 256, 320, 240);
            if (recorded) {
                /* The recorder draws the widened picture too (gl_picture.c);
                 * the 1x target is still kept here. */
                CHECK(!pic);
                continue;
            }
            CHECK(pic);
            CHECK(pic[(size_t)(270 * scale) * stride + 36 * scale] == 0xff0000);
            /* The diagonal x+y=330 is red just before the edge and black
             * on it, inside the same native pixel at (53,276). */
            CHECK(pic[(size_t)(276 * scale) * stride + 53 * scale] == 0xff0000);
            CHECK(pic[(size_t)(277 * scale - 1) * stride + 54 * scale - 1] == 0);
            if (!recorded) {
                const uint32_t *normal = SoftGpu_Picture();
                CHECK(normal[(size_t)(270 * scale) * stride + 36 * scale] == 0);
            }
            /* Transfers seed the centre and full-width clears reach sides. */
            {
                const uint16_t green = 0x03e0;
                SoftGpu_Load(100, 300, 1, 1, &green);
                CHECK(pic[(size_t)(300 * scale) * stride + 154 * scale] == 0x00ff00);
                SoftGpu_Move(100, 300, 101, 300, 1, 1);
                CHECK(pic[(size_t)(300 * scale) * stride + 155 * scale] == 0x00ff00);
            }
            SoftGpu_Fill(0, 256, 320, 240, 0xff0000);
            CHECK(pic[(size_t)(300 * scale) * stride] == 0x0000ff);
            CHECK(pic[(size_t)(300 * scale) * stride + 428 * scale - 1] == 0x0000ff);
            CHECK(SoftGpu_WideFrame(0, 256, 320, 240, &words, &x, &w));
            CHECK(pic[(size_t)(300 * scale) * stride] == 0);
        }
        CHECK(SoftGpu_SetScale(1));
        CHECK(!SoftGpu_WidePicture(0, 256, 320, 240));
        SoftGpu_SetWidescreen(0);
    }
    SoftGpu_SetRecorder(NULL);

    /* Texture banks: a polygon's page word names 1-15 by bits 11-14 and
     * 16-31 with bit 10 too; an E1 word's bit 10 names no bank. */
    {
        const uint32_t area[] = {0xe3000000u, 0xe4000000u | 1023 | (511 << 10), 0xe5000000u};
        uint32_t triangle[] = {0x25808080u, 200 | (200 << 16), 0, 204 | (200 << 16), 0, 200 | (204 << 16), 0};
        const uint32_t page = 0xe1000000u | 0x100 | (1 << 10) | SOFT_GPU_BANK_TPAGE(1);
        const uint32_t sprite15[] = {0x65808080u, 210 | (210 << 16), 0, 2 | (2 << 16)};
        const uint16_t blue = 0x7c00;
        uint16_t *seventeen = SoftGpu_Bank(17), *one = SoftGpu_Bank(1);
        uint32_t state[6];
        SoftGpu_Reset();
        CHECK(SoftGpu_Gp0(area, 3) == 3);
        CHECK(seventeen && one && SoftGpu_BankPixels(17) == seventeen && !SoftGpu_BankPixels(18));
        CHECK(SOFT_GPU_BANK_TPAGE(17) == ((1u << 11) | (1u << 10)) && SOFT_GPU_BANK_TPAGE(15) == 15u << 11);
        seventeen[0] = 0x001f;
        one[0] = 0x03e0;
        SoftGpu_Load(0, 0, 1, 1, &blue);
        triangle[4] = (0x100 | SOFT_GPU_BANK_TPAGE(17)) << 16;
        CHECK(SoftGpu_Gp0(triangle, 7) == 7 && AT(201, 201) == 0x001f);
        SoftGpu_StateWords(state);
        CHECK(((state[0] >> 11) & 15) == 0); /* an E1 cannot name 17 */
        triangle[4] = (0x100 | SOFT_GPU_BANK_TPAGE(1)) << 16;
        CHECK(SoftGpu_Gp0(triangle, 7) == 7 && AT(201, 201) == 0x03e0);
        triangle[4] = (0x100 | SOFT_GPU_BANK_TPAGE(18)) << 16; /* never made: VRAM */
        CHECK(SoftGpu_Gp0(triangle, 7) == 7 && AT(201, 201) == blue);
        CHECK(SoftGpu_Gp0(&page, 1) == 1 && SoftGpu_Gp0(sprite15, 4) == 4 && AT(210, 210) == 0x03e0);
    }
    puts("Software GPU fill-rule, CLUT, clip, transfer, widescreen and texture bank checks passed");
    return 0;
}
