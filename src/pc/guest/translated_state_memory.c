/* Guest-token identities stay stable while ASLR moves their native storage. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "translated_state_memory.h"
#include "translated_runtime.h"
#include "state_io.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct SavedRegion {
    uint64_t host, length, identity;
    uint32_t guest, flags;
} SavedRegion;
struct MemoriesNativeMemoryState {
    unsigned count;
    SavedRegion *saved;
    uintptr_t *current;
    const uint8_t **payload;
    uint8_t *created;
    int applied;
    /* The saved regions by host address, and the span they cover (end
     * iterators included): relocation reads every native word of a state,
     * and most are no pointer into any region. */
    unsigned *order;
    uint64_t low, high;
};
static int stored(unsigned flags)
{
    return !(flags & MEMORIES_REGION_CONSTANT) && (flags & (MEMORIES_REGION_GAME | MEMORIES_REGION_HEAP | MEMORIES_REGION_MAPPING));
}
static int owned(unsigned flags) { return flags & (MEMORIES_REGION_HEAP | MEMORIES_REGION_MAPPING); }
static void release(void *host, size_t length, unsigned flags)
{
    if (flags & MEMORIES_REGION_MAPPING) munmap(host, length);
    else free(host);
}
static const GuestRuntimeRegion *find_region(uint32_t guest)
{
    for (unsigned i = 0; i < GuestRuntime_RegionCount(); ++i) {
        const GuestRuntimeRegion *region = GuestRuntime_Region(i);
        if (region->guest == guest) return region;
    }
    return NULL;
}
int Memories_NativeMemorySave(MemoriesState *state)
{
    unsigned count = GuestRuntime_RegionCount() + 1;
    SavedRegion *saved = calloc(count, sizeof(*saved));
    MemoriesStateField *fields = calloc(count + 2, sizeof(*fields));
    size_t field_count = 2;
    if (!saved || !fields) { free(saved); free(fields); return -1; }
    fields[0] = (MemoriesStateField){&count, sizeof(count)};
    fields[1] = (MemoriesStateField){saved, count * sizeof(*saved)};
    for (unsigned i = 0; i + 1 < count; ++i) {
        const GuestRuntimeRegion *region = GuestRuntime_Region(i);
        saved[i] = (SavedRegion){region->host, region->length, region->identity, region->guest, region->flags};
        if (stored(region->flags)) fields[field_count++] = (MemoriesStateField){(void *)region->host, region->length};
    }
    saved[count - 1] = (SavedRegion){(uintptr_t)GuestRuntime_Memory(), sizeof(MemoriesMemory), 0, 0x80000000u, 0};
    Memories_StateChunk(state, "arm64-regions", fields, field_count);
    free(fields); free(saved);
    return ferror(state->file) ? -1 : 0;
}
void Memories_NativeMemoryFree(MemoriesNativeMemoryState *pending)
{
    if (!pending) return;
    if (!pending->applied && pending->created && pending->current)
        for (unsigned i = 0; i < pending->count; ++i)
            if (pending->created[i]) release((void *)pending->current[i], pending->saved[i].length, pending->saved[i].flags);
    free(pending->saved); free(pending->current); free(pending->payload); free(pending->created);
    free(pending->order); free(pending);
}
static const SavedRegion *sorting;
static int by_host(const void *a, const void *b)
{
    uint64_t x = sorting[*(const unsigned *)a].host, y = sorting[*(const unsigned *)b].host;
    return x < y ? -1 : x > y;
}
/* Prepare has refused regions that share host bytes, so one region at most
 * holds an address. */
static void index_regions(MemoriesNativeMemoryState *pending)
{
    pending->low = UINT64_MAX;
    pending->high = 0;
    for (unsigned i = 0; i < pending->count; ++i) {
        const SavedRegion *saved = &pending->saved[i];
        pending->order[i] = i;
        if (saved->host < pending->low) pending->low = saved->host;
        if (saved->host + saved->length > pending->high) pending->high = saved->host + saved->length;
    }
    sorting = pending->saved;
    qsort(pending->order, pending->count, sizeof(*pending->order), by_host);
    sorting = NULL;
}
/* The region whose bytes hold `word`, or else the one it is the end of;
 * -1 for none. */
