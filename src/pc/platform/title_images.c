/* The title's pictures of a mod's own (title_images.h). Each PNG is made
 * into the game's kind of texture -- 8 bits a texel through a 256-color
 * palette, entry 0 clear -- at the size it is drawn, put in VRAM the title
 * leaves unused, and drawn with the game's own polygons in the place of the
 * picture it replaces, so what the game draws over it (the menu, the dimming)
 * still does. At an internal resolution above the console's the PNG itself
 * is drawn instead, at its own resolution, as a mod card's art is
 * (TexturePack_AddMade).
 *
 * The VRAM (a dump at the title, 2026-09-29): (0,256)-(479,495) is the
 * intro movie's 24-bit picture, idle while the title shows and written
 * again by the movie, which is why every opening of the title uploads the
 * pictures again; (768,256)-(1023,511) is empty. The game's own title
 * sheets are at (512,256)-(767,511) and (896,0)-(1023,255); the rows
 * under the movie's picture, (64,496)-(511,511), are empty too. */
#include "title_images.h"
#include "menu_label.h"
#include "menu_letters.h"
#include "pc/cards/art.h"
#include "pc/mods/mods.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_pack.h"
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixed places, in 16-bit VRAM words (row 256 on), with the most each
 * may measure in texels. The four backgrounds -- the title's and the
 * menus', 4:3 and widescreen -- share one place, wide enough for a
 * widescreen one, and the one drawn is put there then (TitleImages_Draw). */
static const struct { int x, y, clut_y, max_w, max_h; } fixed[TITLE_IMAGE_ITEMS] = {
    {0, 256, 500, 320, 240},                   /* background, in the movie's picture */
    {224, 256, 501, 320, 240},                 /* logo, beside it */
    {768, 256, 502, 320, 120},                 /* copyright */
    {768, 376, 503, 320, 120},                 /* PUSH START BUTTON */
    {0, 256, 504, 320, 240},                   /* the menus' background */
    {0, 256, 505, TITLE_WIDE_WIDTH, 240},      /* the title's, widescreen */
    {0, 256, 506, TITLE_WIDE_WIDTH, 240},      /* the menus', widescreen */
};
enum { CLUT_X = 768, STRIP = 128, ITEM_MAX_W = 256, ITEM_MAX_H = 64, ITEM_GUESS_H = 32, SHARED_WORDS = 224 };
/* An added picture ("images") at most this size: what the widest region
 * below holds without the backgrounds (448 texels), and the screen's
 * height. Without them a picture fits in 192 texels across. */
enum { PICTURE_MAX_W = 320, PICTURE_MAX_H = 240 };

/* The items' pictures go where the others leave room: beside the logo in
 * the movie's picture, and right of the copyright line, and in the
 * backgrounds' and logo's places when the title has none of its own there
 * (in words, x0 to x1 across and rows 256 to 496; needs_free -1 always, 0
 * without backgrounds, else without that picture). Their palettes go in
 * the rows under the movie's picture and around the fixed ones. */
static const struct { int x0, x1, needs_free; } regions[] = {
    {384, 480, -1}, {928, 1024, -1}, {0, SHARED_WORDS, TITLE_IMAGE_BACKGROUND}, {224, 384, TITLE_IMAGE_LOGO}};
enum { REGION_TOP = 256, REGION_BOTTOM = 496, REGIONS = sizeof(regions) / sizeof(regions[0]) };
static const struct { int x, y; } item_cluts[] = {
    {768, 496}, {768, 497}, {768, 498}, {768, 499}, {768, 507}, {768, 508}, {768, 509},
    {768, 510}, {768, 511}, {256, 496}, {256, 497}, {256, 498}, {256, 499}, {256, 500}, {256, 501}, {256, 502},
    {256, 503}, {256, 504}, {256, 505}, {256, 506}, {256, 507}, {256, 508}, {256, 509}, {256, 510}, {256, 511}};
