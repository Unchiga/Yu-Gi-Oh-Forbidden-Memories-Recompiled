#include "pc/memory_endian.h"
/* Memories_Resolve for the mapped ILP32 image: a guest address is already a
 * host address, so this only keeps the range and alignment checks. */
#include "image.h"
#include "pc/memory.h"

void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment)
{
    uint32_t physical = address & UINT32_C(0x1fffffff);
    (void)memory;
    if (!alignment || (alignment & (alignment - 1)) || (address & (alignment - 1)) ||
        (address >= UINT32_C(0x20000000) && (address < MEMORIES_GUEST_RAM ||
                                             address >= UINT32_C(0xc0000000)))) {
        return NULL;
    }
    if (physical >= UINT32_C(0x10000) && physical < MEMORIES_GUEST_RAM_SIZE) {
#ifdef _WIN32
        /* Windows holds most of the low mirror (image.c): use guest RAM. */
        if (address < UINT32_C(0x20000000)) address |= MEMORIES_GUEST_RAM;
#endif
        return length <= MEMORIES_GUEST_RAM_SIZE - physical ? (void *)(uintptr_t)address : NULL;
    }
    if (physical >= MEMORIES_GUEST_SCRATCHPAD_RETAIL &&
        physical < MEMORIES_GUEST_SCRATCHPAD_RETAIL + MEMORIES_GUEST_SCRATCHPAD_SIZE && address < UINT32_C(0xa0000000)) {
        /* Either view of the scratchpad: the port's, which every host maps (image.h). */
        return length <= MEMORIES_GUEST_SCRATCHPAD_RETAIL + MEMORIES_GUEST_SCRATCHPAD_SIZE - physical
                   ? (void *)(uintptr_t)(physical | UINT32_C(0x80000000)) : NULL;
    }
    if (Memories_ModPacketBase && physical >= Memories_ModPacketPhysical &&
        physical - Memories_ModPacketPhysical < Memories_ModPacketSize && address < UINT32_C(0xa0000000)) {
        /* The mods' packet area (memory.h), mapped at its direct address. */
        return length <= Memories_ModPacketPhysical + Memories_ModPacketSize - physical
                   ? (void *)(uintptr_t)(physical | MEMORIES_GUEST_RAM) : NULL;
    }
    return NULL;
}

uint32_t Memories_ReadLE32(const uint8_t *bytes)
{
    return Memories_LoadLE32(bytes);
}
void Memories_WriteLE32(uint8_t *bytes, uint32_t value)
{
    Memories_StoreLE32(bytes, value);
}
