#include "pc/guest/translated_state_memory.h"
#include "pc/guest/translated_runtime.h"
#include "pc/guest/state_io.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* No game beside the test: no symbol table, so states stay same-build
 * (translated_state_symbols.c). */
int Memories_SymbolTablePath(char *out, size_t size)
{
    if (out && size) out[0] = 0;
    return -1;
}

int main(void)
{
    MemoriesMemory *old_ram = calloc(1, sizeof(*old_ram)), *new_ram = calloc(1, sizeof(*new_ram));
    uint64_t old_global[6], new_global[6] = {0}, old_constant = 22, new_constant = 11;
    uint64_t adjacent[4] = {0}, moved[2] = {0};
    uint64_t *heap = malloc(3 * sizeof(*heap));
    uint32_t heap_token;
    MemoriesState save = {0, tmpfile(), NULL, 0}, load;
    MemoriesNativeMemoryState *pending;
    char why[128];
    uint8_t header[16] = {0}, *image;
    long size;
    assert(old_ram && new_ram && heap && save.file);
    assert(!GuestRuntime_Bind(old_ram));
    GuestRuntime_RegisterGlobal(old_global, sizeof(old_global), 1, MEMORIES_REGION_GAME);
    GuestRuntime_RegisterGlobal(&old_constant, sizeof(old_constant), 2, MEMORIES_REGION_CONSTANT);
    GuestRuntime_RegisterGlobal(adjacent, 2 * sizeof(uint64_t), 3, MEMORIES_REGION_GAME);
    GuestRuntime_RegisterGlobal(adjacent + 2, 2 * sizeof(uint64_t), 4, MEMORIES_REGION_GAME);
    GuestRuntime_RegisterAllocation(heap, 3 * sizeof(*heap));
    heap_token = GuestRuntime_EncodePointer(heap);
    old_global[0] = (uintptr_t)heap;
    old_global[1] = 0x100000123ull;
    /* Guest text cursors can be one past the final byte. */
    old_global[2] = ((uint64_t)0x90000011u << 32) | 0x90000010u;
    old_global[3] = ((uint64_t)0x9000000fu << 32) | 0x90000000u;
    /* The second region stays put while its adjacent predecessor moves.
     * A pointer owned by the second must not become the first's endpoint. */
    old_global[4] = (uintptr_t)(adjacent + 2);
    old_global[5] = (uintptr_t)(adjacent + 4);
    heap[0] = 42;
    heap[1] = (uintptr_t)old_ram->ram + 32;
    heap[2] = (uintptr_t)(heap + 3);
    uint32_t *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    assert(!GuestRuntime_RegisterMapping(mapping, 4096, 0x9ff00000u));
    mapping[17] = 0x12345678;
    Memories_StateWrite(&save, header, sizeof(header));
    assert(!Memories_NativeMemorySave(&save));
    size = ftell(save.file);
    assert(size > 16);
    image = malloc((size_t)size);
    assert(image);
    rewind(save.file);
    assert(fread(image, 1, (size_t)size, save.file) == (size_t)size);
    fclose(save.file);
    load = (MemoriesState){1, NULL, image, (size_t)size};
    /* Rebind to different storage, like a second process under ASLR. */
    assert(!GuestRuntime_Bind(new_ram));
    GuestRuntime_RegisterGlobal(new_global, sizeof(new_global), 1, MEMORIES_REGION_GAME);
    GuestRuntime_RegisterGlobal(&new_constant, sizeof(new_constant), 2, MEMORIES_REGION_CONSTANT);
    GuestRuntime_RegisterGlobal(moved, sizeof(moved), 3, MEMORIES_REGION_GAME);
    GuestRuntime_RegisterGlobal(adjacent + 2, 2 * sizeof(uint64_t), 4, MEMORIES_REGION_GAME);
    /* The running game may have split the saved heap span into smaller
     * allocations. Preflight replaces every overlapping owned span. */
    uint64_t *small = malloc(sizeof(*small)), *neighbor = malloc(sizeof(*neighbor));
    assert(small && neighbor);
    GuestRuntime_RegisterAllocation(small, sizeof(*small));
    GuestRuntime_RegisterAllocation(neighbor, sizeof(*neighbor));
    assert(GuestRuntime_EncodePointer(small) == heap_token);
    assert(GuestRuntime_EncodePointer(neighbor) < heap_token + 3 * sizeof(*heap));
    /* Restore over an existing mapping at the same guest address whose
     * length changed since the save. It must be unmapped, not heap-freed. */
    uint32_t *resized_mapping = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(resized_mapping != MAP_FAILED);
    assert(!GuestRuntime_RegisterMapping(resized_mapping, 8192, 0x9ff00000u));
    resized_mapping[17] = 0xdeadbeef;
    pending = Memories_NativeMemoryPrepare(&load, why, sizeof(why));
    assert(pending);
    assert(!Memories_NativeMemoryApply(pending));
    Memories_NativeMemoryRemapGuests(pending, 0x90000000u, 0x91000000u, 16);
    Memories_NativeMemoryRestorePayloads(pending, 0x100000000ull, 0x120000000ull, 0x1000);
    assert(new_global[1] == 0x120000123ull);
    assert(new_constant == 11);
    assert(new_global[4] == (uintptr_t)(adjacent + 2));
    assert(new_global[5] == (uintptr_t)(adjacent + 4));
    assert(new_global[2] == (((uint64_t)0x90000011u << 32) | 0x91000010u));
    assert(new_global[3] == (((uint64_t)0x9100000fu << 32) | 0x91000000u));
    assert(new_global[0] != (uintptr_t)heap);
    assert(GuestRuntime_EncodePointer((void *)(uintptr_t)new_global[0]) == heap_token);
    assert(((uint64_t *)(uintptr_t)new_global[0])[0] == 42);
    assert(((uint64_t *)(uintptr_t)new_global[0])[1] == (uintptr_t)new_ram->ram + 32);
    assert(((uint64_t *)(uintptr_t)new_global[0])[2] == new_global[0] + 3 * sizeof(*heap));
    uint32_t *restored_mapping = GuestRuntime_ResolveData((void *)(uintptr_t)0x9ff00000u, 4096);
    assert(restored_mapping != mapping && restored_mapping[17] == 0x12345678);
    Memories_NativeMemoryFree(pending);
    /* Invalid containers are rejected before modifying running storage. */
    load.image_size--;
    assert(!Memories_NativeMemoryPrepare(&load, why, sizeof(why)));
    assert(new_global[1] == 0x120000123ull);
    free((void *)(uintptr_t)new_global[0]);
    assert(!munmap(mapping, 4096) && !munmap(restored_mapping, 4096));
    GuestRuntime_Reset();
    free(image); free(heap); free(old_ram); free(new_ram);
    puts("Native state memory: stable tokens, heap recreation, ASLR fixups, constants and preflight rejection passed");
    return 0;
}