enum { ITEM_CLUTS = sizeof(item_cluts) / sizeof(item_cluts[0]) };

int TitleImages_IsBackground(int which)
{
    return which == TITLE_IMAGE_BACKGROUND || which == TITLE_IMAGE_MENU_BACKGROUND ||
           which == TITLE_IMAGE_WIDE_BACKGROUND || which == TITLE_IMAGE_WIDE_MENU_BACKGROUND;
}

typedef struct {
    char file[TITLE_PATH];
    int width, height, ready;    /* its texels */
    int draw_w, draw_h;          /* the size it is drawn at, in the game's pixels: an item's at its "scale" */
    int x, y, clut_x, clut_y;    /* where it is in VRAM */
    unsigned char *texels;
    unsigned short clut[256];
} Picture;

static Picture pictures[TITLE_IMAGES];
/* Which background is in the shared place, -1 none, and which of its
 * frames when it is animated. */
static int resident = -1, resident_frame = -1;

/* An animated background ("frames"): every picture made at the
 * background's size through one palette and kept here, the one due
 * uploaded into the shared place over the last -- no VRAM past a still
 * one's. Kept from one opening of the title to the next while the frames
 * are the same (`key`): made again only when they change. */
typedef struct {
    int count, time, tick;
    unsigned long key;
    unsigned char *texels[TITLE_MAX_SHEET_FRAMES];
    unsigned short clut[256];
} Animation;
static Animation animations[TITLE_IMAGE_ITEMS];

/* The size a PNG is drawn at: a background fills the screen; a sprite is
 * "width" by "height", one of them and the PNG's shape, or the PNG's own
 * size made smaller by a whole factor until it fits (a 4x drawing of the
 * logo lands at the console's size). Even across: two texels a word. */
static void measure(int which, const TitleImage *image, int png_w, int png_h, int *w, int *h)
{
    int picture = which >= TITLE_IMAGE_PICTURES;
    int factor = 1, max_w = which < TITLE_IMAGE_ITEMS ? fixed[which].max_w : picture ? PICTURE_MAX_W : ITEM_MAX_W;
    int max_h = which < TITLE_IMAGE_ITEMS ? fixed[which].max_h : picture ? PICTURE_MAX_H : ITEM_MAX_H;
    if (TitleImages_IsBackground(which)) {
        *w = max_w;
        *h = 240;
        return;
    }
    if (image->width && image->height) {
        *w = image->width;
        *h = image->height;
    } else if (image->width) {
        *w = image->width;
        *h = (int)((long)png_h * image->width / png_w);
    } else if (image->height) {
        *h = image->height;
        *w = (int)((long)png_w * image->height / png_h);
    } else {
        /* An item's is taken for one drawn at a whole multiple of the
         * retail entries' size, 28 rows (up to ITEM_GUESS_H). */
        int guess_h = which < TITLE_IMAGE_ITEMS || picture ? max_h : ITEM_GUESS_H;
        while (png_w / factor > max_w || png_h / factor > guess_h) factor++;
        *w = png_w / factor;
        *h = png_h / factor;
    }
    /* Whatever was asked, no bigger than the slot, keeping the shape. */
    if (*w > max_w) { *h = (int)((long)*h * max_w / *w); *w = max_w; }
    if (*h > max_h) { *w = (int)((long)*w * max_h / *h); *h = max_h; }
    *w = (*w + 1) & ~1;
    if (*w < 2) *w = 2;
    if (*h < 1) *h = 1;
}

/* An item's size at `scale` percent of w x h, rounded, and the texels it
 * is made of: as many, or, past what an item's place holds (ITEM_MAX_W x
 * ITEM_MAX_H), the most that keep its shape, drawn stretched to the size
 * at the console's resolution (the PNG itself above it). */
