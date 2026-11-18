/* The software SPU's mixer (src/pc/audio/spu.c) against a golden stream.
 *
 * A seeded schedule changes the registers between mix calls (keys, pitch,
 * volume, ADSR, loop points, master and CD volume, CD/XA input at both
 * rates, the port's bus and output volume, mute, interpolation, and a
 * mod's replacement sounds) for ten minutes of sound in 256-frame calls,
 * as SDL's feed() makes them. SHA-256 covers every call's output, each
 * voice's envelope and key status after it, and every 16th call the bytes
 * of Spu_State's chunk (the voices and registers; not the sound RAM).
 *
 *  1. The plain stream. Its digest is the one the mixer's bench gave on
 *     GCC and Clang, x86-64 and i686, before this test (the schedule is
 *     that bench's, call for call).
 *  2. The same stream again in this process, from a state saved before
 *     the first call: up to the middle it must hash as pass 1 did, so a
 *     load leaves nothing of the earlier stream behind. There a state is
 *     saved, the SPU scrambled, the state loaded and saved again: the two
 *     must be the same bytes, sound RAM included. The stream then goes on
 *     from the loaded state (a load flushes the CD input) to its own
 *     golden digest.
 *
 * `spu_mix_test print` prints the digests instead of checking them. */
#include "pc/audio/replace.h"
#include "pc/audio/spu.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pass 1: the bench's `exact 600` output, whose digest began dd455fc440ed48a2. */
static const char PLAIN[] = "dd455fc440ed48a29c93290c4ef81946c6caa1b77ee115a2f5d2b1cb5a43b3cf";
static const char ROUND_TRIP[] = "80b85fdeec723d0830779916d4d90fe75725459746a64474898434354dbdad9a";
#define SECONDS 600

/* --- SHA-256, streamed (FIPS 180-4) ---------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint8_t buffer[64];
    uint64_t length;
} Sha;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k, t1, t2;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (i = 0; i < 64; i++) {
        t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

static void sha_init(Sha *s)
{
    static const uint32_t start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, start, sizeof(start));
    s->length = 0;
}

static void sha_add(Sha *s, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        size_t at = (size_t)(s->length % 64), take = 64 - at < size ? 64 - at : size;
        memcpy(s->buffer + at, p, take);
        s->length += take;
        p += take;
        size -= take;
        if (at + take == 64) sha_block(s->h, s->buffer);
    }
}

/* The hex digest of what was added so far; `s` itself goes on unchanged. */
static void sha_hex(const Sha *s, char hex[65])
{
    Sha end = *s;
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, zero = 0, size[8];
    unsigned i;
    sha_add(&end, &pad, 1);
    while (end.length % 64 != 56) sha_add(&end, &zero, 1);
    for (i = 0; i < 8; i++) size[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_add(&end, size, 8);
    for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", (unsigned)(uint8_t)(end.h[i / 4] >> (24 - 8 * (i % 4))));
}

/* --- what spu.c links to ----------------------------------------------------- */

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

/* Spu_State's chunk. HASH adds fields 1.. (all but the sound RAM) to the
 * stream; SAVE and LOAD copy every field to and from `saved`. */
enum { HASH, SAVE, LOAD };
struct MemoriesState {
    int mode;
    Sha *sha;
    uint8_t *saved;
    size_t size;
};
static uint8_t saved_start[SPU_RAM_SIZE + 4096], saved_a[SPU_RAM_SIZE + 4096], saved_b[SPU_RAM_SIZE + 4096];
static int loads;

int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    size_t i, at = 0;
    if (strcmp(tag, "spu")) {
        fprintf(stderr, "spu_mix_test: unexpected chunk %s\n", tag);
        exit(1);
    }
    for (i = 0; i < count; i++) {
        if (state->mode == HASH) {
            if (i) sha_add(state->sha, fields[i].data, fields[i].size);
            continue;
        }
        if (at + fields[i].size > sizeof(saved_a)) {
            fprintf(stderr, "spu_mix_test: the spu chunk outgrew the test's buffer\n");
            exit(1);
        }
        if (state->mode == SAVE) memcpy(state->saved + at, fields[i].data, fields[i].size);
        else memcpy(fields[i].data, state->saved + at, fields[i].size);
        at += fields[i].size;
    }
    if (state->mode == LOAD && at != state->size) {
        fprintf(stderr, "spu_mix_test: the spu chunk is %zu bytes, %zu were saved\n", at, state->size);
        exit(1);
    }
    state->size = at;
    loads += state->mode == LOAD;
    return state->mode == LOAD;
}

static size_t save(uint8_t *to)
{
    MemoriesState state = {SAVE, NULL, to, 0};
    Spu_State(&state);
    return state.size;
}

