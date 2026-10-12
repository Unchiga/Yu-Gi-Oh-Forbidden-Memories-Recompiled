#include "pc/memory_endian.h"
#include "memory.h"
#include "memory_span.h"
#ifdef MEMORIES_TRANSLATED
#include "guest/translated_runtime.h"
#endif

void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment)
{
#ifdef MEMORIES_TRANSLATED
    /* Native LIBGS/render adapters pass the original mapped-image token. */
    if (!GuestRuntime_IsHostAddress((uintptr_t)memory)) memory = GuestRuntime_Memory();
#endif
    return Memories_ResolveSpan(memory, address, length, alignment);
}

uint32_t Memories_ReadLE32(const uint8_t *bytes)
{
    return Memories_LoadLE32(bytes);
}
void Memories_WriteLE32(uint8_t *bytes, uint32_t value)
{
    Memories_StoreLE32(bytes, value);
}
