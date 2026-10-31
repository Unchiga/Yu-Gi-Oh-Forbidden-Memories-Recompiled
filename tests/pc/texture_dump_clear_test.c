/* Compare bulk clears with VRAM's cell-by-cell wrapping contract, on native
 * 32/64-bit hosts without the translated runtime, and the clean blocks the
 * clears pass over (TextureDump_Clean). */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "pc/render/texture_dump.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WORDS (SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT)
static uint32_t tags[WORDS + 2];
static uint16_t shadow[WORDS * 4 + 2];
static unsigned char cleared[WORDS];
static int forget_calls, expected_x, expected_y, expected_w, expected_h;

const uint16_t *SoftGpu_Vram(void) { return NULL; }

static void forget(int x, int y, int w, int h)
{
    assert(x == expected_x && y == expected_y);
    assert(w == expected_w && h == expected_h);
    forget_calls++;
}

static void check_clear(int x, int y, int w, int h, int with_shadow)
{
    size_t i;
    TextureDump_Tags = tags + 1;
    TextureDump_Shadow = with_shadow ? shadow + 1 : NULL;
    for (i = 0; i < WORDS + 2; i++) tags[i] = 11;
    for (i = 0; i < WORDS * 4 + 2; i++) shadow[i] = 17;
    TextureDump_Touched(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    memset(cleared, 0, sizeof(cleared));
    for (int row = 0; row < h; row++) {
        for (int column = 0; column < w; column++) {
            unsigned cx = (unsigned)(x + column) % SOFT_GPU_WIDTH;
            unsigned cy = (unsigned)(y + row) % SOFT_GPU_HEIGHT;
            cleared[cy * SOFT_GPU_WIDTH + cx] = 1;
        }
    }
    expected_x = x; expected_y = y; expected_w = w; expected_h = h;
    forget_calls = 0;
    TextureDump_Cleared(x, y, w, h);
    assert(forget_calls == 1);
    assert(tags[0] == 11 && tags[WORDS + 1] == 11);
    assert(shadow[0] == 17 && shadow[WORDS * 4 + 1] == 17);
    for (i = 0; i < WORDS; i++) {
        assert(tags[i + 1] == (cleared[i] ? 0u : 11u));
        for (size_t sub = 0; sub < 4; sub++)
            assert(shadow[1 + i * 4 + sub] ==
                   (with_shadow && cleared[i] ? 0 : 17));
    }
}

static int clean(int block, int row) { return TextureDump_Clean[row * TEXTURE_CLEAN_COLUMNS + block]; }

/* A clear passes over the blocks it covers whole that are clean: a word
 * written there without TextureDump_Touched stays (which no writer does).
 * Touched blocks, and blocks a clear covers in part, are cleared. */
static void check_clean(void)
{
    uint32_t *tag = TextureDump_Tags;
    TextureDump_Shadow = shadow + 1;
    expected_x = 0; expected_y = 0; expected_w = SOFT_GPU_WIDTH; expected_h = SOFT_GPU_HEIGHT;
    TextureDump_Cleared(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    for (int row = 0; row < SOFT_GPU_HEIGHT; row++)
        for (int block = 0; block < TEXTURE_CLEAN_COLUMNS; block++) assert(clean(block, row));
    tag[3 * SOFT_GPU_WIDTH + 70] = 11;
    shadow[1 + (3 * SOFT_GPU_WIDTH + 70) * 4] = 17;
    expected_x = 64; expected_y = 3; expected_w = 64; expected_h = 1;
    TextureDump_Cleared(64, 3, 64, 1);
    assert(tag[3 * SOFT_GPU_WIDTH + 70] == 11 && shadow[1 + (3 * SOFT_GPU_WIDTH + 70) * 4] == 17);
    TextureDump_Touched(70, 3, 1, 1);
    assert(!clean(1, 3) && clean(0, 3) && clean(2, 3) && clean(1, 2) && clean(1, 4));
    expected_x = 32; expected_w = 64;
    TextureDump_Cleared(32, 3, 64, 1); /* half of blocks 0 and 1 */
    assert(tag[3 * SOFT_GPU_WIDTH + 70] == 0 && shadow[1 + (3 * SOFT_GPU_WIDTH + 70) * 4] == 0);
    assert(!clean(1, 3) && clean(0, 3));
    tag[3 * SOFT_GPU_WIDTH + 100] = 11;
    expected_x = 0; expected_w = SOFT_GPU_WIDTH;
    TextureDump_Cleared(0, 3, SOFT_GPU_WIDTH, 1);
    assert(tag[3 * SOFT_GPU_WIDTH + 100] == 0 && clean(1, 3));
    /* Touched wraps as the words do. */
    TextureDump_Touched(SOFT_GPU_WIDTH - 24, SOFT_GPU_HEIGHT - 1, 100, 2);
    for (int block = 0; block < TEXTURE_CLEAN_COLUMNS; block++) {
        int touched = block == TEXTURE_CLEAN_COLUMNS - 1 || block <= 1;
        assert(clean(block, SOFT_GPU_HEIGHT - 1) == !touched && clean(block, 0) == !touched);
        assert(clean(block, 1) && clean(block, SOFT_GPU_HEIGHT - 2));
    }
    TextureDump_Touched(5, 9, SOFT_GPU_WIDTH, 1);
    for (int block = 0; block < TEXTURE_CLEAN_COLUMNS; block++) assert(!clean(block, 9));
}

int main(void)
{
    TextureDump_Forget = forget;
    for (int with_shadow = 0; with_shadow <= 1; with_shadow++) {
        check_clear(20, 30, 8, 8, with_shadow);
        check_clear(SOFT_GPU_WIDTH - 2, SOFT_GPU_HEIGHT - 1, 4, 2, with_shadow);
        check_clear(-3, -2, 7, 5, with_shadow);
        check_clear(9, 11, SOFT_GPU_WIDTH * 2 + 3, 2, with_shadow);
        check_clear(0, 0, 3, SOFT_GPU_HEIGHT + 2, with_shadow);
        check_clear(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT, with_shadow);
        check_clear(0, 0, 0, 2, with_shadow);
        check_clear(0, 0, 2, 0, with_shadow);
    }
    check_clean();
    TextureDump_Tags = NULL;
    forget_calls = 0;
    TextureDump_Cleared(0, 0, 1, 1);
    assert(forget_calls == 0);
    puts("Texture clears: native wrapping, shadow, adjacent cells, callback contract and clean blocks agree");
    return 0;
}
