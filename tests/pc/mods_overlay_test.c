#include <stddef.h>
/* The mods' overlay drawn again where it changed (Mods_OverlayChanges,
 * Mods_RedrawOverlayIn), as the window does when only the mods' drawing
 * changed. A fixture mod (mod_fixtures/overlay.c) draws a scene the test
 * changes at random between passes -- items moved, faded, recolored, added,
 * taken away, swapped in order, clipped, pictures redrawn in their own
 * buffers -- and after every change a canvas drawn again only in the
 * rectangles that differ must hold exactly the pixels of a canvas drawn
 * whole. */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/mods/events.h"
#include "pc/platform/menu.h"
#include "mod_fixtures/overlay_scene.h"
#include <png.h>
#include <time.h>

#define W 220
#define H 150
#define MOST 8

static uint32_t rng = 20261022u;
static int below(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (unsigned)n);
}

/* A fixture object, copied into a mod directory. */
static void install(const char *from, const char *to)
{
    FILE *file = fopen(from, "rb");
    long length;
    void *object;
    assert(file);
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    object = malloc((size_t)length);
    assert(object);
    assert(fread(object, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    write_file(to, object, (size_t)length);
    free(object);
}

/* The port's text, as a test can have it: a block per letter, holes in it,
 * inside the width it reports and five of the scale either side of the
 * middle. Like a font, the same wherever it is drawn: the holes are placed
 * from the text's own corner, not the canvas's. */
static void text(MenuCanvas *canvas, int x, int middle, const char *s, uint32_t rgb, int scale)
{
    for (int i = 0; s[i]; i++) {
        for (int row = middle - 5 * scale; row < middle + 5 * scale; row++) {
            for (int column = x + i * 6 * scale; column < x + (i * 6 + 4) * scale; column++) {
                if (row < 0 || row >= canvas->height || column < 0 || column >= canvas->width) continue;
                if ((row - middle + column - x + s[i]) % 3)
                    canvas->pixels[(size_t)row * (size_t)canvas->stride + (size_t)column] = 0xFF000000u | rgb;
            }
        }
    }
}

static int width(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

/* Blocks clear, half and whole, so that a picture stretched has pixels
 * covered whole in runs and pixels in part between them. */
static void picture(uint32_t *pixels)
{
    static const uint32_t alpha[4] = {0x00u, 0xFFu, 0xFFu, 0x80u};
    int shift = below(4), block = 3 + below(6);
    uint32_t color = (uint32_t)below(0x1000000);
    for (int y = 0; y < SCENE_PICTURE_SIZE; y++) {
        for (int x = 0; x < SCENE_PICTURE_SIZE; x++) {
            uint32_t a = alpha[(x / block + y / block + shift) % 4];
            pixels[y * SCENE_PICTURE_SIZE + x] = a << 24 | ((color + (uint32_t)(x * 7 + y * 3)) & 0xFFFFFFu);
        }
    }
}

static void item(OverlayScene *scene, int i)
{
    static const char *const words[] = {"FREE", "DUEL", "W 0 L 0", "?", "MIGHT"};
    static const unsigned alphas[] = {0, 40, 128, 228, 255, 255};
    scene->items[i].kind = below(ITEM_KINDS);
    scene->items[i].x = below(W + 40) - 30;
    scene->items[i].y = below(H + 40) - 30;
    scene->items[i].w = 2 + below(90);
    scene->items[i].h = 2 + below(70);
    scene->items[i].alpha = (int)alphas[below(6)];
    scene->items[i].picture = below(SCENE_PICTURES);
    scene->items[i].scale = 1 + below(2);
    scene->items[i].rgb = (uint32_t)below(0x1000000);
    snprintf(scene->items[i].text, sizeof(scene->items[i].text), "%s", words[below(5)]);
    scene->items[i].clipped = below(4) == 0;
    scene->items[i].clip[0] = below(W) - 20;
    scene->items[i].clip[1] = below(H) - 20;
    scene->items[i].clip[2] = below(W);
    scene->items[i].clip[3] = below(H);
}

/* One frame's worth of change, as a screen has: mostly small, now and then
 * none at all or a great deal. */
static void change(OverlayScene *scene)
{
    int i = scene->count ? below(scene->count) : 0, j;
    switch (below(10)) {
    case 0: /* nothing */
        break;
    case 1: /* a new one, somewhere in the order */
        if (scene->count < SCENE_ITEMS) {
            j = below(scene->count + 1);
            memmove(&scene->items[j + 1], &scene->items[j], (size_t)(scene->count - j) * sizeof(scene->items[0]));
            scene->count++;
            item(scene, j);
        }
        break;
    case 2: /* one gone */
        if (scene->count > 1) {
            memmove(&scene->items[i], &scene->items[i + 1], (size_t)(scene->count - i - 1) * sizeof(scene->items[0]));
            scene->count--;
        }
        break;
    case 3: /* two swapped in the order */
        j = below(scene->count);
        {
            char kept[sizeof(scene->items[0])];
            memcpy(kept, &scene->items[i], sizeof(kept));
            memcpy(&scene->items[i], &scene->items[j], sizeof(kept));
            memcpy(&scene->items[j], kept, sizeof(kept));
        }
        break;
    case 4: /* a picture drawn again in its own buffer */
        picture(scene->pictures[below(SCENE_PICTURES)]);
        break;
    case 5: /* faded */
        scene->items[i].alpha = below(256);
        break;
    case 6: /* recolored, or its words */
        scene->items[i].rgb ^= 0x204080u;
        scene->items[i].text[0] = (char)('A' + below(26));
        break;
    case 7: /* clipped, or not */
        scene->items[i].clipped = !scene->items[i].clipped;
        break;
    default: /* moved a little, as a cursor glides */
        scene->items[i].x += below(9) - 4;
        scene->items[i].y += below(9) - 4;
        break;
    }
}

/* What a sink was handed. */
static struct {
    int begins, ends, count;
    char kinds[8];
    const uint32_t *pixels[8];
    uint32_t hash[8];
} sunk;

static void sink_begin(void) { sunk.begins++; sunk.count = 0; }

static void sink_fill(const int clip[4], int x, int y, int w, int h, uint32_t rgb, unsigned alpha)
{
    (void)clip; (void)x; (void)y; (void)w; (void)h; (void)rgb; (void)alpha;
    if (sunk.count < 8) sunk.kinds[sunk.count++] = 'f';
}

static void sink_image(const int clip[4], const uint32_t *pixels, int w, int h, uint32_t hash, int x, int y, int dw,
                       int dh, unsigned alpha)
{
    (void)clip; (void)w; (void)h; (void)x; (void)y; (void)dw; (void)dh; (void)alpha;
    if (sunk.count < 8) {
        sunk.pixels[sunk.count] = pixels;
        sunk.hash[sunk.count] = hash;
        sunk.kinds[sunk.count++] = 'i';
    }
}

static void sink_end(void) { sunk.ends++; }
static void sink_forget(const uint32_t *pixels) { (void)pixels; }
static const ModsOverlaySink sink = {sink_begin, sink_fill, sink_image, sink_end, sink_forget};

static void compare(const uint32_t *a, const uint32_t *b, int step)
{
    for (int i = 0; i < W * H; i++) {
        if (a[i] != b[i]) {
            fprintf(stderr, "step %d: pixel %d,%d is %08x drawn in part, %08x drawn whole\n", step, i % W, i / W,
                    a[i], b[i]);
            exit(1);
        }
    }
}

/* Until the reader thread has read every picture asked for. */
static size_t settled(int *jobs)
{
    int unread = 1;
    size_t bytes = 0;
    for (int tries = 0; unread && tries < 2000; tries++) {
        struct timespec pause = {0, 5000000};
        bytes = Mods_Prefetched(jobs, &unread);
        if (unread) nanosleep(&pause, NULL);
    }
    assert(!unread);
    return bytes;
}

/* Pictures read ahead (host->prefetch_image) are kept to a budget, 64 KB
 * in this test (four 64 x 64), the oldest let go first, load_image reading
 * those itself; a mod turned off loses the ones read for it. */
static void prefetched(OverlayScene *scene)
{
    char name[32], path[1024];
    uint32_t pixels[64 * 64], *loaded;
    int i, jobs, w, h;
    for (i = 0; i < 64 * 64; i++) pixels[i] = 0xFF000000u | (uint32_t)i;
    for (i = 0; i < 6; i++) {
        png_image image;
        memset(&image, 0, sizeof(image));
        image.version = PNG_IMAGE_VERSION;
        image.width = image.height = 64;
        image.format = PNG_FORMAT_BGRA;
        snprintf(path, sizeof(path), "%s/mods/overlay/p%d.png", root, i);
        pixels[0] = 0xFF000000u | (uint32_t)i;
        assert(png_image_write_to_file(&image, path, 0, pixels, 0, NULL));
    }
    for (i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "p%d.png", i);
        scene->prefetch(name);
        settled(&jobs); /* one at a time: the oldest are the first asked */
    }
    assert(settled(&jobs) == (size_t)4 * 64 * 64 * 4);
    assert(jobs == 4);
    /* Taken (p5) or read again (p0, let go): the same pixels either way. */
    for (i = 5; i >= 0; i -= 5) {
        snprintf(name, sizeof(name), "p%d.png", i);
        loaded = scene->load(name, &w, &h);
        assert(loaded && w == 64 && h == 64 && loaded[0] == (0xFF000000u | (uint32_t)i) && loaded[1] == 0xFF000001u);
        scene->free(loaded);
    }
    assert(settled(&jobs) == (size_t)3 * 64 * 64 * 4 && jobs == 3);
    scene->prefetch("p0.png");
    Mods_SetEnabled(0, 0);
    assert(settled(&jobs) == 0 && jobs == 0);
    puts("mods overlay: pictures read ahead kept to their budget and let go with their mod");
}

int main(int argc, char **argv)
{
    static uint32_t window[W * H], fresh[W * H];
    MenuCanvas canvas = {window, W, W, H, 1}, whole = {fresh, W, W, H, 1};
    OverlayScene *scene;
    char path[1024];
    int rects[4 * MOST], x, y, w, h, n, i, step, steps = 4000, partial = 0, same = 0;
    assert(argc == 2);
    assert(scratch_dir(root, sizeof(root), "memories-overlay"));
    make_dir("mods");
    make_dir("mods/overlay");
    install(argv[1], "mods/overlay/overlay.o");
    write_text("mods/overlay/mod.json", "{\"id\":\"overlay\",\"library\":\"overlay\",\"enabled\":true}");
    snprintf(path, sizeof(path), "%s/mods", root);
    setenv("MEMORIES_MODS_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    setenv("MEMORIES_SETTINGS", path, 1);
    Settings_Load();
    Mods_Load();
    scene = Mods_Find("overlay:scene");
    assert(scene);
    for (i = 0; i < SCENE_PICTURES; i++) picture(scene->pictures[i]);
    for (scene->count = 0; scene->count < 20; scene->count++) item(scene, scene->count);

    /* Nothing to compare with before a pass has drawn the canvas whole. */
    assert(Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST) == -1);
    Mods_DrawOverlay(&canvas, 1, text, width, &x, &y, &w, &h);
    assert(w > 0 && h > 0);
    for (step = 0; step < steps; step++) {
        change(scene);
        n = Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST);
        assert(n >= 0 && n <= MOST);
        if (!n) same++;
        for (i = 0; i < n; i++) {
            const int *r = rects + 4 * i;
            assert(r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] + r[2] <= W && r[1] + r[3] <= H);
            for (int row = r[1]; row < r[1] + r[3]; row++) memset(window + row * W + r[0], 0, (size_t)r[2] * 4);
        }
        if (n) {
            assert(Mods_RedrawOverlayIn(&canvas, 1, text, width, rects, n));
            partial++;
        }
        memset(fresh, 0, sizeof(fresh));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        compare(window, fresh, step);
    }
    printf("mods overlay: %d of %d passes drawn in part, %d with nothing changed\n", partial, steps, same);
    assert(partial > steps / 2 && same > 0);

    /* Scaling, the unscaled path, alpha and clips match the scalar sampler. */
    {
        static const unsigned alphas[] = {0, 1, 63, 127, 128, 228, 255};
        static const int sizes[][2] = {{24, 24}, {39, 31}, {17, 19}};
        memset(scene, 0, offsetof(OverlayScene, kept));
        scene->count = 1;
        scene->items[0].kind = ITEM_PICTURE;
        scene->items[0].x = -3;
        scene->items[0].y = 5;
        scene->items[0].clipped = 1;
        scene->items[0].clip[0] = 2;
        scene->items[0].clip[1] = 3;
        scene->items[0].clip[2] = 16;
        scene->items[0].clip[3] = 12;
        for (i = 0; i < SCENE_PICTURE_SIZE * SCENE_PICTURE_SIZE; i++)
            scene->pictures[0][i] = (unsigned)(i % 256) << 24 | (unsigned)(i * 8191) % 0x1000000;
        for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            int dw = scene->items[0].w = sizes[si][0], dh = scene->items[0].h = sizes[si][1];
            for (unsigned ai = 0; ai < sizeof(alphas) / sizeof(alphas[0]); ai++) {
                unsigned alpha = alphas[ai];
                scene->items[0].alpha = (int)alpha;
                for (i = 0; i < W * H; i++) window[i] = fresh[i] = 0x80506070u;
                for (int row = 5; row < 15; row++) {
                    float fy = (row - 5 + 0.5f) * SCENE_PICTURE_SIZE / dh - 0.5f;
                    int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < SCENE_PICTURE_SIZE ? y0 + 1 : y0;
                    float ty = fy < 0 ? 0 : fy - y0;
                    for (int column = 2; column < 18 && column < dw - 3; column++) {
                        float fx = (column + 3 + 0.5f) * SCENE_PICTURE_SIZE / dw - 0.5f;
                        int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < SCENE_PICTURE_SIZE ? x0 + 1 : x0;
                        float tx = fx < 0 ? 0 : fx - x0;
                        const uint32_t *pic = scene->pictures[0];
                        uint32_t q[4] = {pic[y0 * 24 + x0], pic[y0 * 24 + x1], pic[y1 * 24 + x0], pic[y1 * 24 + x1]};
                        float k[4] = {(1-tx)*(1-ty), tx*(1-ty), (1-tx)*ty, tx*ty}, a = 0, c[3] = {0};
                        for (int corner = 0; corner < 4; corner++) {
                            float qa = (q[corner] >> 24) * k[corner];
                            a += qa;
                            c[0] += (q[corner] >> 16 & 255) * qa;
                            c[1] += (q[corner] >> 8 & 255) * qa;
                            c[2] += (q[corner] & 255) * qa;
                        }
                        if (a < 0.5f || !alpha) continue;
                        unsigned blend = (unsigned)(a * alpha / 255 + 0.5f), inverse = 255 - blend;
                        fresh[row * W + column] = (blend + 128 * inverse / 255) << 24 |
                            (((unsigned)(c[0] / a) * blend + 0x50 * inverse) / 255) << 16 |
                            (((unsigned)(c[1] / a) * blend + 0x60 * inverse) / 255) << 8 |
                            (((unsigned)(c[2] / a) * blend + 0x70 * inverse) / 255);
                    }
                }
                Mods_DrawOverlay(&canvas, 1, text, width, &x, &y, &w, &h);
                compare(window, fresh, (int)ai);
            }
        }
    }

