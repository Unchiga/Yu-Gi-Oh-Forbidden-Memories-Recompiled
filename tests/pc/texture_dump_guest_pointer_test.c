/* Texture provenance crosses translated guest tokens and resolved host
 * pointers. This models both an aliased guest address and state RAM rebasing. */
#include "pc/guest/translated_runtime.h"
#include "pc/render/texture_dump.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static MemoriesMemory memories[2];
static MemoriesMemory *active_memory = &memories[0];
static uint16_t registered_pixels[64];
static GuestRuntimeRegion registered_region = {
    (uintptr_t)registered_pixels, sizeof(registered_pixels), 0xd0001000u, 0, 0};

int GuestRuntime_IsBound(void) { return active_memory != NULL; }
MemoriesMemory *GuestRuntime_Memory(void) { return active_memory; }
unsigned GuestRuntime_RegionCount(void) { return 1; }
const GuestRuntimeRegion *GuestRuntime_Region(unsigned index) { return index == 0 ? &registered_region : NULL; }

void *GuestRuntime_ResolveData(void *pointer, size_t length)
{
    uintptr_t address = (uintptr_t)pointer;
    uint32_t guest = (uint32_t)address;
    uint32_t canonical = (guest >= 0xa0000000u && guest < 0xc0000000u)
                             ? 0x80000000u | (guest & 0x1fffffffu)
                             : guest;
    size_t offset;
    assert(address <= UINT32_MAX);
    if (canonical >= 0x80000000u && canonical - 0x80000000u < MEMORIES_RAM_SIZE) {
        offset = canonical - 0x80000000u;
        assert(length <= MEMORIES_RAM_SIZE - offset);
        return active_memory->ram + offset;
    }
    assert(guest >= registered_region.guest && guest - registered_region.guest < registered_region.length);
    offset = guest - registered_region.guest;
    assert(length <= registered_region.length - offset);
    return (uint8_t *)registered_region.host + offset;
}

uint32_t GuestRuntime_EncodePointer(void *pointer)
{
    uintptr_t host = (uintptr_t)pointer;
    uintptr_t base = (uintptr_t)active_memory->ram;
    if (host >= base && host - base < MEMORIES_RAM_SIZE) return 0x80000000u + (uint32_t)(host - base);
    base = registered_region.host;
    assert(host >= base && host - base < registered_region.length);
    return registered_region.guest + (uint32_t)(host - base);
}

const uint16_t *SoftGpu_Vram(void) { return NULL; }

static uint16_t *source(void)
{
    return (uint16_t *)(void *)(active_memory->ram + 0x1000);
}

static void fill_source(void)
{
    size_t i;
    for (i = 0; i < 64; i++) source()[i] = (uint16_t)(0x1234u + i * 37u);
}

int main(void)
{
    const unsigned lba = 7, sector_offset = 13;
    const unsigned byte_count = 8 * 8 * sizeof(uint16_t);
    const unsigned first_tag = lba * 2048u + sector_offset + 1;
    assert(TextureDump_EnableTags());

    fill_source();
    /* The disc callback uses an aliased guest token; the upload observes the
     * resolved host pointer. Both must identify the same guest RAM bytes. */
    TextureDump_Delivered((const void *)(uintptr_t)0xa0001000u, byte_count, (int)lba, sector_offset);
    TextureDump_Loaded(20, 30, 8, 8, source());
    assert(TextureDump_Tags[30 * SOFT_GPU_WIDTH + 20] == first_tag);
    assert(TextureDump_Tags[37 * SOFT_GPU_WIDTH + 27] == first_tag + 126);

    /* A state restore may recreate host RAM elsewhere while preserving guest
     * addresses. The in-process delivery ring remains valid by guest identity. */
    memcpy(memories[1].ram + 0x1000, memories[0].ram + 0x1000, byte_count);
    active_memory = &memories[1];
    TextureDump_Loaded(40, 50, 8, 8, source());
    assert(TextureDump_Tags[50 * SOFT_GPU_WIDTH + 40] == first_tag);

    /* The invalidation hook can again receive a token. Changing bytes must
     * end the delivery's claim, even after the memory rebase. */
    TextureDump_Written((const void *)(uintptr_t)0xa0001000u, byte_count);
    source()[0] ^= 0xffffu;
    TextureDump_Loaded(60, 70, 8, 8, source());
    assert(TextureDump_Tags[70 * SOFT_GPU_WIDTH + 60] == 0);

    /* External allocations registered as guest data use their own guest
     * range, not their native host address, as the stable provenance key. */
    for (size_t i = 0; i < 64; i++) registered_pixels[i] = (uint16_t)(0xabcd - i * 19u);
    TextureDump_Delivered((const void *)(uintptr_t)registered_region.guest, byte_count, 9, 21);
    TextureDump_Loaded(80, 90, 8, 8, registered_pixels);
    assert(TextureDump_Tags[90 * SOFT_GPU_WIDTH + 80] == 9 * 2048u + 22u);
    /* Bulk clearing must preserve wrapping and leave adjacent cells intact. */
    assert(TextureDump_EnableShadow());
    for (int y = 0; y < SOFT_GPU_HEIGHT; y++) {
        for (int x = 0; x < SOFT_GPU_WIDTH; x++) {
            TextureDump_Tags[y * SOFT_GPU_WIDTH + x] = 11;
            for (int sub = 0; sub < 4; sub++) *TextureDump_Cell(x, y, sub) = 17;
        }
    }
    TextureDump_Touched(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    TextureDump_Cleared(SOFT_GPU_WIDTH - 2, SOFT_GPU_HEIGHT - 1, 4, 2);
    for (int y = 0; y < SOFT_GPU_HEIGHT; y++) {
        for (int x = 0; x < SOFT_GPU_WIDTH; x++) {
            int cleared = (y == 0 || y == SOFT_GPU_HEIGHT - 1) &&
                          (x < 2 || x >= SOFT_GPU_WIDTH - 2);
            assert(TextureDump_Tags[y * SOFT_GPU_WIDTH + x] == (cleared ? 0u : 11u));
            for (int sub = 0; sub < 4; sub++)
                assert(*TextureDump_Cell(x, y, sub) == (cleared ? 0 : 17));
        }
    }
    puts("Texture provenance: guest pointers, state rebasing and wrapped span clears agree");
    return 0;
}
