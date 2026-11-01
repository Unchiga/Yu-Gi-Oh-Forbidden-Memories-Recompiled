/* The registry-only tests compile fields.c but do not initialise the texture
 * renderer.  Keep that dependency out of their small executables; the
 * dedicated fields_test.c supplies recording stubs of its own to test the
 * five-strip registration and rollback path. */
#include "pc/render/texture_pack.h"

int TexturePack_AddDiscImageGroup(unsigned group, const char *file, const char *archive, unsigned offset, int words,
                                  int rows, int bpp, unsigned clut_offset, int clut_entries, int stride,
                                  int crop_left, int crop_width, int source_x, int source_y, int source_w,
                                  int source_h, unsigned rank)
{
    (void)group; (void)file; (void)archive; (void)offset; (void)words; (void)rows; (void)bpp; (void)clut_offset;
    (void)clut_entries; (void)stride; (void)crop_left; (void)crop_width; (void)source_x; (void)source_y;
    (void)source_w; (void)source_h; (void)rank;
    return 1;
}

int TexturePack_ImageSize(const char *path, int *width, int *height)
{
    (void)path; (void)width; (void)height;
    return 0;
}

unsigned TexturePack_EntryCount(void) { return 0; }
void TexturePack_RemoveEntriesFrom(unsigned mark) { (void)mark; }
void TexturePack_SelectGroup(unsigned group) { (void)group; }