static void scaled(int w, int h, int scale, int *draw_w, int *draw_h, int *texels_w, int *texels_h)
{
    *draw_w = (w * scale + 50) / 100;
    *draw_h = (h * scale + 50) / 100;
    if (*draw_w < 1) *draw_w = 1;
    if (*draw_h < 1) *draw_h = 1;
    *texels_w = *draw_w;
    *texels_h = *draw_h;
    if (*texels_w > ITEM_MAX_W) { *texels_h = *texels_h * ITEM_MAX_W / *texels_w; *texels_w = ITEM_MAX_W; }
    if (*texels_h > ITEM_MAX_H) { *texels_w = *texels_w * ITEM_MAX_H / *texels_h; *texels_h = ITEM_MAX_H; }
    *texels_w = (*texels_w + 1) & ~1;
    if (*texels_w < 2) *texels_w = 2;
    if (*texels_h < 1) *texels_h = 1;
}

/* Remade only when the file or its size changes. `scale` is an item's
 * size in percent (100 for the rest). */
static int make(int which, const TitleImage *image, int scale)
{
    Picture *picture = &pictures[which];
    int png_w, png_h, w, h, draw_w, draw_h;
    char why[1300];
    if (!CardArt_ImageSize(image->file, &png_w, &png_h)) {
        Mods_Note(image->mod, "title: %s is not a PNG it could read", image->file);
        return 0;
    }
    measure(which, image, png_w, png_h, &w, &h);
    draw_w = w;
    draw_h = h;
    if (scale != 100) scaled(draw_w, draw_h, scale, &draw_w, &draw_h, &w, &h);
    picture->draw_w = draw_w;
    picture->draw_h = draw_h;
    if (picture->texels && picture->width == w && picture->height == h && !strcmp(picture->file, image->file))
        return 1;
    free(picture->texels);
    memset(picture, 0, sizeof(*picture));
    picture->texels = malloc((size_t)w * h);
    if (!picture->texels) return 0;
    if (!CardArt_IndexedImage(image->file, w, h, picture->texels, picture->clut, why, sizeof(why))) {
        Mods_Note(image->mod, "title: %s", why);
        free(picture->texels);
        picture->texels = NULL;
        return 0;
    }
    snprintf(picture->file, sizeof(picture->file), "%s", image->file);
    picture->width = w;
    picture->height = h;
    picture->draw_w = draw_w;
    picture->draw_h = draw_h;
    /* Above the console's resolution, the PNG itself: known by these bytes
     * when they are uploaded below. */
    if ((png_w > w || png_h > h) &&
        !TexturePack_AddMadeSeeThrough(picture->texels, w / 2, h, 8, picture->clut, 256, image->file, 0, 0, png_w,
                                       png_h))
        fprintf(stderr, "memories-pc: title: %s is drawn at the console's size only\n", image->file);
    return 1;
}

/* An item's picture, as it is (selected 0) or with the cursor on it: the
 * mod's PNG, or the button of words made for its label. 0 for none. */
static int item_image(const TitleItem *item, int selected, TitleImage *out)
{
    char why[1300];
    const TitleImage *given = selected ? &item->selected : &item->image;
    if (given->file[0]) {
        *out = *given;
        if (selected && !out->width && !out->height) {
            out->width = item->image.width;
            out->height = item->image.height;
        }
        return 1;
    }
    if (item->image.file[0] || !item->label[0]) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->mod, sizeof(out->mod), "%s", item->mod);
    /* "font": "game": the entries' own letters on an entry's own frame,
     * while the game has every letter of it (menu_letters.h). */
    if (item->font_game) {
        int index = (int)(item - TitleConfig_Get()->items);
        if (MenuLetters_Make(item->label, selected, index < TITLE_ENTRIES ? index : -1,
                             (MENU_LABEL_FACTOR * item->scale + 99) / 100, out->file, sizeof(out->file), &out->width,
                             &out->height, why, sizeof(why)))
            return 1;
        Mods_Note(item->mod, "menu: %s: %s; set in the serif instead", item->name, why);
    }
    /* Its PNG as sharp at the item's size as at 100 at the highest
     * internal resolution. */
    if (!MenuLabel_Make(item->label, selected, (MENU_LABEL_FACTOR * item->scale + 99) / 100, out->file,
                        sizeof(out->file), &out->width, &out->height, why, sizeof(why))) {
        Mods_Note(item->mod, "menu: %s: %s", item->name, why);
        return 0;
    }
    return 1;
}

