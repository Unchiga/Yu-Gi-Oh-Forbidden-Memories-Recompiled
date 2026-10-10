#ifndef MEMORIES_PC_TEXTURE_PACK_H
#define MEMORIES_PC_TEXTURE_PACK_H
#include <stdint.h>

/* A texture pack: a directory of PNGs named by where the images come from
 * on the disc, as tools/pc/extract_images.py writes them, with its
 * manifest.json. When an upload tags VRAM words with their disc offsets
 * (texture_dump.c), the words an image covers get its pixels in the shadow,
 * and a primitive whose palette is the one the image was extracted through
 * samples the shadow instead of VRAM. A pack image may be any size: it is
 * resampled to the texture's own size for the console's resolution, and
 * sampled at its own for an internal resolution above it. Packs add up; a
 * mod names its pack with "textures" in its manifest (src/pc/mods). `rank`
 * is the pack's place in the mods' load order: where two packs read the
 * same words the same way, the higher rank's image is the one drawn.
 * Returns the number of images indexed (0 when every one is switched off,
 * below), -1 when the pack cannot be used, and writes into `problems` (may be
 * NULL) a line on the entries that were left out and why, for the Mods
 * window: a file missing or not a PNG, outside the pack, measures out of
 * range. An entry with a "setting" is one part of the pack the owning mod
 * switches with that setting: `part` says 1 while it is on, 0 while off,
 * -1 when the mod declares no such setting (a problem; the entry is used).
 * `part` may be NULL. */
#include <stddef.h>
int TexturePack_Load(const char *directory, unsigned rank, int (*part)(const char *setting, void *context),
                     void *context, char *problems, size_t problems_size);
/* Direct mod.json "assets": named {"image": "path.png", "setting": "optional"}
 * objects, resolved relative to the mod directory. Same lifecycle and rendering
 * as packs; the engine supplies geometry from its built-in catalog. */
struct JsonValue;
int TexturePack_LoadAssets(const char *directory, const struct JsonValue *assets, unsigned rank,
                           int (*part)(const char *, void *), void *context, char *problems, size_t size);
/* Or mod.json's "assets": "<directory>", where the pictures are not listed
 * at all: every PNG under it whose path is a name in the catalog replaces
 * that image, so assets/monster_type/dragon.png is monster_type/dragon. */
int TexturePack_LoadAssetFolder(const char *directory, unsigned rank, int (*part)(const char *, void *),
                                void *context, char *problems, size_t size);
void TexturePack_Unload(void);
/* Once a frame, on the main thread: reads what uploads asked for (an
 * upload can come from the interrupt tick, where reading is not safe). */
void TexturePack_Service(void);

/* An image the port makes instead of reading it from the disc: a mod
 * card's art (cards.c), whose bytes art.c makes from the mod's PNG at the
 * console's size. An upload of exactly `pixels` (words x rows at bpp) or of
 * `clut` (clut_entries) is known by its bytes (texture_dump.h, recall), and
 * texels read through that palette take the PNG at `file`, cut to the
 * rectangle x, y, w, h of it, as a pack's image does: resampled at 1x, at
 * its own resolution above. Identical blocks share one place (from
 * TEXTURE_MADE_BASE up); the same picture twice is kept once. Made images
 * are kept across the packs' loads and unloads. 1 added, 0 not. */
int TexturePack_AddMade(const void *pixels, int words, int rows, int bpp, const void *clut, int clut_entries,
                        const char *file, int x, int y, int w, int h);
/* The same for a picture with see-through parts (the title's,
 * title_images.c): the PNG keeps its alpha instead of lying over black, so
 * above the console's resolution its clear parts show what is under them
 * and its edges are soft. */
int TexturePack_AddMadeSeeThrough(const void *pixels, int words, int rows, int bpp, const void *clut,
                                  int clut_entries, const char *file, int x, int y, int w, int h);

/* A picture placed directly in a software-GPU texture bank.  Its ordinary
 * indexed texels remain the 1x fallback; above 1x renderers sample this PNG
 * instead.  The rectangle is in page-local texels. */
int TexturePack_AddBankSprite(int bank, int page_x, int page_y, int depth, int u, int v, int w, int h,
                              const char *file);
/* As above, but sample `source_x`, `source_y`, `source_w`, `source_h` from
 * the PNG.  A zero width and height mean the entire PNG. */
int TexturePack_AddBankSpriteCrop(int bank, int page_x, int page_y, int depth, int u, int v, int w, int h,
                                  const char *file, int source_x, int source_y, int source_w, int source_h);
int TexturePack_BankEntryFor(int bank, int page_x, int page_y, int depth, int u, int v);
int TexturePack_BankEntryForRegion(int bank, int page_x, int page_y, int depth, int u0, int v0, int u1, int v1);
int TexturePack_BankSample(int bank, int page_x, int page_y, int depth, int u, int v, uint32_t *rgb);
int TexturePack_BankEntryRect(int entry, int *u, int *v, int *w, int *h);
int TexturePack_BankEntrySource(int entry, int *x, int *y, int *w, int *h);
enum { TEXTURE_BANK_OWNER_STARS = 1, TEXTURE_BANK_OWNER_LAYOUT, TEXTURE_BANK_OWNER_DUEL_UI };
void TexturePack_BankSpritesUseOwner(unsigned owner);
void TexturePack_BankSpritesClear(unsigned owner);

/* For a renderer that samples the pack's images itself, at their own
 * resolution (gl_picture.c). The entry (its index + 1) whose image replaces
 * the texel a primitive starts at, as the software pass decides it, when
 * that image is loaded; 0 otherwise. The entry's image, and how it maps
 * onto the texture's texels. The maps from every VRAM word to the entry
 * painted there (index + 1, 0 none) and its place in it (row << 16 | word).
 * The generation changes whenever the entries do, the map generation
 * whenever a word of the maps does. */
int TexturePack_EntryFor(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v);
/* Readings of the same words (one geometry, several depths or palettes)
 * are entries in a row; the maps name the first, the head, whichever the
 * primitive's palette picks. */
int TexturePack_EntryHead(int entry);
int TexturePack_EntryImage(int entry, const unsigned char **rgba, int *width, int *height, int *crop_left,
                           int *crop_width, int *rows, int *texels_per_word);
unsigned TexturePack_Generation(void);
unsigned TexturePack_MapGeneration(void);
const uint16_t *TexturePack_EntryMap(void);
const uint32_t *TexturePack_PlaceMap(void);
#endif
