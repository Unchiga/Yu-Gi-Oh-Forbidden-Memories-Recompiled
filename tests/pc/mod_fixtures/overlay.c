/* A code mod for the overlay drawn again in part (mods_overlay_test.c):
 * its overlay draws a scene of fills, pictures with alpha, text and clips,
 * in order, from the state it provides as "scene", which the test changes
 * between passes as a mod's screen would change between frames. */
#include "pc/mods/modapi.h"
#include "overlay_scene.h"

static const MemoriesModHost *host;
static OverlayScene scene;

static void overlay(void)
{
    for (int i = 0; i < scene.count && i < SCENE_ITEMS; i++) {
        const int *clip = scene.items[i].clip;
        if (scene.items[i].clipped) host->clip(host, clip[0], clip[1], clip[2], clip[3]);
        switch (scene.items[i].kind) {
        case ITEM_FILL:
            host->fill(host, scene.items[i].x, scene.items[i].y, scene.items[i].w, scene.items[i].h,
                       scene.items[i].rgb, (unsigned)scene.items[i].alpha);
            break;
        case ITEM_PICTURE:
            host->draw_image(host, scene.pictures[scene.items[i].picture], SCENE_PICTURE_SIZE, SCENE_PICTURE_SIZE,
                             scene.items[i].x, scene.items[i].y, scene.items[i].w, scene.items[i].h,
                             (unsigned)scene.items[i].alpha);
            break;
        default:
            host->draw_text_alpha(host, scene.items[i].x, scene.items[i].y, scene.items[i].text, scene.items[i].rgb,
                                  scene.items[i].scale, (unsigned)scene.items[i].alpha);
            break;
        }
        if (scene.items[i].clipped) host->clip(host, 0, 0, 0, 0);
    }
    if (scene.kept_changed) host->image_changed(host, scene.kept);
    scene.kept_changed = 0;
    if (scene.kept) host->draw_image(host, scene.kept, 4, 4, 0, 0, 4, 4, 255);
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    if (host->api < 23) return 0;
    mod->api = MEMORIES_MOD_API;
    mod->overlay = overlay;
    scene.kept = host->alloc_image(host, 4, 4);
    if (!scene.kept) return 0;
    host->provide(host, "scene", &scene);
    return 1;
}