typedef struct { int x, y, shelf; } Shelf;

/* A place for w x h texels (w even) in the regions free this time. */
static int place(Shelf *shelves, const int *usable, int w, int h, int *x, int *y)
{
    int r, words = w / 2;
    for (r = 0; r < (int)REGIONS; r++) {
        Shelf *shelf = &shelves[r];
        if (!usable[r] || words > regions[r].x1 - regions[r].x0) continue;
        if (shelf->x + words > regions[r].x1) {
            shelf->x = regions[r].x0;
            shelf->y += shelf->shelf;
            shelf->shelf = 0;
        }
        if (shelf->y + h > REGION_BOTTOM) continue;
        *x = shelf->x;
        *y = shelf->y;
        shelf->x += words;
        if (h > shelf->shelf) shelf->shelf = h;
        return 1;
    }
    return 0;
}

static void forget_frames(Animation *animation)
{
    int i;
    for (i = 0; i < animation->count; i++) free(animation->texels[i]);
    memset(animation, 0, sizeof(*animation));
}

/* What a set of frames is made from, for knowing it again: the files, the
 * kind, the size made at. */
static unsigned long frames_key(const TitleImage *frames, int count, int sheet, int w, int h)
{
    unsigned long key = 2166136261UL;
    int i;
    const char *c;
    for (i = 0; i < count; i++)
        for (c = frames[i].file; *c; c++) key = (key ^ (unsigned char)*c) * 16777619UL;
    key = (key ^ (unsigned long)count) * 16777619UL;
    key = (key ^ (unsigned long)sheet) * 16777619UL;
    key = (key ^ (unsigned long)w) * 16777619UL;
    return ((key ^ (unsigned long)h) * 16777619UL) | 1;
}

/* How many frames a sheet holds: as many as its height takes of pictures
 * the shape of the background (`w` x `h` texels) at the sheet's width. 0,
 * and noted, when that is not a whole number of them or too many. */
static int sheet_frames(const TitleImage *sheet, int w, int h, int *png_w, int *png_h)
{
    long frame_h, count;
    if (!CardArt_ImageSize(sheet->file, png_w, png_h)) {
        Mods_Note(sheet->mod, "title: %s is not a PNG it could read", sheet->file);
        return 0;
    }
    frame_h = (long)*png_w * h / w;
    count = frame_h > 0 ? (*png_h + frame_h / 2) / frame_h : 0;
    if (count < 1 || labs(count * frame_h - *png_h) > count || count > TITLE_MAX_SHEET_FRAMES) {
        Mods_Note(sheet->mod, "title: %s is not frames of %d x %d one under another (at most %d)", sheet->file,
                  *png_w, (int)frame_h, TITLE_MAX_SHEET_FRAMES);
        return 0;
    }
    return (int)count;
}

/* A background's frames, each at the size its picture was made at, all
 * through one palette: a list of PNGs or a sheet of them. Above the
 * console's resolution each frame's own part of its PNG is drawn. */
