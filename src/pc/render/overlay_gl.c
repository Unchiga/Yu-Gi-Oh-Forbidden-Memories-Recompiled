/* overlay_gl.h. A picture is kept by its address, size and hash, as the
 * canvas keeps its stretches, and sampled the way they are: linear between
 * texels, the color weighted by alpha (the texture is premultiplied, so the
 * clear texels at a picture's edge bring no color in). The quads are made
 * when a pass runs, while the mod's pixels are alive, and drawn from then
 * on without looking at them. */
#include "overlay_gl.h"
#include "pc/mods/mods.h"
#if defined(__APPLE__) || defined(__ANDROID__) || defined(SDL_PLATFORM_ANDROID)

const struct ModsOverlaySink *OverlayGl_Sink(void) { return NULL; }
void OverlayGl_Draw(int win_w, int win_h, int output_w, int output_h)
{
    (void)win_w; (void)win_h; (void)output_w; (void)output_h;
}
void OverlayGl_Lost(void) {}

#else
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The textures past this many bytes go, the least lately drawn first. */
#define TEXTURE_BYTES (192u * 1024u * 1024u)
#define BUCKETS 1024u

typedef struct Texture {
    struct Texture *next;
    const uint32_t *pixels;
    int w, h;
    uint32_t hash;
    GLuint name;
    unsigned used; /* the pass that last drew it */
} Texture;

typedef struct {
    GLuint texture;
    int x, y, w, h;
    float color[4]; /* premultiplied */
    int clip[4];
} Quad;

static Texture *buckets[BUCKETS];
static size_t texture_bytes;
static unsigned pass;
static GLuint white;
static Quad *quads;
static int quad_count, quad_room;
/* Textures deleted when the quads that may name them have been replaced. */
static GLuint *dead;
static int dead_count, dead_room;
static uint32_t *upload;
static size_t upload_room;

static Texture **bucket_of(const uint32_t *pixels)
{
    return &buckets[((uint32_t)((uintptr_t)pixels >> 4) * 2654435761u) >> 22];
}

static void bury(GLuint name)
{
    if (dead_count == dead_room) {
        int room = dead_room ? dead_room * 2 : 64;
        GLuint *more = realloc(dead, (size_t)room * sizeof(*more));
        if (!more) return; /* a texture left over; nothing shown is wrong */
        dead = more;
        dead_room = room;
    }
    dead[dead_count++] = name;
}

static void texture_drop(Texture **at)
{
    Texture *texture = *at;
    *at = texture->next;
    texture_bytes -= (size_t)texture->w * (size_t)texture->h * 4;
    bury(texture->name);
    free(texture);
}

/* The pixels, premultiplied, into the bound texture. */
static void texture_fill(const uint32_t *pixels, int w, int h, int fresh)
{
    size_t count = (size_t)w * (size_t)h;
    if (count > upload_room) {
        uint32_t *more = realloc(upload, count * sizeof(*more));
        if (!more) return;
        upload = more;
        upload_room = count;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t q = pixels[i];
        unsigned a = q >> 24;
        if (a == 255) {
            upload[i] = q;
        } else if (!a) {
            upload[i] = 0;
        } else {
            unsigned rb = (q & 0xFF00FFu) * a + 0x800080u, g = (q & 0xFF00u) * a + 0x8000u;
            upload[i] = q & 0xFF000000u;
            upload[i] |= ((rb + (rb >> 8 & 0xFF00FFu)) >> 8 & 0xFF00FFu) | ((g + (g >> 8 & 0xFF00u)) >> 8 & 0xFF00u);
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fresh) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, upload);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, upload);
}

static GLuint texture_for(const uint32_t *pixels, int w, int h, uint32_t hash)
{
    Texture **at = bucket_of(pixels), *texture;
    for (; (texture = *at) != NULL; at = &texture->next) {
        if (texture->pixels != pixels || texture->w != w || texture->h != h) continue;
        if (texture->hash != hash) { /* new pixels in the same buffer */
            glBindTexture(GL_TEXTURE_2D, texture->name);
            texture_fill(pixels, w, h, 0);
            texture->hash = hash;
        }
        texture->used = pass;
        return texture->name;
    }
    if (!(texture = calloc(1, sizeof(*texture)))) return 0;
    texture->pixels = pixels;
    texture->w = w;
    texture->h = h;
    texture->hash = hash;
    texture->used = pass;
    glGenTextures(1, &texture->name);
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture_fill(pixels, w, h, 1);
    texture_bytes += (size_t)w * (size_t)h * 4;
    texture->next = *bucket_of(pixels);
    *bucket_of(pixels) = texture;
    return texture->name;
}

static Quad *quad_add(const int clip[4])
{
    Quad *quad;
    if (quad_count == quad_room) {
        int room = quad_room ? quad_room * 2 : 256;
        Quad *more = realloc(quads, (size_t)room * sizeof(*more));
        if (!more) return NULL;
        quads = more;
        quad_room = room;
    }
    quad = &quads[quad_count++];
    memcpy(quad->clip, clip, sizeof(quad->clip));
    return quad;
}

