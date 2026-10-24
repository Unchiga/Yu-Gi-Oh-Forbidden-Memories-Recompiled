#ifndef MEMORIES_GUEST_IMAGE_LOADER_H
#define MEMORIES_GUEST_IMAGE_LOADER_H
#include "exe_magic.h"
#include "image.h"
#include "pc/memory_endian.h"
#include <string.h>

typedef struct MemoriesExeImage {
    const uint8_t *data;
    uint32_t address, size, entry;
} MemoriesExeImage;

/* Parsing never touches guest RAM. Entry validation is optional for the fixed
 * backend's existing data-only image contract; the image bounds are shared. */
static inline int Memories_ParseExe(const uint8_t *data, size_t length,
                                    int require_entry, MemoriesExeImage *out)
{
    MemoriesExeImage image;
    if (!data || !out || length < 0x800 || !Memories_ExeMagicOk(data)) return -1;
    image.data = data + 0x800;
    image.address = Memories_LoadLE32(data + 0x18);
    image.size = Memories_LoadLE32(data + 0x1c);
    image.entry = Memories_LoadLE32(data + 0x10);
    if (image.address < MEMORIES_GUEST_RAM + 0x10000u ||
        image.size > MEMORIES_GUEST_RAM_SIZE ||
        image.address - MEMORIES_GUEST_RAM > MEMORIES_GUEST_RAM_SIZE - image.size ||
        image.size > length - 0x800) return -1;
    if (require_entry && (image.size < 4 || image.entry < image.address ||
        image.entry - image.address > image.size - 4 || (image.entry & 3u))) return -1;
    *out = image;
    return 0;
}
#endif