static void make_frames(int which, const TitleImage *frames, int count, int sheet, int time)
{
    Animation *animation = &animations[which];
    const Picture *picture = &pictures[which];
    const char *paths[TITLE_MAX_FRAMES];
    char why[1300];
    int i, png_w = 0, png_h = 0, w = picture->width, h = picture->height;
    unsigned long key = frames_key(frames, count, sheet, w, h);
    if (animation->count && animation->key == key) {
        animation->time = time;
        return;
    }
    forget_frames(animation);
    if (sheet && !(count = sheet_frames(&frames[0], w, h, &png_w, &png_h))) return;
    for (i = 0; i < count; i++) {
        animation->texels[i] = malloc((size_t)w * h);
        if (!animation->texels[i]) {
            animation->count = i;
            forget_frames(animation);
            return;
        }
        if (!sheet) paths[i] = frames[i].file;
    }
    animation->count = count;
    if (!(sheet ? CardArt_IndexedSheet(frames[0].file, count, w, h, animation->texels, animation->clut, why,
                                       sizeof(why))
                : CardArt_IndexedImages(paths, count, w, h, animation->texels, animation->clut, why, sizeof(why)))) {
        Mods_Note(frames[0].mod, "title: %s", why);
        forget_frames(animation);
        return;
    }
    for (i = 0; i < count; i++) {
        int top = 0, rows;
        if (sheet) {
            top = (int)((long)png_h * i / count);
            rows = (int)((long)png_h * (i + 1) / count) - top;
        } else if (!CardArt_ImageSize(frames[i].file, &png_w, &png_h)) {
            continue;
        } else {
            rows = png_h;
        }
        if ((png_w > w || rows > h) &&
            !TexturePack_AddMadeSeeThrough(animation->texels[i], w / 2, h, 8, animation->clut, 256,
                                           frames[sheet ? 0 : i].file, 0, top, png_w, rows))
            fprintf(stderr, "memories-pc: title: %s is drawn at the console's size only\n", frames[sheet ? 0 : i].file);
    }
    animation->time = time;
    animation->key = key;
}

/* A 4:3 background animated with the middle of the widescreen one's frames,
 * when it has an image but no frames of its own: the 320 texels the 4:3
 * picture shows of the 428, through the same palette. Above the console's
 * resolution, the same part of each frame's PNG. */
static void middle_frames(int which, int wide, const TitleImage *frames, int sheet)
{
    Animation *animation = &animations[which];
    const Animation *source = &animations[wide];
    const Picture *picture = &pictures[which];
    unsigned long key = source->key * 31UL + 7UL;
    int i, y, png_w = 0, png_h = 0;
    if (!source->count || pictures[wide].width != TITLE_WIDE_WIDTH || picture->width != 320 ||
        picture->height != pictures[wide].height)
        return;
    if (animation->count && animation->key == key) {
        animation->time = source->time;
        return;
    }
    forget_frames(animation);
    for (i = 0; i < source->count; i++) {
        animation->texels[i] = malloc((size_t)picture->width * picture->height);
        if (!animation->texels[i]) {
            forget_frames(animation);
            return;
        }
        animation->count = i + 1;
        for (y = 0; y < picture->height; y++)
            memcpy(animation->texels[i] + (size_t)y * picture->width,
                   source->texels[i] + (size_t)y * TITLE_WIDE_WIDTH + TITLE_WIDE_MARGIN, (size_t)picture->width);
    }
    memcpy(animation->clut, source->clut, sizeof(animation->clut));
    for (i = 0; i < animation->count; i++) {
        const char *file = frames[sheet ? 0 : i].file;
        int top = 0, rows;
        if (!CardArt_ImageSize(file, &png_w, &png_h) || png_w <= TITLE_WIDE_WIDTH) continue;
        rows = png_h;
        if (sheet) {
            top = (int)((long)png_h * i / animation->count);
            rows = (int)((long)png_h * (i + 1) / animation->count) - top;
        }
        TexturePack_AddMadeSeeThrough(animation->texels[i], picture->width / 2, picture->height, 8, animation->clut,
                                      256, file, (int)((long)png_w * TITLE_WIDE_MARGIN / TITLE_WIDE_WIDTH), top,
                                      (int)((long)png_w * 320 / TITLE_WIDE_WIDTH), rows);
    }
    animation->time = source->time;
    animation->key = key;
}

/* The frames a background slot is animated with, how many and whether a
 * sheet. */