static int region_of(const MemoriesNativeMemoryState *pending, uint64_t word)
{
    unsigned first = 0, last = pending->count;
    const SavedRegion *saved;
    if (word < pending->low || word > pending->high) return -1;
    while (last - first > 1) {   /* the last region starting at or below word */
        unsigned middle = first + (last - first) / 2;
        if (pending->saved[pending->order[middle]].host <= word) first = middle;
        else last = middle;
    }
    if (!pending->count) return -1;
    saved = &pending->saved[pending->order[first]];
    if (word < saved->host) return -1;
    if (word - saved->host < saved->length) return (int)pending->order[first];
    if (word == saved->host + saved->length) return (int)pending->order[first];
    return -1;
}
MemoriesNativeMemoryState *Memories_NativeMemoryPrepare(MemoriesState *state, char *why, size_t size)
{
    size_t length = 0, at;
    unsigned count;
    const uint8_t *chunk = Memories_StateFindChunk(state, "arm64-regions", &length);
    MemoriesNativeMemoryState *pending = NULL;
    if (!chunk || length < sizeof(count)) goto invalid;
    memcpy(&count, chunk, sizeof(count));
    if (count > (length - sizeof(count)) / sizeof(SavedRegion)) goto invalid;
    at = sizeof(count) + count * sizeof(SavedRegion);
    pending = calloc(1, sizeof(*pending));
    if (!pending) goto invalid;
    pending->count = count;
    pending->saved = calloc(count ? count : 1, sizeof(*pending->saved));
    pending->current = calloc(count ? count : 1, sizeof(*pending->current));
    pending->payload = calloc(count ? count : 1, sizeof(*pending->payload));
    pending->created = calloc(count ? count : 1, 1);
    pending->order = calloc(count ? count : 1, sizeof(*pending->order));
    if (!pending->saved || !pending->current || !pending->payload || !pending->created || !pending->order) goto invalid;
    memcpy(pending->saved, chunk + sizeof(count), count * sizeof(SavedRegion));
    for (unsigned i = 0; i < count; ++i) {
        const SavedRegion *saved = &pending->saved[i];
        const GuestRuntimeRegion *now = find_region(saved->guest);
        if (i + 1 == count && saved->host > UINT32_MAX && saved->length <= UINT64_MAX - saved->host &&
            saved->guest == 0x80000000u && saved->length == sizeof(MemoriesMemory) && !saved->flags && !saved->identity) {
            pending->current[i] = (uintptr_t)GuestRuntime_Memory();
            continue;
        }
        if (!saved->length || saved->length > UINT64_MAX - saved->host ||
            saved->guest < 0x80200000u || saved->guest >= 0xf0000000u || saved->length > 0xf0000000ull - saved->guest) goto invalid;
        for (unsigned j = 0; j < i; ++j) {
            const SavedRegion *other = &pending->saved[j];
            if (saved->guest < other->guest + other->length && other->guest < saved->guest + saved->length) goto invalid;
            if (saved->host < other->host + other->length && other->host < saved->host + saved->length) goto invalid;
        }
        if (saved->flags & ~31u) goto invalid;
        if (owned(saved->flags) && saved->flags != MEMORIES_REGION_HEAP && saved->flags != MEMORIES_REGION_MAPPING) goto invalid;
        for (unsigned j = 0; j < GuestRuntime_RegionCount(); ++j) {
            const GuestRuntimeRegion *region = GuestRuntime_Region(j);
            if (saved->guest < region->guest + region->length && region->guest < saved->guest + saved->length &&
                region->guest != saved->guest && !owned(region->flags)) goto invalid;
        }
        if (!owned(saved->flags)) {
            if (!now || now->length != saved->length || now->identity != saved->identity || now->flags != saved->flags) goto invalid;
            pending->current[i] = now->host;
        } else if (now && now->flags == saved->flags && now->length == saved->length) pending->current[i] = now->host;
        else if (now && !owned(now->flags)) goto invalid;
        if (stored(saved->flags)) {
            if (saved->length > length - at) goto invalid;
            pending->payload[i] = chunk + at;
            at += saved->length;
        }
    }
    if (at != length) goto invalid;
    if (!count || pending->saved[count - 1].guest != 0x80000000u) goto invalid;
    if (GuestRuntime_ReserveRegions((size_t)GuestRuntime_RegionCount() + count)) goto invalid;
    index_regions(pending);
    for (unsigned i = 0; i < count; ++i) {
        const SavedRegion *saved = &pending->saved[i];
        if (owned(saved->flags) && !pending->current[i]) {
            void *host = saved->flags & MEMORIES_REGION_MAPPING
                ? mmap(NULL, saved->length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
                : malloc(saved->length);
            if (!host || host == MAP_FAILED) goto invalid;
            pending->current[i] = (uintptr_t)host;
            pending->created[i] = 1;
        }
    }
    return pending;
invalid:
    snprintf(why, size, "invalid ARM64 region layout or incompatible native allocations");
    Memories_NativeMemoryFree(pending);
    return NULL;
}
int Memories_NativeMemoryApply(MemoriesNativeMemoryState *pending)
{
    /* Preflight reserved all replacements and registry capacity. */
    for (unsigned j = 0; j < GuestRuntime_RegionCount();) {
        const GuestRuntimeRegion *region = GuestRuntime_Region(j);
        int remove = 0;
        for (unsigned i = 0; i < pending->count; ++i) {
            const SavedRegion *saved = &pending->saved[i];
            if (owned(saved->flags) &&
                saved->guest < region->guest + region->length && region->guest < saved->guest + saved->length &&
                region->host != pending->current[i]) { remove = 1; break; }
        }
        if (remove) {
            void *old = (void *)region->host;
            size_t length = region->length;
            unsigned flags = region->flags;
            GuestRuntime_UnregisterData(old);
            release(old, length, flags);
        } else ++j;
    }
    for (unsigned i = 0; i < pending->count; ++i) {
        const SavedRegion *saved = &pending->saved[i];
        const GuestRuntimeRegion *now = find_region(saved->guest);
        if (!owned(saved->flags) || (now && now->host == pending->current[i])) continue;
        if (now) {
            void *old = (void *)now->host;
            size_t length = now->length;
            unsigned flags = now->flags;
            GuestRuntime_UnregisterData(old);
            release(old, length, flags);
        }
        int failed = saved->flags & MEMORIES_REGION_MAPPING
            ? GuestRuntime_RegisterMapping((void *)pending->current[i], saved->length, saved->guest)
            : GuestRuntime_RegisterAllocationAt((void *)pending->current[i], saved->length, saved->guest);
        if (failed) return -1;
    }
    pending->applied = 1;
    return 0;
}
void Memories_NativeMemoryRelocate(MemoriesNativeMemoryState *pending, void *data, size_t size,
                                  uintptr_t old_image, uintptr_t new_image, size_t image_size)
{
    uint8_t *bytes = data;
    /* Scan full native words only. PS1 addresses and integers stay 32 bits. */
    for (size_t offset = 0; offset <= size && size - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
        uint64_t word, relocated;
        memcpy(&word, bytes + offset, sizeof(word));
        relocated = word;
        if (word >= old_image && word - old_image < image_size) relocated = new_image + (word - old_image);
        else {
            /* End iterators are valid C pointers: one past a region is
             * moved with it, unless the next region starts there. */
            int region = region_of(pending, word);
            if (region >= 0) relocated = pending->current[region] + (word - pending->saved[region].host);
        }
        if (word != relocated) memcpy(bytes + offset, &relocated, sizeof(relocated));
    }
}
/* Copy only guest game globals and managed heap payloads. Subsystems keep
 * explicit serializers; OS objects in native globals are never copied. */
void Memories_NativeMemoryRestorePayloads(MemoriesNativeMemoryState *pending,
                                         uintptr_t old_image, uintptr_t new_image, size_t image_size)
{
    for (unsigned i = 0; i < pending->count; ++i) {
        if (!pending->payload[i]) continue;
        void *host = (void *)pending->current[i];
        memcpy(host, pending->payload[i], pending->saved[i].length);
        Memories_NativeMemoryRelocate(pending, host, pending->saved[i].length, old_image, new_image, image_size);
    }
}
void Memories_NativeMemoryRemapGuests(MemoriesNativeMemoryState *pending, uint32_t from, uint32_t to, uint32_t size)
{
    if (!pending || !size) return;
    for (unsigned i = 0; i < pending->count; ++i) {
        if (!pending->payload[i]) continue;
        for (size_t at = 0; at + sizeof(uint32_t) <= pending->saved[i].length; at += sizeof(uint32_t)) {
            uint32_t word;
            memcpy(&word, pending->payload[i] + at, sizeof(word));
            if (word >= from && word - from <= size) {
                word = to + (word - from);
                memcpy((uint8_t *)pending->payload[i] + at, &word, sizeof(word));
                memcpy((uint8_t *)pending->current[i] + at, &word, sizeof(word));
            }
        }
    }
}