static void load(uint8_t *from, size_t size)
{
    MemoriesState state = {LOAD, NULL, from, size};
    Spu_State(&state);
}

/* A mod's replacement sounds (replace.h): a deterministic sequence, on in
 * the modes the schedule picks. */
static int replace_mode;
static unsigned replace_n;
int AudioReplace_MixBegin(void) { return replace_mode; }
void AudioReplace_MixFrame(int out[6])
{
    int i;
    for (i = 0; i < 6; i++) out[i] = (int)((replace_n * 2654435761u + (unsigned)i * 40503u) >> 20) - 2048;
    replace_n++;
}
void AudioReplace_MixEnd(void) {}

/* --- the schedule ------------------------------------------------------------ */

static uint32_t seed = 12345;
/* One draw per statement: an argument list's order is the compiler's. */
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* 24 samples of 64 blocks each, at 0x1000 + 0x400 * v; odd ones stop at their end. */
static void make_samples(void)
{
    unsigned v, b, i;
    for (v = 0; v < 24; v++) {
        uint8_t *at = Spu_Ram + 0x1000 + v * 0x400;
        for (b = 0; b < 64; b++) {
            uint8_t *block = at + b * 16;
            uint32_t filter = rnd() % 5;
            block[0] = (uint8_t)(filter << 4 | (rnd() % 16));
            block[1] = b == 0 ? 4 : b == 63 ? (v & 1 ? 1 : 3) : 0;
            for (i = 2; i < 16; i++) block[i] = (uint8_t)rnd();
        }
    }
}

static void events(int16_t *cd)
{
    unsigned count = rnd() % 6, e;
    for (e = 0; e < count; e++) {
        unsigned kind = rnd() % 16, v = rnd() % 24;
        switch (kind) {
        case 0: case 1: case 2: {
            uint32_t address = (rnd() % 24) * 0x400;
            address += (rnd() % 4) * 16;
            Spu_SetStart(v, 0x1000 + address);
            Spu_KeyOn(1u << v);
            break;
        }
        case 3: Spu_KeyOff(1u << v); break;
        case 4: Spu_KeyOn(1u << v); Spu_KeyOff(1u << v); break;
        case 5: Spu_SetPitch(v, (uint16_t)rnd()); break;
        case 6: Spu_SetPitch(v, (uint16_t)(0x1000 + rnd() % 0x400)); break;
        case 7: { int16_t l = (int16_t)rnd(), r = (int16_t)rnd(); Spu_SetVolume(v, l, r); break; }
        case 8: { int16_t l = (int16_t)(rnd() & 0x3fff), r = (int16_t)(rnd() & 0x3fff); Spu_SetVolume(v, l, r); break; }
        case 9: { uint16_t a1 = (uint16_t)rnd(), a2 = (uint16_t)rnd(); Spu_SetAdsr(v, a1, a2); break; }
        case 10: Spu_SetLoop(v, 0x1000 + (rnd() % 24) * 0x400); break;
        case 11: { int16_t l = (int16_t)(rnd() & 0x3fff), r = (int16_t)(rnd() & 0x3fff); Spu_SetMaster(l, r); break; }
        case 12: {
            int16_t l = (int16_t)rnd(), r = (int16_t)rnd();
            int enabled = rnd() % 4 != 0;
            Spu_SetCd(l, r, enabled);
            break;
        }
        case 13: {
            size_t n = rnd() % 2048, i;
            for (i = 0; i < n; i++) {
                cd[i * 2] = (int16_t)rnd();
                cd[i * 2 + 1] = (int16_t)rnd();
            }
            Spu_CdWrite(cd, n, rnd() % 2 ? 37800 : 18900);
            if (rnd() % 8 == 0) Spu_CdFlush();
            break;
        }
        case 14:
            if (rnd() % 4 == 0) { SpuBus bus = (SpuBus)(rnd() % 3); Spu_SetBusVolume(bus, (int)(rnd() % 101)); }
            if (rnd() % 8 == 0) Spu_SetOutputVolume((int)(rnd() % 101));
            if (rnd() % 16 == 0) Spu_SetMuted(rnd() % 2);
            break;
        default:
            if (rnd() % 4 == 0) Spu_SetInterpolation((SpuInterpolation)(rnd() % 2));
            if (rnd() % 4 == 0) replace_mode = (int)(rnd() % 4);
            break;
        }
    }
}