static const TitleImage *frames_of(const TitleConfig *config, int which, int *count, int *sheet)
{
    const TitleBackground *background = &config->background[which == TITLE_IMAGE_MENU_BACKGROUND ||
                                                             which == TITLE_IMAGE_WIDE_MENU_BACKGROUND];
    int wide = which == TITLE_IMAGE_WIDE_BACKGROUND || which == TITLE_IMAGE_WIDE_MENU_BACKGROUND;
    *count = wide ? background->wide_frame_count : background->frame_count;
    *sheet = wide ? background->wide_frame_sheet : background->frame_sheet;
    return wide ? background->wide_frames : background->frames;
}

static int same_frames(const TitleBackground *a, const TitleBackground *b)
{
    int i;
    if (a->frame_count != b->frame_count || a->wide_frame_count != b->wide_frame_count ||
        a->frame_sheet != b->frame_sheet || a->wide_frame_sheet != b->wide_frame_sheet)
        return 0;
    for (i = 0; i < a->frame_count; i++)
        if (strcmp(a->frames[i].file, b->frames[i].file)) return 0;
    for (i = 0; i < a->wide_frame_count; i++)
        if (strcmp(a->wide_frames[i].file, b->wide_frames[i].file)) return 0;
    return a->frame_time == b->frame_time;
}