#ifdef MEMORIES_TEST_OVERLAY_CACHE
    /* A long run of partial redraws must recycle old scaled pictures.
     * Comparing against a whole draw each frame masks the pinned-cache bug. */
    {
        extern unsigned Mods_TestOverlayFallbacks(void);
        unsigned before = Mods_TestOverlayFallbacks();
        memset(scene, 0, offsetof(OverlayScene, kept));
        scene->count = 1;
        scene->items[0].kind = ITEM_PICTURE;
        scene->items[0].alpha = 255;
        picture(scene->pictures[0]);
        memset(window, 0, sizeof(window));
        Mods_DrawOverlay(&canvas, 1, text, width, &x, &y, &w, &h);
        for (step = 0; step < 400; step++) {
            scene->items[0].w = 8 + step % 180;
            scene->items[0].h = 8 + step / 180;
            n = Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST);
            assert(n > 0);
            for (i = 0; i < n; i++) {
                const int *r = rects + 4 * i;
                for (int row = r[1]; row < r[1] + r[3]; row++)
                    memset(window + row * W + r[0], 0, (size_t)r[2] * 4);
            }
            assert(Mods_RedrawOverlayIn(&canvas, 1, text, width, rects, n));
        }
        assert(Mods_TestOverlayFallbacks() == before);
        memset(fresh, 0, sizeof(fresh));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        compare(window, fresh, step);
        puts("mods overlay: sustained partial redraws recycle the image cache");
    }
