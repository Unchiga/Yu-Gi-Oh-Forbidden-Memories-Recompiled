#include "pc/render/soft_gpu.h"
#include <stdio.h>
#include <string.h>

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

/* --- MoveImage with the mask check (the scaled picture follows the words) --- */

static uint32_t expand_word(uint16_t c)
{
    uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static uint32_t pic_pixel(int scale, int x, int y, int px, int py)
{
    return SoftGpu_Picture()[(size_t)(y * scale + py) * SOFT_GPU_WIDTH * scale + (size_t)(x * scale + px)];
}

/* The word's scale x scale picture block is `color` throughout. */
static int block_is(int scale, int x, int y, uint32_t color)
{
    int px, py;
    for (py = 0; py < scale; py++) {
        for (px = 0; px < scale; px++) {
            if (pic_pixel(scale, x, y, px, py) != color) return 0;
        }
    }
    return 1;
}

/* Two words' blocks are pixel for pixel the same. */
static int blocks_equal(int scale, int ax, int ay, int bx, int by)
{
    int px, py;
    for (py = 0; py < scale; py++) {
        for (px = 0; px < scale; px++) {
            if (pic_pixel(scale, ax, ay, px, py) != pic_pixel(scale, bx, by, px, py)) return 0;
        }
    }
    return 1;
}

/* Black VRAM and picture, mask state off, then a flat red triangle whose
 * diagonal edge splits some words' blocks. Finds one (its block is not one
 * colour: the picture holds what the words cannot). */
static int make_split_word(int scale, int *wx, int *wy)
{
    const uint32_t triangle[] = {0x200000ffu, (300u << 16) | 400, (300u << 16) | 420, (320u << 16) | 400};
    const uint32_t off = 0xe6000000u;
    int x, y;
    if (!SoftGpu_SetScale(scale)) return 0;
    SoftGpu_Reset();
    SoftGpu_Fill(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, 0);
    if (SoftGpu_Gp0(&off, 1) != 1 || SoftGpu_Gp0(triangle, 4) != 4) return 0;
    for (y = 300; y < 320; y++) {
        for (x = 400; x < 420; x++) {
            if (!block_is(scale, x, y, pic_pixel(scale, x, y, 0, 0))) {
                *wx = x;
                *wy = y;
                return 1;
            }
        }
    }
    return 0;
}

/* One word of the picture moved with the mask check on or off, `set` the
 * mask-set bit, the destination word `dest` first. */
static int move_one_word(int scale, int set, int check, uint16_t dest, int expect_written, int expect_equal_src)
{
    int sx, sy, dx, dy;
    uint16_t src;
    uint32_t state[1];
    if (!make_split_word(scale, &sx, &sy)) return 0;
    src = AT(sx, sy);
    dx = sx + 30;
    dy = sy;
    SoftGpu_Load(dx, dy, 1, 1, &dest);
    state[0] = 0xe6000000u | (uint32_t)set | ((uint32_t)check << 1);
    if (SoftGpu_Gp0(state, 1) != 1) return 0;
    SoftGpu_Move(sx, sy, dx, dy, 1, 1);
    if (expect_written) {
        if (AT(dx, dy) != (uint16_t)(src | (set ? 0x8000 : 0))) return 0;
        if (!blocks_equal(scale, sx, sy, dx, dy)) return 0; /* its pixels went with it */
    } else {
        if (AT(dx, dy) != dest) return 0;
        /* kept: what the destination's picture was, one colour */
        if (!block_is(scale, dx, dy, expand_word(dest))) return 0;
        if (expect_equal_src && blocks_equal(scale, sx, sy, dx, dy)) return 0;
    }
    return 1;
}

/* An overlapping run of words with the mask check on, a few of them already
 * kept: the words and the picture must end up the same, word by word, as a
 * copy that reads each row forwards leaves them. */
static int move_overlapping(int scale, int set, int left, int wide)
{
    enum { N = 9, X = 300, Y = 300 };
    uint16_t words[N], model[N];
    uint32_t state[1] = {0xe6000000u | (uint32_t)set | 2u};
    int k, sx, dx, i;
    if (!SoftGpu_SetScale(scale)) return 0;
    SoftGpu_Reset();
    if (wide) {
        /* A widescreen target for the drawing area (0,256) 320x240 which holds
         * the words, made by a first sprite: the move runs with it present. */
        const uint32_t area[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10),
                                 0xe5000000u | (256 << 11)};
        const uint32_t sprite[] = {0x60ffffffu, 0x0102000au, 0x00020002u};
        SoftGpu_SetWidescreen(1);
        if (SoftGpu_Gp0(area, 3) != 3 || SoftGpu_Gp0(sprite, 3) != 3) return 0;
    }
    for (k = 0; k < N; k++) {
        words[k] = (uint16_t)((k + 1) * 3 | (((k + 1) * 2) << 5) | ((k + 1) << 10));
        if (k == 3 || k == 5) words[k] |= 0x8000; /* a kept word when a destination */
        model[k] = words[k];
    }
    SoftGpu_Load(X, Y, N, 1, words);
    if (SoftGpu_Gp0(state, 1) != 1) return 0;
    sx = left ? X + 1 : X;
    dx = left ? X : X + 1;
    SoftGpu_Move(sx, Y, dx, Y, N - 1, 1);
    for (i = 0; i < N - 1; i++) {
        uint16_t *d = &model[dx - X + i];
        if (!(*d & 0x8000)) *d = (uint16_t)(model[sx - X + i] | (set ? 0x8000 : 0));
    }
    for (k = 0; k < N; k++) {
        if (AT(X + k, Y) != model[k]) return 0;
        if (!block_is(scale, X + k, Y, expand_word(model[k]))) return 0;
    }
    if (wide) {
        /* The widened picture's centre holds the same words (it is seeded from
         * VRAM, which is right either way), and the target is still there. */
        const uint16_t *pixels;
        const uint32_t *picture;
        int x, w, ok = 1;
        if (!SoftGpu_WideFrame(0, 256, 320, 240, &pixels, &x, &w) || w != 428) ok = 0;
        picture = ok ? SoftGpu_WidePicture(0, 256, 320, 240) : NULL;
        for (k = 0; ok && k < N; k++) {
            if (pixels[Y * SOFT_GPU_WIDTH + 54 + X + k] != model[k]) ok = 0;
            if (picture && picture[(size_t)(Y * scale) * SOFT_GPU_WIDTH * scale + (size_t)((54 + X + k) * scale)] !=
                               expand_word(model[k]))
                ok = 0;
        }
        SoftGpu_SetWidescreen(0);
        if (!ok) return 0;
    }
    return 1;
}