void TitleImages_Prepare(const TitleConfig *config)
{
    Shelf shelves[REGIONS];
    int usable[REGIONS], which, i, cluts = 0;
    for (which = 0; which < TITLE_IMAGES; which++) pictures[which].ready = 0;
    for (which = 0; which < TITLE_IMAGE_ITEMS; which++) {
        const TitleImage *image = which == TITLE_IMAGE_BACKGROUND ? &config->background[0].image :
                                  which == TITLE_IMAGE_MENU_BACKGROUND ? &config->background[1].image :
                                  which == TITLE_IMAGE_WIDE_BACKGROUND ? &config->background[0].wide_image :
                                  which == TITLE_IMAGE_WIDE_MENU_BACKGROUND ? &config->background[1].wide_image :
                                  &config->layers[which - 1].image;
        const TitleImage *frames = NULL;
        int count = 0, sheet = 0;
        if (TitleImages_IsBackground(which)) {
            frames = frames_of(config, which, &count, &sheet);
            /* A list of frames without a still picture: the first stands
             * for it. */
            if (!image->file[0] && count && !sheet) image = &frames[0];
        }
        /* The menus' background the title's own: drawn as the title's. */
        if ((which == TITLE_IMAGE_MENU_BACKGROUND && !strcmp(image->file, config->background[0].image.file) &&
             same_frames(&config->background[0], &config->background[1])) ||
            (which == TITLE_IMAGE_WIDE_MENU_BACKGROUND && !strcmp(image->file, config->background[0].wide_image.file) &&
             same_frames(&config->background[0], &config->background[1]))) {
            forget_frames(&animations[which]);
            continue;
        }
        pictures[which].ready = image->file[0] && make(which, image, 100);
        if (pictures[which].ready && count)
            make_frames(which, frames, count, sheet,
                        config->background[which == TITLE_IMAGE_MENU_BACKGROUND ||
                                           which == TITLE_IMAGE_WIDE_MENU_BACKGROUND].frame_time);
        else if (count || which == TITLE_IMAGE_WIDE_BACKGROUND || which == TITLE_IMAGE_WIDE_MENU_BACKGROUND)
            forget_frames(&animations[which]);   /* a 4:3 one without frames of its own: below */
        pictures[which].x = fixed[which].x;
        pictures[which].y = fixed[which].y;
        pictures[which].clut_x = CLUT_X;
        pictures[which].clut_y = fixed[which].clut_y;
        /* Only a background of a mod's own draws through its palette. */
        if (TitleImages_IsBackground(which) && pictures[which].ready)
            SoftGpu_PictureOnlyClut(pictures[which].clut_x, pictures[which].clut_y);
    }
    /* A 4:3 background with a picture but no frames of its own, the
     * widescreen one animated: the middle of those frames. */
    for (i = 0; i < 2; i++) {
        int which = i ? TITLE_IMAGE_MENU_BACKGROUND : TITLE_IMAGE_BACKGROUND, count, sheet, wide_count, wide_sheet;
        int wide = i ? TITLE_IMAGE_WIDE_MENU_BACKGROUND : TITLE_IMAGE_WIDE_BACKGROUND;
        const TitleImage *wide_frames;
        frames_of(config, which, &count, &sheet);
        wide_frames = frames_of(config, wide, &wide_count, &wide_sheet);
        if (count) continue;
        if (pictures[which].ready && pictures[wide].ready && animations[wide].count)
            middle_frames(which, wide, wide_frames, wide_sheet);
        else
            forget_frames(&animations[which]);
    }
    for (i = 0; i < (int)REGIONS; i++) {
        int taken = regions[i].needs_free;
        shelves[i].x = regions[i].x0;
        shelves[i].y = REGION_TOP;
        shelves[i].shelf = 0;
        usable[i] = taken < 0 || (taken == TITLE_IMAGE_BACKGROUND
                                      ? !pictures[TITLE_IMAGE_BACKGROUND].ready && !pictures[TITLE_IMAGE_MENU_BACKGROUND].ready &&
                                            !pictures[TITLE_IMAGE_WIDE_BACKGROUND].ready &&
                                            !pictures[TITLE_IMAGE_WIDE_MENU_BACKGROUND].ready
                                      : !pictures[taken].ready);
    }
    for (i = 0; i < TITLE_ITEMS; i++) {
        const TitleItem *item = &config->items[i];
        int selected;
        if (!item->used || item->hidden || !TitleConfig_Drawn(item)) continue;
        for (selected = 0; selected < 2; selected++) {
            TitleImage image;
            Picture *picture = &pictures[TITLE_IMAGE_ITEM(i, selected)];
            if (!item_image(item, selected, &image) || !make(TITLE_IMAGE_ITEM(i, selected), &image, item->scale))
                continue;
            if (cluts == ITEM_CLUTS || !place(shelves, usable, picture->width, picture->height, &picture->x, &picture->y)) {
                Mods_Note(item->mod, "menu: no room left for %s's pictures", item->name);
                continue;
            }
            picture->clut_x = item_cluts[cluts].x;
            picture->clut_y = item_cluts[cluts].y;
            cluts++;
            picture->ready = 1;
        }
    }
    /* The added pictures where the items leave room, after them: the
     * menus come first. */
    for (i = 0; i < config->pictures; i++) {
        const TitlePicture *added = &config->picture[i];
        Picture *picture = &pictures[TITLE_IMAGE_PICTURE(i)];
        if (!make(TITLE_IMAGE_PICTURE(i), &added->image, 100)) continue;
        if (cluts == ITEM_CLUTS || !place(shelves, usable, picture->width, picture->height, &picture->x, &picture->y)) {
            Mods_Note(added->image.mod, "title: no room left for the picture %s", added->image.file);
            continue;
        }
        picture->clut_x = item_cluts[cluts].x;
        picture->clut_y = item_cluts[cluts].y;
        cluts++;
        picture->ready = 1;
    }
    /* A picture made just now is known by its bytes only once the packs
     * have sorted it in, which they do between frames: now, before the
     * upload, or the first opening of the title would draw it at the
     * console's size. */
    TexturePack_Service();
    resident = -1;
    resident_frame = -1;
    for (which = 0; which < TITLE_IMAGES; which++) {
        Picture *picture = &pictures[which];
        RECT rect;
        if (!picture->ready) continue;
        /* The shared place gets the title's; the others go there when drawn. */
        if (TitleImages_IsBackground(which) && which != TITLE_IMAGE_BACKGROUND) {
            rect.x = CLUT_X;
            rect.y = (short)picture->clut_y;
            rect.w = 256;
            rect.h = 1;
            LoadImage(&rect, (u32 *)picture->clut);
            continue;
        }
        rect.x = (short)picture->x;
        rect.y = (short)picture->y;
        rect.w = (short)(picture->width / 2);
        rect.h = (short)picture->height;
        LoadImage(&rect, (u32 *)picture->texels);
        rect.x = (short)picture->clut_x;
        rect.y = (short)picture->clut_y;
        rect.w = 256;
        rect.h = 1;
        LoadImage(&rect, (u32 *)picture->clut);
        if (which == TITLE_IMAGE_BACKGROUND) resident = which;
    }
}