/* One mix call and what it adds to the stream. */
static void call(long c, Sha *sha)
{
    static int16_t out[256 * 2], cd[2048 * 2];
    uint8_t record[sizeof(out) + 24 * 3];
    unsigned v, i;
    events(cd);
    if (c % 64 == 0) Spu_SetAdsr(rnd() % 24, 0x00ff, 0x9fc0);
    Spu_Mix(out, 256);
    for (i = 0; i < 256 * 2; i++) {
        record[2 * i] = (uint8_t)((uint16_t)out[i] & 0xff);
        record[2 * i + 1] = (uint8_t)((uint16_t)out[i] >> 8);
    }
    for (v = 0; v < 24; v++) {
        uint16_t envelope = (uint16_t)Spu_Envelope(v);
        record[sizeof(out) + v * 3] = (uint8_t)(envelope & 0xff);
        record[sizeof(out) + v * 3 + 1] = (uint8_t)(envelope >> 8);
        record[sizeof(out) + v * 3 + 2] = (uint8_t)Spu_KeyStatus(v);
    }
    sha_add(sha, record, sizeof(record));
    if (c % 16 == 0) {
        MemoriesState state = {HASH, sha, NULL, 0};
        Spu_State(&state);
    }
}

/* The port's own settings (outside save states) back to their defaults,
 * and the mixer's gains walked back to them. */
static void port_defaults(void)
{
    static int16_t silence[256 * 2];
    int i;
    replace_mode = 0;
    Spu_SetOutputVolume(100);
    Spu_SetMuted(0);
    Spu_SetBusVolume(SPU_BUS_MUSIC, 100);
    Spu_SetBusVolume(SPU_BUS_SFX, 100);
    Spu_SetBusVolume(SPU_BUS_STREAM, 100);
    Spu_SetInterpolation(SPU_INTERPOLATION_GAUSSIAN);
    for (i = 0; i < 64; i++) Spu_Mix(silence, 256); /* GAIN_STEP: the whole range in 1600 frames */
    Spu_CdWrite(silence, 0, 37800);                  /* the input rate a new process starts at */
}

static int check(const char *what, const char *got, const char *want, int print)
{
    if (print) {
        printf("%s %s\n", what, got);
        return 0;
    }
    if (!strcmp(got, want)) return 0;
    fprintf(stderr, "spu_mix_test: %s digest %s, expected %s\n", what, got, want);
    return 1;
}

int main(int argc, char **argv)
{
    const long calls = (long)SECONDS * 44100 / 256, middle = calls / 2;
    int print = argc > 1 && !strcmp(argv[1], "print"), failed = 0;
    Sha sha, at_middle;
    char hex[65], hex_middle[65];
    size_t start_size, size_a, size_b;
    uint32_t seed_start;
    long c;

    make_samples();
    Spu_Reset();
    seed_start = seed;
    start_size = save(saved_start);

    /* 1: the plain stream */
    sha_init(&sha);
    for (c = 0; c < calls; c++) {
        if (c == middle) at_middle = sha;
        call(c, &sha);
    }
    sha_hex(&at_middle, hex_middle);
    sha_hex(&sha, hex);
    failed |= check("plain", hex, PLAIN, print);

    /* 2: again from the state saved before the first call */
    port_defaults();
    load(saved_start, start_size);
    seed = seed_start;
    replace_n = 0;
    sha_init(&sha);
    for (c = 0; c < middle; c++) call(c, &sha);
    sha_hex(&sha, hex);
    if (strcmp(hex, hex_middle)) {
        fprintf(stderr, "spu_mix_test: restarted from a state, the stream's first half hashes %s, not %s\n",
                hex, hex_middle);
        failed = 1;
    }
    /* the round trip: save, scramble every field, load, save again */
    size_a = save(saved_a);
    {
        unsigned v;
        Spu_Reset();
        memset(Spu_Ram, 0x5a, SPU_RAM_SIZE);
        for (v = 0; v < 24; v++) {
            Spu_SetVolume(v, -1, 0x1234);
            Spu_SetPitch(v, 0x2345);
            Spu_SetStart(v, 0x3450);
            Spu_SetLoop(v, 0x4560);
            Spu_SetAdsr(v, 0x5678, 0x6789);
        }
        Spu_KeyOn(0xffffff);
        Spu_KeyOff(0x0f0f0f);
        Spu_SetMaster(1, 2);
        Spu_SetCd(3, 4, 0);
    }
    load(saved_a, size_a);
    size_b = save(saved_b);
    if (size_a != size_b || memcmp(saved_a, saved_b, size_a)) {
        fprintf(stderr, "spu_mix_test: a state saved, loaded and saved again changed\n");
        failed = 1;
    }
    for (c = middle; c < calls; c++) call(c, &sha);
    sha_hex(&sha, hex);
    failed |= check("round-trip", hex, ROUND_TRIP, print);
    if (loads != 2) {
        fprintf(stderr, "spu_mix_test: %d loads, expected 2\n", loads);
        failed = 1;
    }
    if (!failed && !print) printf("spu_mix_test: %ld mix calls twice, both digests match\n", calls);
    return failed;
}
