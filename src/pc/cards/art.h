#ifndef MEMORIES_PC_CARDS_ART_H
#define MEMORIES_PC_CARDS_ART_H
/* Custom card artwork (art.c): the first 0x3060 bytes of a card's art record,
 * made from a mod's PNGs and its name. */
#include <stddef.h>

#define CARD_ART_WIDTH 102
#define CARD_ART_HEIGHT 96
#define CARD_THUMB_WIDTH 40
#define CARD_THUMB_HEIGHT 32
#define CARD_TITLE_WIDTH 96
#define CARD_TITLE_HEIGHT 14

#define CARD_ART_PIXELS 0x0000
#define CARD_ART_CLUT 0x2640
#define CARD_TITLE_PIXELS 0x2840
#define CARD_TITLE_BYTES 0x2A0
#define CARD_THUMB_PIXELS 0x2AE0     /* also the start of the duel's 0x580-byte block */
#define CARD_THUMB_CLUT 0x2FE0
#define CARD_THUMB_BLOCK 0x580
#define CARD_ART_RECORD 0x3060

/* A Free Duel portrait: 48x48 at 8 bits a pixel, then a 64-entry palette
 * (FREE_DUEL_PORTRAIT_IMAGE_SIZE and _RECORD_SIZE say the same in the game's
 * constants). */
#define PORTRAIT_SIDE 48
#define PORTRAIT_PIXELS 2304
#define PORTRAIT_RECORD 2432

/* 1 on success; `why` says what went wrong otherwise. */
int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size);

/* The same, but a part of the picture that is mostly transparent stays see-through (index 0, as the field
 * art's): what is behind the picture shows there. A card's "art_transparent". */
int CardArt_FromImageSeeThrough(const char *path, unsigned char *record, char *why, size_t why_size);
int CardArt_ThumbnailFromImageSeeThrough(const char *path, unsigned char *record, char *why, size_t why_size);
int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size);
/* Like CardArt_FromImage, but keeps the PNG's own transparency as a
 * silhouette instead of flattening it to black: for "field_art"
 * (notes/more-cards.md), the 2D Monsters mod's cutout alone, never a
 * card's real record. A binary cutout (an alpha under 128 is fully out),
 * not a softly feathered edge. */
int CardArt_FieldArtFromImage(const char *path, unsigned char *record, char *why, size_t why_size);
/* The rectangle of the PNG those take a w x h picture from (the middle at
 * that shape), and the PNG's size: for the full-resolution picture a
 * texture pack draws above the console's resolution (cards.c). 0 when the
 * file is not a PNG. */
/* 1 when the PNG has a pixel that is mostly transparent: a card's picture is then made with see-through
 * parts (CardArt_FromImage), and its larger copy is registered to keep them (cards.c). */
int CardArt_HasHoles(const char *path);

int CardArt_Crop(const char *path, int w, int h, int *x, int *y, int *cw, int *ch, int *width, int *height);
/* The title plate alone, CARD_TITLE_BYTES, what a record holds at
 * CARD_TITLE_PIXELS. */
int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why, size_t why_size);
/* A picture of any size with see-through parts, for the title screen
 * (title_images.c): the PNG stretched to w x h, one byte a texel, entry 0
 * clear and 1-255 its colors in `clut` (256 BGR555 entries). */
int CardArt_IndexedImage(const char *path, int w, int h, unsigned char *indices, unsigned short *clut, char *why,
                         size_t why_size);
/* Several such pictures at once, all `w` x `h` and sharing the one palette
 * in `clut`: the colors of all of them are reduced together, so a texel the
 * same color in two of them is the same entry in both (a duelist's bust,
 * busts.c, draws a talking frame's changes over its base by that). */
int CardArt_IndexedImages(const char *const *paths, int count, int w, int h, unsigned char *const *indices,
                          unsigned short *clut, char *why, size_t why_size);
/* A sheet of `count` pictures one under another, each its share of the
 * sheet's rows, made into `count` sets of texels as CardArt_IndexedImage
 * makes one, all through the one palette (title_images.c's animations):
 * the sheet's own colors, exactly, when it has 255 or fewer. */
int CardArt_IndexedSheet(const char *path, int count, int w, int h, unsigned char *const *indices,
                         unsigned short *clut, char *why, size_t why_size);
/* The PNG's size; 0 when it is not one. */
int CardArt_ImageSize(const char *path, int *width, int *height);
/* PORTRAIT_RECORD bytes: a duelist's face for the Free Duel grid. */
int CardArt_PortraitFromImage(const char *path, unsigned char *record, char *why, size_t why_size);
/* A guardian star's icon, CARD_ICON_SIDE square at 4 bits a pixel (128
 * bytes, the low nibble first), from a PNG (stars.h): with `palette` its 16
 * colors (the disc's stars'), else the PNG's own 15 in `clut` (entry 0
 * transparent). */
#define CARD_ICON_SIDE 16
int CardArt_IconFromImage(const char *path, const unsigned short *palette, unsigned char *pixels,
                          unsigned short *clut, char *why, size_t why_size);
/* `name` in UTF-8. 0 when no serif font could be found; the plate is left
 * as it was. */
int CardArt_TitleFromName(const char *name, unsigned char *plate);
/* The same plate at `factor` pixels per texel, as the plate's 4-bit inks
 * (0 clear, 1 the darkest to 7 the faintest), CARD_TITLE_WIDTH * factor by
 * CARD_TITLE_HEIGHT * factor into `indices`, `pitch` bytes a row: what HD
 * text draws a card's title from (hd_text.h). 0 when no serif font. */
int CardArt_TitlePicture(const char *name, int factor, unsigned char *indices, int pitch);
/* The serif face the plates are set in (an FT_Face), opened the first time;
 * NULL when there is none. The duel's Magic, Equip, Trap and Ritual are set
 * in it too (hd_text.h). */
void *CardArt_SerifFace(void);

#endif