int TitleImages_Ready(int which, int *width, int *height)
{
    if (which < 0 || which >= TITLE_IMAGES || !pictures[which].ready) return 0;
    if (width) *width = pictures[which].draw_w;
    if (height) *height = pictures[which].draw_h;
    return 1;
}

/* In strips of at most STRIP texels, each inside one texture page, as the
 * game draws its own background (MainMenu_DrawFrontendBackground); at its
 * size (TitleImages_Ready), each strip's share of it. */
void TitleImages_Draw(int which, void *ot, int depth, int x, int y, int r, int g, int b, int blend)
{
    const Picture *picture;
    POLY_FT4 strip;
    int left;
    if (!TitleImages_Ready(which, NULL, NULL)) return;
    picture = &pictures[which];
    if (TitleImages_IsBackground(which)) {
        Animation *animation = &animations[which];
        int frame = -1;
        if (animation->count) {
            if (resident != which) animation->tick = 0;
            frame = animation->tick++ / animation->time % animation->count;
        }
        if (resident != which || resident_frame != frame) {
            RECT rect;
            rect.x = (short)picture->x;
            rect.y = (short)picture->y;
            rect.w = (short)(picture->width / 2);
            rect.h = (short)picture->height;
            LoadImage(&rect, (u32 *)(frame >= 0 ? animation->texels[frame] : picture->texels));
            /* The palette with the first frame: all of them share it. */
            if (resident != which || (frame < 0) != (resident_frame < 0)) {
                rect.x = (short)picture->clut_x;
                rect.y = (short)picture->clut_y;
                rect.w = 256;
                rect.h = 1;
                LoadImage(&rect, (u32 *)(frame >= 0 ? animation->clut : picture->clut));
            }
            resident = which;
            resident_frame = frame;
        }
    }
    setPolyFT4(&strip);
    if (blend) setSemiTrans(&strip, 1);
    strip.r0 = (u8)r;
    strip.g0 = (u8)g;
    strip.b0 = (u8)b;
    strip.clut = getClut(picture->clut_x, picture->clut_y);
    for (left = 0; left < picture->width; left += STRIP) {
        int width = picture->width - left < STRIP ? picture->width - left : STRIP;
        int word = picture->x + left / 2, page = word & ~63, u = (word - page) * 2;
        int v = picture->y - 256;
        strip.tpage = getTPage(1, blend ? 1 : 0, page, 256);
        strip.x0 = strip.x2 = (short)(x + left * picture->draw_w / picture->width);
        strip.x1 = strip.x3 = (short)(x + (left + width) * picture->draw_w / picture->width);
        strip.y0 = strip.y1 = (short)y;
        strip.y2 = strip.y3 = (short)(y + picture->draw_h);
        strip.u0 = strip.u2 = (u8)u;
        /* The far edge is the texel after the last, as a polygon's is not
         * drawn: one to one, no texel twice. A strip starts at most 126
         * texels into its page, so it ends by 254. */
        strip.u1 = strip.u3 = (u8)(u + width);
        strip.v0 = strip.v1 = (u8)v;
        strip.v2 = strip.v3 = (u8)(v + picture->height);
        GsSortPoly(&strip, (GsOT *)ot, (unsigned short)depth);
    }
}
