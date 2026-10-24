#include "types.h"
#include "pc/guest/translated_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    u8 image[0x810] = {0};
    u32 entry = 0xfeedfaceu;
    CHECK(memory != NULL);
    memcpy(image, "PS-X EXE", 8);
    Memories_WriteLE32(image + 0x10, 0x80010000u);
    Memories_WriteLE32(image + 0x18, 0x80010000u);
    Memories_WriteLE32(image + 0x1c, 16);
    memset(image + 0x800, 0xa5, 16);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == 0);
    CHECK(entry == 0x80010000u);
    CHECK(!memcmp(memory->ram + 0x10000, image + 0x800, 16));
    CHECK(memory->ram[0xffff] == 0 && memory->ram[0x10010] == 0);
    memset(memory->ram + 0x10000, 0x5a, 16);
    entry = 0xfeedfaceu;
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image) - 1, &entry) == -1);
    image[0] = 0;
    /* A community "free version" image ships its executable with the magic's
     * first byte wiped and the rest of the header unchanged: it loads. */
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == 0);
    image[0] = 'P';
    image[1] = 0;
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    image[1] = 'S';
    /* The loads that failed left the memory and the entry as they were. */
    memset(memory->ram + 0x10000, 0x5a, 16);
    entry = 0xfeedfaceu;
    Memories_WriteLE32(image + 0x1c, 0);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x1c, 1);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x1c, 0xffffffffu);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x1c, 16);
    Memories_WriteLE32(image + 0x10, 0x80010010u);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x10, 0x80010001u);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x18, 0x801ffffcu);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    Memories_WriteLE32(image + 0x18, 0x80000000u);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), &entry) == -1);
    CHECK(Memories_LoadTranslatedExe(NULL, image, sizeof(image), &entry) == -1);
    CHECK(Memories_LoadTranslatedExe(memory, NULL, sizeof(image), &entry) == -1);
    CHECK(Memories_LoadTranslatedExe(memory, image, sizeof(image), NULL) == -1);
    CHECK(Memories_LoadTranslatedExe(memory, image, 7, &entry) == -1);
    CHECK(entry == 0xfeedfaceu && memory->ram[0x10000] == 0x5a);
    free(memory);
    puts("Translated PS-X EXE loader: bounds, entry and failure atomicity passed");
    return 0;
}
