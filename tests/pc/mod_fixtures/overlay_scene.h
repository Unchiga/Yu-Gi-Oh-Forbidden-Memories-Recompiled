#ifndef MEMORIES_TEST_OVERLAY_SCENE_H
#define MEMORIES_TEST_OVERLAY_SCENE_H
/* What the overlay fixture (overlay.c) draws, in order, and the test
 * (mods_overlay_test.c) changes between passes. */
#include <stdint.h>

#define SCENE_ITEMS 48
#define SCENE_PICTURES 3
#define SCENE_PICTURE_SIZE 24

enum { ITEM_FILL, ITEM_PICTURE, ITEM_TEXT, ITEM_KINDS };
typedef struct {
    int count;
    struct {
        int kind, x, y, w, h, alpha, picture, scale;
        int clipped, clip[4]; /* host->clip's x, y, w, h around the item */
        uint32_t rgb;
        char text[12];
    } items[SCENE_ITEMS];
    uint32_t pictures[SCENE_PICTURES][SCENE_PICTURE_SIZE * SCENE_PICTURE_SIZE];
    uint32_t *kept;     /* a picture from alloc_image, drawn last; */
    int kept_changed;   /* set: image_changed first */
} OverlayScene;

#endif