static void sink_begin(void)
{
    if (dead_count) glDeleteTextures(dead_count, dead);
    dead_count = 0;
    quad_count = 0;
    pass++;
    if (!white) {
        static const uint32_t one = 0xFFFFFFFFu;
        glGenTextures(1, &white);
        glBindTexture(GL_TEXTURE_2D, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &one);
    }
}

static void sink_fill(const int clip[4], int x, int y, int w, int h, uint32_t rgb, unsigned alpha)
{
    Quad *quad = alpha ? quad_add(clip) : NULL;
    float a = (float)alpha / 255.0f;
    if (!quad) return;
    quad->texture = white;
    quad->x = x;
    quad->y = y;
    quad->w = w;
    quad->h = h;
    quad->color[0] = (float)(rgb >> 16 & 255) / 255.0f * a;
    quad->color[1] = (float)(rgb >> 8 & 255) / 255.0f * a;
    quad->color[2] = (float)(rgb & 255) / 255.0f * a;
    quad->color[3] = a;
}

static void sink_image(const int clip[4], const uint32_t *pixels, int w, int h, uint32_t hash, int x, int y, int dw,
                       int dh, unsigned alpha)
{
    GLuint name = alpha ? texture_for(pixels, w, h, hash) : 0;
    Quad *quad = name ? quad_add(clip) : NULL;
    float a = (float)alpha / 255.0f;
    if (!quad) return;
    quad->texture = name;
    quad->x = x;
    quad->y = y;
    quad->w = dw;
    quad->h = dh;
    quad->color[0] = quad->color[1] = quad->color[2] = quad->color[3] = a;
}

static void sink_end(void)
{
    /* The least lately drawn go until the rest fit; this pass's stay. */
    while (texture_bytes > TEXTURE_BYTES) {
        Texture **oldest = NULL;
        for (unsigned i = 0; i < BUCKETS; i++) {
            for (Texture **at = &buckets[i]; *at; at = &(*at)->next) {
                if ((*at)->used != pass && (!oldest || (*at)->used - (*oldest)->used > 0x80000000u)) oldest = at;
            }
        }
        if (!oldest) break;
        texture_drop(oldest);
    }
}

static void sink_forget(const uint32_t *pixels)
{
    Texture **at = bucket_of(pixels);
    while (*at) {
        if ((*at)->pixels == pixels) texture_drop(at);
        else at = &(*at)->next;
    }
}

static const ModsOverlaySink sink = {sink_begin, sink_fill, sink_image, sink_end, sink_forget};

const struct ModsOverlaySink *OverlayGl_Sink(void) { return &sink; }

void OverlayGl_Draw(int win_w, int win_h, int output_w, int output_h)
{
    GLuint bound = 0;
    int clipped = 0, begun = 0, *clip_now = NULL;
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < quad_count; i++) {
        const Quad *quad = &quads[i];
        int whole = quad->clip[0] <= 0 && quad->clip[1] <= 0 && quad->clip[2] >= win_w && quad->clip[3] >= win_h;
        if (bound != quad->texture || (!whole != clipped) ||
            (!whole && memcmp(clip_now, quad->clip, sizeof(quad->clip)))) {
            if (begun) glEnd();
            begun = 0;
            if (bound != quad->texture) glBindTexture(GL_TEXTURE_2D, bound = quad->texture);
            if (!whole != clipped) {
                clipped = !whole;
                if (clipped) glEnable(GL_SCISSOR_TEST);
                else glDisable(GL_SCISSOR_TEST);
            }
            if (clipped) {
                /* In the window's pixels, from its bottom. */
                float sx = (float)output_w / (float)win_w, sy = (float)output_h / (float)win_h;
                int x0 = (int)((float)quad->clip[0] * sx + 0.5f), x1 = (int)((float)quad->clip[2] * sx + 0.5f);
                int y0 = (int)((float)quad->clip[1] * sy + 0.5f), y1 = (int)((float)quad->clip[3] * sy + 0.5f);
                glScissor(x0, output_h - y1, x1 - x0, y1 - y0);
                clip_now = (int *)quad->clip;
            }
        }
        if (!begun) glBegin(GL_QUADS);
        begun = 1;
        glColor4fv(quad->color);
        glTexCoord2f(0, 0); glVertex2i(quad->x, quad->y);
        glTexCoord2f(1, 0); glVertex2i(quad->x + quad->w, quad->y);
        glTexCoord2f(1, 1); glVertex2i(quad->x + quad->w, quad->y + quad->h);
        glTexCoord2f(0, 1); glVertex2i(quad->x, quad->y + quad->h);
    }
    if (begun) glEnd();
    if (clipped) glDisable(GL_SCISSOR_TEST);
    glColor4f(1, 1, 1, 1);
}

void OverlayGl_Lost(void)
{
    for (unsigned i = 0; i < BUCKETS; i++) {
        while (buckets[i]) {
            Texture *texture = buckets[i];
            buckets[i] = texture->next;
            free(texture);
        }
    }
    texture_bytes = 0;
    white = 0;
    quad_count = dead_count = 0;
}
#endif
