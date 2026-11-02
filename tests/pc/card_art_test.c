/* The records card art makes of a PNG (cards/art.c): a card's picture and
 * thumbnail, a field cutout and a duelist's face, over pictures drawn here
 * with whole-number arithmetic, so they are the same everywhere. The record
 * of each is held to a hash made before the median cut worked in an arena
 * of its own: the palette and every index the same, byte for byte.
 * MEMORIES_ART_DUMP=1 prints the hashes in the table's form. */
#include "pc/cards/art.h"
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_folder(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_folder(path) mkdir(path, 0755)
#endif

/* The art unit's two outside needs. */
uint32_t Glyphs_NextCharacter(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t c = *s++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3Fu >> more;
    while (more-- > 0 && (*s & 0xC0) == 0x80) c = c << 6 | (*s++ & 0x3F);
    *text = (const char *)s;
    return c;
}

#ifdef _WIN32
const char *Win32_SerifFontPath(void) { return NULL; }
#endif

static int failures;

#define CHECK(condition, ...)                                                                                     \
    do {                                                                                                          \
        if (!(condition)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
            fprintf(stderr, __VA_ARGS__);                                                                         \
            fputc('\n', stderr);                                                                                  \
            failures++;                                                                                           \
        }                                                                                                         \
    } while (0)

static uint32_t seed;
static unsigned next_random(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 16;
}

/* A wave that rises and falls over `period` steps, 0-255. */
static int triangle(int at, int period)
{
    int half = period / 2, p = at % period;
    return (p < half ? p : period - p) * 255 / half;
}

typedef struct {
    const char *name;
    int width, height;
} Picture;

static const Picture pictures[] = {
    {"gradient", 408, 384},     /* smooth: few colors after the average */
    {"noise", 408, 384},        /* every texel its own */
    {"flat", 300, 300},         /* three colors */
    {"photo", 408, 384},        /* waves with a little noise */
    {"exact", CARD_ART_WIDTH, CARD_ART_HEIGHT},
    {"wide", 800, 200},         /* alpha, cropped at the middle */
    {"tiny", 17, 9},
    {"tall", 120, 700},
};
#define PICTURES ((int)(sizeof(pictures) / sizeof(*pictures)))

static void paint(int which, int x, int y, unsigned char *p)
{
    const Picture *picture = &pictures[which];
    p[3] = 255;
    switch (which) {
    case 0:
        p[0] = (unsigned char)(x * 255 / (picture->width - 1));
        p[1] = (unsigned char)(y * 255 / (picture->height - 1));
        p[2] = (unsigned char)((x + y) & 0xFF);
        break;
    case 1: case 4: case 6:
        p[0] = (unsigned char)next_random();
        p[1] = (unsigned char)next_random();
        p[2] = (unsigned char)next_random();
        break;
    case 2:
        p[0] = x < picture->width / 3 ? 200 : 20;
        p[1] = y < picture->height / 2 ? 90 : 240;
        p[2] = 60;
        break;
    case 3:
        p[0] = (unsigned char)((triangle(x, 97) + triangle(y, 61) + (int)(next_random() & 15)) / 2);
        p[1] = (unsigned char)((triangle(x + y, 131) * 3 + triangle(y, 23) + (int)(next_random() & 7)) / 4);
        p[2] = (unsigned char)(triangle(x * 2 + y, 211));
        break;
    case 5:
        p[0] = (unsigned char)triangle(x, 300);
        p[1] = (unsigned char)(y * 255 / (picture->height - 1));
        p[2] = (unsigned char)next_random();
        p[3] = (unsigned char)(x % 100 < 30 ? 0 : x % 100 < 40 ? 128 + (int)(next_random() & 63) : 255);
        break;
    default:
        p[0] = (unsigned char)triangle(y, 80);
        p[1] = (unsigned char)triangle(x * 5, 90);
        p[2] = (unsigned char)((x * y) & 0xFF);
        break;
    }
}

static int write_picture(const char *path, int which)
{
    const Picture *picture = &pictures[which];
    png_image image;
    unsigned char *rgba = malloc((size_t)picture->width * picture->height * 4);
    int x, y, ok;
    if (!rgba) return 0;
    seed = 12345u + (uint32_t)which;
    for (y = 0; y < picture->height; y++)
        for (x = 0; x < picture->width; x++) paint(which, x, y, rgba + ((size_t)y * picture->width + x) * 4);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = (png_uint_32)picture->width;
    image.height = (png_uint_32)picture->height;
    image.format = PNG_FORMAT_RGBA;
    ok = png_image_write_to_file(&image, path, 0, rgba, 0, NULL);
    free(rgba);
    return ok;
}

static uint64_t hash_of(const unsigned char *bytes, size_t length)
{
    uint64_t hash = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

/* What each picture's four records hashed to (the code that malloc'd every
 * box): card, thumbnail only, field cutout, portrait. */
enum { CARD, THUMBNAIL, FIELD, PORTRAIT, KINDS };
static const uint64_t held[PICTURES][KINDS] = {
    {0xa16d48ed63da68e2ull, 0x0398aaaf65198e2cull, 0xd39e86e07e89c051ull, 0x21a70e9202df380full}, /* gradient */
    {0xa5ad6fbcf27bd02bull, 0x4e7b0379a56e7e25ull, 0x8e4fb2cc0b27ddc9ull, 0x1058fd2f7a4d3dd4ull}, /* noise */
    {0x8e974d238b695405ull, 0x4a69dda56c9ee892ull, 0x6a3071bee74cc23cull, 0xecc35dd5df5ed90dull}, /* flat */
    {0x29b17ad3d562316full, 0xaefe458ac0b9dafcull, 0x8f25b18a0accfc10ull, 0x55832c12753fea7bull}, /* photo */
    {0x92585db77a689facull, 0xe6057792c119030full, 0x7d4ae33496e35394ull, 0xb70bb1b0a9af5bfcull}, /* exact */
    {0x845e8890b60c9401ull, 0xdfdc09dfaf41615aull, 0xdac8308e2eca84d8ull, 0x7ce583a09cc94c38ull}, /* wide */
    {0x2c545a81e9ce3654ull, 0xbc72e747599d4e40ull, 0xf3cf3f3a96a8761full, 0x64674a3f5451ba55ull}, /* tiny */
    {0x58d8fbe0bdd151e5ull, 0x3b5f208e6e58bedaull, 0xfc719cf3bba35980ull, 0xc6808a74e442c148ull}, /* tall */
};

int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : "card-art-test";
    int dump = getenv("MEMORIES_ART_DUMP") != NULL, which;
    make_folder(folder);
    for (which = 0; which < PICTURES; which++) {
        char path[512], why[512];
        unsigned char record[CARD_ART_RECORD], portrait[PORTRAIT_RECORD];
        uint64_t got[KINDS];
        int kind;
        snprintf(path, sizeof(path), "%s/%s.png", folder, pictures[which].name);
        CHECK(write_picture(path, which), "%s: could not be written", path);
        memset(record, 0, sizeof(record));
        CHECK(CardArt_FromImage(path, record, why, sizeof(why)), "%s: %s", pictures[which].name, why);
        got[CARD] = hash_of(record, sizeof(record));
        memset(record, 0, sizeof(record));
        CHECK(CardArt_ThumbnailFromImage(path, record, why, sizeof(why)), "%s: %s", pictures[which].name, why);
        got[THUMBNAIL] = hash_of(record, sizeof(record));
        memset(record, 0, sizeof(record));
        CHECK(CardArt_FieldArtFromImage(path, record, why, sizeof(why)), "%s: %s", pictures[which].name, why);
        got[FIELD] = hash_of(record, sizeof(record));
        memset(portrait, 0, sizeof(portrait));
        CHECK(CardArt_PortraitFromImage(path, portrait, why, sizeof(why)), "%s: %s", pictures[which].name, why);
        got[PORTRAIT] = hash_of(portrait, sizeof(portrait));
        if (dump) {
            printf("    {");
            for (kind = 0; kind < KINDS; kind++)
                printf("0x%016llxull%s", (unsigned long long)got[kind], kind + 1 < KINDS ? ", " : "},");
            printf(" /* %s */\n", pictures[which].name);
        }
        for (kind = 0; kind < KINDS; kind++)
            CHECK(dump || got[kind] == held[which][kind], "%s: record %d is 0x%016llx, held 0x%016llx",
                  pictures[which].name, kind, (unsigned long long)got[kind], (unsigned long long)held[which][kind]);
        remove(path);
    }
    if (failures) return 1;
    puts("Card art: every record is the one held");
    return 0;
}