/* --- MoveImage with the mask check against a model of the hardware copy ---
 * Every word the move reads or writes is taken as a cell: its word and its
 * scale x scale picture block. The model copies cell to cell, row by row and
 * word by word forwards (a kept word, one with bit 15 set, is left alone), and
 * the move's result must be the same in the words and in every pixel of the
 * picture. A cell is found by its wrapped coordinates, so a copy wrapping past
 * x 1023 and y 511 is the same code. */
typedef struct Cell {
    int x, y;
    uint16_t word;
    uint32_t pixels[64];
} Cell;

static Cell cells[512];
static int cell_count;

static unsigned rnd(void)
{
    static unsigned state = 12345;
    state = state * 1103515245u + 12345u;
    return (state >> 16) & 0x7fff;
}

static Cell *cell_at(int scale, int x, int y)
{
    int i, px, py;
    Cell *cell;
    x &= SOFT_GPU_WIDTH - 1;
    y &= SOFT_GPU_HEIGHT - 1;
    for (i = 0; i < cell_count; i++) {
        if (cells[i].x == x && cells[i].y == y) return &cells[i];
    }
    cell = &cells[cell_count++];
    cell->x = x;
    cell->y = y;
    cell->word = AT(x, y);
    for (py = 0; py < scale; py++) {
        for (px = 0; px < scale; px++) cell->pixels[py * scale + px] = pic_pixel(scale, x, y, px, py);
    }
    return cell;
}

