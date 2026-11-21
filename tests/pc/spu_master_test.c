/* The software SPU's master volume (src/pc/audio/spu.c, Spu_Mix) with many
 * loud voices.
 *
 * The voices are summed into an int, and that sum was multiplied by the
 * master volume in 32 bits: from about two full-scale voices on, the product
 * left the int range and the output came back as the opposite sign instead of
 * clipping. Here 24 voices sound a constant full-scale level at once and the
 * output must clip to the 16-bit range on the side the sum is on, in each
 * channel, and never cross zero.
 *
 * Built with -fwrapv, as every game build is, so the old product wraps
 * the same way on every compiler and the test fails there rather than
 * depending on what the optimizer does with the undefined overflow. */
#include "pc/audio/replace.h"
#include "pc/audio/spu.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Log_Wanted(LogChannel channel)
{
    (void)channel;
    return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...)
{
    (void)channel;
    (void)format;
}
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    (void)state;
    (void)tag;
    (void)fields;
    (void)count;
    return 0;
}
int AudioReplace_MixBegin(void) { return 0; }
void AudioReplace_MixFrame(int out[6]) { (void)out; }
void AudioReplace_MixEnd(void) {}

#define FRAMES 512
#define SETTLE 64 /* frames the envelopes need to reach full level */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* One ADPCM block (shift 0, filter 0) of the nibble 7 over and over, looping
 * onto itself: every sample is 28672. */
static void load_block(uint32_t at)
{
    int i;
    Spu_Ram[at] = 0;
    Spu_Ram[at + 1] = 7; /* loop start, loop end, repeat */
    for (i = 0; i < 14; i++) Spu_Ram[at + 2 + i] = 0x77;
}

/* `count` voices keyed on with the register volumes `left` and `right`, the
 * master at `master`; mixes FRAMES frames. Returns the range of the output
 * after SETTLE frames in lo[], hi[] (left, right). */
static void run(int count, int16_t left, int16_t right, int16_t master, int lo[2], int hi[2])
{
    static int16_t out[FRAMES * 2];
    int v, n, side;
    Spu_Reset();
    load_block(0x1000);
    Spu_SetMaster(master, master);
    for (v = 0; v < count; v++) {
        Spu_SetStart((unsigned)v, 0x1000);
        Spu_SetPitch((unsigned)v, 0x1000);
        Spu_SetAdsr((unsigned)v, 0x000f, 0x0000); /* instant attack, no decay, sustain at full */
        Spu_SetVolume((unsigned)v, left, right);
    }
    Spu_KeyOn((count >= 32 ? 0xffffffffu : (1u << count) - 1u));
    Spu_Mix(out, FRAMES);
    for (side = 0; side < 2; side++) lo[side] = 32768, hi[side] = -32769;
    for (n = SETTLE; n < FRAMES; n++) {
        for (side = 0; side < 2; side++) {
            int s = out[n * 2 + side];
            if (s < lo[side]) lo[side] = s;
            if (s > hi[side]) hi[side] = s;
        }
    }
}

int main(void)
{
    int lo[2], hi[2], count;
    /* Register 0x3fff is +0x7ffe, register 0x4000 is -0x8000 (the hardware
     * doubles the 15-bit level); the game's own master is 0x3fff. */
    const int16_t loud = 0x3fff, loud_negative = 0x4000;

    /* From two voices to all 24 the left channel clips high and the right,
     * which has the inverted volume, clips low; neither side may wrap. */
    for (count = 2; count <= 24; count++) {
        char what[80];
        run(count, loud, loud_negative, 0x3fff, lo, hi);
        if (count >= 4) {
            snprintf(what, sizeof(what), "%d voices: left clips at +32767 (low %d, high %d)", count, lo[0], hi[0]);
            check(lo[0] == 32767 && hi[0] == 32767, what);
            snprintf(what, sizeof(what), "%d voices: right clips at -32768 (low %d, high %d)", count, lo[1], hi[1]);
            check(lo[1] == -32768 && hi[1] == -32768, what);
        } else {
            snprintf(what, sizeof(what), "%d voices: left stays positive (low %d)", count, lo[0]);
            check(lo[0] > 0, what);
            snprintf(what, sizeof(what), "%d voices: right stays negative (high %d)", count, hi[1]);
            check(hi[1] < 0, what);
        }
    }

    /* A quieter master: still above the old 32-bit bound before the multiply. */
    run(24, loud, loud_negative, 0x2000, lo, hi);
    check(lo[0] == 32767 && hi[1] == -32768, "24 voices, master 0x2000: clips on each side");

    /* A master low enough not to clip: the product is in range either way,
     * so the level is exact: 24 voices of 28668 each, times 512 / 32768. */
    run(24, loud, loud, 0x0100, lo, hi);
    check(lo[0] == hi[0] && lo[1] == hi[1] && lo[0] == lo[1], "24 voices, master 0x0100: a steady level");
    check(lo[0] > 10500 && lo[0] < 11000, "24 voices, master 0x0100: the level is unclipped, near 10750");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("spu master volume: ok\n");
    return 0;
}