#endif

    /* With a sink the calls go there, in order, and the canvas is left alone.
     * Text comes as a picture, the same one while it says the same; a
     * picture the host made keeps its hash until the mod says it changed. */
    {
        static OverlayScene before;
        uint32_t kept_hash, *stamp = NULL;
        before = *scene;
        Mods_SetOverlaySink(&sink);
        scene->count = 3;
        for (i = 0; i < 3; i++) {
            scene->items[i].kind = i == 0 ? ITEM_FILL : i == 1 ? ITEM_TEXT : ITEM_PICTURE;
            scene->items[i].x = scene->items[i].y = 30;
            scene->items[i].w = scene->items[i].h = 20;
            scene->items[i].scale = 1;
            scene->items[i].picture = 0;
            scene->items[i].clipped = 0;
            scene->items[i].alpha = i == 0 ? 100 : 255;
        }
        snprintf(scene->items[1].text, sizeof(scene->items[1].text), "stamp");
        memset(fresh, 0x5A, sizeof(fresh));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        for (i = 0; i < W * H; i++) assert(fresh[i] == 0x5A5A5A5Au);
        assert(!w && !h);
        assert(sunk.begins == 1 && sunk.ends == 1 && sunk.count == 4);
        assert(sunk.kinds[0] == 'f' && sunk.kinds[1] == 'i' && sunk.kinds[2] == 'i' && sunk.kinds[3] == 'i');
        stamp = (uint32_t *)sunk.pixels[1];
        assert(sunk.pixels[3] == scene->kept);
        kept_hash = sunk.hash[3];
        memset(&sunk, 0, sizeof(sunk));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        assert(sunk.pixels[1] == stamp);
        memset(scene->kept, 0x77, 16 * sizeof(uint32_t));
        memset(&sunk, 0, sizeof(sunk));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        assert(sunk.hash[3] == kept_hash); /* not looked at again */
        scene->kept_changed = 1;
        memset(&sunk, 0, sizeof(sunk));
        Mods_DrawOverlay(&whole, 1, text, width, &x, &y, &w, &h);
        assert(sunk.hash[3] != kept_hash);
        Mods_SetOverlaySink(NULL);
        puts("mods overlay: a sink takes the calls in order; pictures the host made keep their hash");
        *scene = before;
        memset(window, 0, sizeof(window));
        Mods_DrawOverlay(&canvas, 1, text, width, &x, &y, &w, &h);
    }

    /* A pass made again that draws something else (a mod whose drawing
     * changed between the two) is refused: the canvas is then drawn whole,
     * and until it is there is nothing to compare with. */
    scene->items[0].x += 3;
    n = Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST);
    assert(n > 0);
    scene->items[0].x += 5;
    assert(!Mods_RedrawOverlayIn(&canvas, 1, text, width, rects, n));
    assert(Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST) == -1);
    memset(window, 0, sizeof(window));
    Mods_DrawOverlay(&canvas, 1, text, width, &x, &y, &w, &h);
    assert(Mods_OverlayChanges(&canvas, 1, text, width, rects, MOST) == 0);
    /* Nor is a canvas of another size compared with this one's calls. */
    whole.width = W - 10;
    assert(Mods_OverlayChanges(&whole, 1, text, width, rects, MOST) == -1);
    prefetched(scene);
    Mods_Shutdown();
    return 0;
}