/* A flat triangle of a random colour in a rectangle, drawn with the mask-set
 * bit on or off; its diagonal edge splits some words' blocks. */
static void seed_triangle(int x, int y, int w, int h, int set)
{
    uint32_t state = 0xe6000000u | (uint32_t)set;
    uint32_t tri[4];
    int k;
    tri[0] = 0x20000000u | (((rnd() << 9) + rnd()) & 0xffffffu);
    for (k = 1; k < 4; k++) {
        tri[k] = ((uint32_t)(y - 1 + (int)(rnd() % (unsigned)(h + 2))) << 16) |
                 (uint32_t)(x - 1 + (int)(rnd() % (unsigned)(w + 2)));
    }
    SoftGpu_Gp0(&state, 1);
    SoftGpu_Gp0(tri, 4);
}

/* detail: the source and destination words come from primitives, so their
 * blocks are not one colour; otherwise from random loads. Without detail a
 * rectangle may wrap; with it the rectangles stay inside VRAM (a primitive
 * does not wrap). Returns 1 if the result is the model's. */
#define NO(why) (fprintf(stderr, "move_model: why %d, scale %d, set %d, detail %d, from %d,%d to %d,%d %dx%d\n", (why), scale, set, detail, sx, sy, dx, dy, w, h), 0)
static int move_model(int scale, int set, int sx, int sy, int dx, int dy, int w, int h, int detail)
{
    uint32_t off = 0xe6000000u, state = 0xe6000000u | (uint32_t)set | 2u;
    int rect[2][2] = {{sx, sy}, {dx, dy}};
    int r, i, j, t, split = 0, kept = 0, written = 0;
    cell_count = 0;
    SoftGpu_Gp0(&off, 1);
    for (r = 0; r < 2; r++) {
        int x = rect[r][0], y = rect[r][1];
        SoftGpu_Fill(x, y, w, h, 0);
        if (detail) {
            for (t = 0; t < 4; t++) seed_triangle(x, y, w, h, t & 1);
            /* and last a mask-set triangle over the rectangle's left half, so
             * that there are kept words as well as written ones */
            {
                uint32_t on = 0xe6000001u, tri[4] = {0x20ffffffu, 0, 0, 0};
                tri[1] = ((uint32_t)y << 16) | (uint32_t)x;
                tri[2] = ((uint32_t)y << 16) | (uint32_t)(x + w / 2 + 1);
                tri[3] = ((uint32_t)(y + h) << 16) | (uint32_t)x;
                SoftGpu_Gp0(&on, 1);
                SoftGpu_Gp0(tri, 4);
            }
        } else {
            for (j = 0; j < h; j++) {
                for (i = 0; i < w; i++) {
                    uint16_t word = (uint16_t)(rnd() | ((rnd() & 1) ? 0x8000 : 0));
                    SoftGpu_Load(x + i, y + j, 1, 1, &word);
                }
            }
        }
    }
    SoftGpu_Gp0(&off, 1);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            Cell *src = cell_at(scale, sx + i, sy + j), *dst = cell_at(scale, dx + i, dy + j);
            int k;
            for (k = 1; k < scale * scale; k++) split += src->pixels[k] != src->pixels[0];
            kept += (dst->word & 0x8000) != 0;
        }
    }
    if (detail && !split) return NO(1); /* the case would prove nothing */
    if (!kept) return NO(2);
    if (SoftGpu_Gp0(&state, 1) != 1) return NO(3);
    SoftGpu_Move(sx, sy, dx, dy, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            Cell *src = cell_at(scale, sx + i, sy + j), *dst = cell_at(scale, dx + i, dy + j);
            if (!(dst->word & 0x8000)) {
                dst->word = (uint16_t)(src->word | (set ? 0x8000 : 0));
                memcpy(dst->pixels, src->pixels, sizeof(dst->pixels));
                written++;
            }
        }
    }
    if (!written) return NO(4);
    for (i = 0; i < cell_count; i++) {
        int px, py;
        if (AT(cells[i].x, cells[i].y) != cells[i].word) return NO(5);
        for (py = 0; py < scale; py++) {
            for (px = 0; px < scale; px++) {
                if (pic_pixel(scale, cells[i].x, cells[i].y, px, py) != cells[i].pixels[py * scale + px]) return NO(6);
            }
        }
    }
    SoftGpu_Gp0(&off, 1);
    return 1;
}

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
    /* Global fades use full-width flat rectangles, including 8-line wipe
     * bands. Both directions and tinted/additive flashes must reach every
     * side pixel, once, at native and scaled resolutions. Smaller UI
     * rectangles and textured sprites keep their existing clipping. */
    for (int scale = 1; scale <= 4; scale *= 2) {
        const uint32_t area[] = {0xe3000000u | (256 << 10), 0xe4000000u | 319 | (495 << 10),
                                 0xe5000000u | (256 << 11)};
        const uint32_t make_target[] = {0x60000000u, 0, 0x00010001u};
        const uint32_t band[] = {0xe1000040u, 0x62404040u, 0x00180000u, (8u << 16) | 320};
        const uint32_t tint[] = {0xe1000020u, 0x62000020u, 0, (240u << 16) | 320};
        const uint32_t black[] = {0xe1000040u, 0x62ffffffu, 0, (240u << 16) | 320};
        const uint32_t white[] = {0xe1000020u, 0x62ffffffu, 0, (240u << 16) | 320};
        const uint16_t *pixels;
        const uint32_t *picture;
        int x, w;
        CHECK(SoftGpu_SetScale(scale));
        SoftGpu_Reset();
        SoftGpu_SetWidescreen(1);
        CHECK(SoftGpu_Gp0(area, 3) == 3);
        CHECK(SoftGpu_Gp0(make_target, 3) == 3);
        SoftGpu_Fill(0, 256, 320, 240, 0xffffff);
        CHECK(SoftGpu_Gp0(band, 4) == 4);
        CHECK(SoftGpu_WideFrameView(0, 256, 320, 240, &pixels, &x, &w) && w == 428);
        picture = SoftGpu_WidePicture(0, 256, 320, 240);
        for (int column = 0; column < w; column++) {
            CHECK(pixels[280 * SOFT_GPU_WIDTH + column] == AT(0, 280));
            CHECK(pixels[279 * SOFT_GPU_WIDTH + column] == 0x7fff);
            CHECK(pixels[288 * SOFT_GPU_WIDTH + column] == 0x7fff);
            if (picture) CHECK(picture[(size_t)(280 * scale) * SOFT_GPU_WIDTH * scale + column * scale] ==
                               picture[(size_t)(280 * scale) * SOFT_GPU_WIDTH * scale + 160 * scale]);
        }
        CHECK(AT(0, 280) != AT(0, 279));
        CHECK(SoftGpu_Gp0(black, 4) == 4);
        for (int column = 0; column < w; column++) CHECK(pixels[300 * SOFT_GPU_WIDTH + column] == 0);
        CHECK(SoftGpu_Gp0(tint, 4) == 4);
        for (int column = 0; column < w; column++) CHECK(pixels[300 * SOFT_GPU_WIDTH + column] == 4);
        CHECK(SoftGpu_Gp0(white, 4) == 4);
        for (int column = 0; column < w; column++) CHECK(pixels[300 * SOFT_GPU_WIDTH + column] == 0x7fff);
        SoftGpu_SetWidescreen(0);
    }
    CHECK(SoftGpu_SetScale(1));

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
    /* MoveImage with the mask check: the picture follows exactly the words
     * the move wrote. Not at once the pixel() of the draw target, not
     * src|0x8000 whatever the mask-set bit says, not after the words have
     * moved. The state (E6) is what the game's mask-write packet sets: bit 0
     * set, bit 1 check. */
    SoftGpu_SetRecorder(NULL);
    SoftGpu_SetWidescreen(0);
    for (int scale = 2; scale <= 4; scale *= 2) {
        /* check on, set off: a written word equals its source */
        CHECK(move_one_word(scale, 0, 1, 0x0000, 1, 0));
        /* check on, set on: a written word is its source | 0x8000 */
        CHECK(move_one_word(scale, 1, 1, 0x0000, 1, 0));
        /* a kept word's pixels stay */
        CHECK(move_one_word(scale, 0, 1, 0x83e0, 0, 1));
        CHECK(move_one_word(scale, 1, 1, 0x83e0, 0, 1));
        /* a kept word that happens to equal source|0x8000 stays too */
        {
            int sx, sy;
            uint16_t src;
            CHECK(make_split_word(scale, &sx, &sy));
            src = AT(sx, sy);
            CHECK(move_one_word(scale, 0, 1, (uint16_t)(src | 0x8000), 0, 1));
            CHECK(move_one_word(scale, 1, 1, (uint16_t)(src | 0x8000), 0, 1));
        }
        /* no check: every word is written, as before */
        CHECK(move_one_word(scale, 0, 0, 0x83e0, 1, 0));
        CHECK(move_one_word(scale, 1, 0, 0x83e0, 1, 0));
        /* overlapping copies, rightwards and leftwards, with kept words */
        CHECK(move_overlapping(scale, 0, 0, 0));
        CHECK(move_overlapping(scale, 1, 0, 0));
        CHECK(move_overlapping(scale, 0, 1, 0));
        CHECK(move_overlapping(scale, 1, 1, 0));
        /* the same with a widescreen target for the area the words are in */
        CHECK(move_overlapping(scale, 0, 0, 1));
        CHECK(move_overlapping(scale, 1, 1, 1));
    }
    /* The same against a model of the copy: wrapping past x 1023 and y 511,
     * several rows overlapping each way, and source blocks that hold subpixel
     * detail (primitives rasterized at the scale, not uniform loads), mask
     * check on, set on and off, up to 8x. */
    for (int scale = 2; scale <= 8; scale *= 2) {
        CHECK(SoftGpu_SetScale(scale));
        SoftGpu_Reset();
        for (int set = 0; set < 2; set++) {
            for (int detail = 0; detail < 2; detail++) {
                CHECK(move_model(scale, set, 300, 300, 300, 303, 8, 6, detail));  /* dy > sy */
                CHECK(move_model(scale, set, 300, 303, 300, 300, 8, 6, detail));  /* dy < sy */
                CHECK(move_model(scale, set, 300, 300, 302, 302, 8, 6, detail));  /* dx > sx, dy > sy */
                CHECK(move_model(scale, set, 302, 302, 300, 300, 8, 6, detail));  /* dx < sx, dy < sy */
                CHECK(move_model(scale, set, 302, 300, 300, 302, 8, 6, detail));  /* dx < sx, dy > sy */
                CHECK(move_model(scale, set, 300, 302, 302, 300, 8, 6, detail));  /* dx > sx, dy < sy */
                CHECK(move_model(scale, set, 300, 300, 303, 300, 10, 3, detail)); /* along a row, rightwards */
                CHECK(move_model(scale, set, 303, 300, 300, 300, 10, 3, detail)); /* leftwards */
            }
            /* past x 1023 and y 511, which wrap (words from random loads) */
            CHECK(move_model(scale, set, 1020, 508, 8, 100, 8, 8, 0));    /* the source wraps */
            CHECK(move_model(scale, set, 100, 100, 1020, 508, 8, 8, 0));  /* the destination wraps */
            CHECK(move_model(scale, set, 1019, 509, 1021, 510, 8, 6, 0)); /* both, overlapping */
            CHECK(move_model(scale, set, 1021, 510, 1019, 509, 8, 6, 0));
            CHECK(move_model(scale, set, 1018, 2, 1022, 2, 12, 4, 0));    /* along a row across the edge */
        }
    }

    {
        uint32_t off = 0xe6000000u;
        CHECK(SoftGpu_Gp0(&off, 1) == 1);
        CHECK(SoftGpu_SetScale(1));
    }
    puts("Software GPU fill-rule, CLUT, clip, transfer, widescreen, masked move and texture bank checks passed");
    return 0;
}
