/* Guest-token identities stay stable while ASLR moves their native storage. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "translated_state_symbols.h"
#include "translated_state_memory.h"
#include "translated_runtime.h"
#include "state_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct SavedRegion {
    uint64_t host, length, identity;
    uint32_t guest, flags;
} SavedRegion;
typedef struct RelocationRegion {
    uint64_t host, length;
    unsigned index;
} RelocationRegion;

struct MemoriesNativeMemoryState {
    unsigned count;
    SavedRegion *saved;
    uintptr_t *current;
    const uint8_t **payload;
    uint8_t *created;
    int applied;
    RelocationRegion *relocations;
};
static int compare_host(const void *left, const void *right)
{
    const RelocationRegion *a = left, *b = right;
    return (a->host > b->host) - (a->host < b->host);
}

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
    free(pending->relocations);
    free(pending->saved); free(pending->current); free(pending->payload); free(pending->created); free(pending);
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
    if (!pending->saved || !pending->current || !pending->payload || !pending->created) goto invalid;
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
    /* Saved regions are non-overlapping, but registry order follows guest
     * tokens rather than host addresses. Build once, before applying state. */
    pending->relocations = calloc(count, sizeof(*pending->relocations));
    if (!pending->relocations) goto invalid;
    for (unsigned i = 0; i < count; ++i)
        pending->relocations[i] = (RelocationRegion){pending->saved[i].host, pending->saved[i].length, i};
    qsort(pending->relocations, count, sizeof(*pending->relocations), compare_host);
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
/* One native word of a saved state, where it belongs now: an address in
 * the old image (classified by the OLD image's size) moves with the image,
 * or for another build's state by the two symbol tables
 * (translated_state_symbols.c), whose answer is certain or a refusal; an
 * address in a saved region moves with that region. The pre-load check
 * (Memories_NativeSymbolsScan over the same words with the same `stack`)
 * has refused every state with a word that cannot be carried, so one here
 * is a broken invariant, not a guess to make. */
static uint64_t relocate_word(MemoriesNativeMemoryState *pending, uint64_t word, uintptr_t old_image,
                              uintptr_t new_image, size_t old_size, int stack)
{
    if (word >= old_image && word - old_image < old_size) {
        uint64_t mapped = word - old_image;
        if (Memories_NativeSymbolsActive() && Memories_NativeSymbolsMap(word - old_image, stack, &mapped)) {
            fprintf(stderr, "memories-pc: ARM64 state: offset 0x%llx passed the pre-load check and cannot be "
                    "carried\n", (unsigned long long)(word - old_image));
            abort();
        }
        return new_image + mapped;
    }
    if (pending->count && word >= pending->relocations[0].host) {
        /* Upper bound selects the owning region at a shared endpoint.
         * In a gap, the preceding region may own a one-past iterator.
         * Pixels/PS1 words outside these ranges need no fixup. */
        unsigned low = 0, high = pending->count;
        const RelocationRegion *last = &pending->relocations[high - 1];
        if (word <= last->host + last->length) {
            while (low < high) {
                unsigned mid = low + (high - low) / 2;
                if (pending->relocations[mid].host <= word) low = mid + 1;
                else high = mid;
            }
            const RelocationRegion *region = &pending->relocations[low - 1];
            if (word - region->host <= region->length)
                return pending->current[region->index] + (word - region->host);
        }
    }
    return word;
}
void Memories_NativeMemoryRelocate(MemoriesNativeMemoryState *pending, void *data, size_t size,
                                  uintptr_t old_image, uintptr_t new_image, size_t old_size, int stack)
{
    uint8_t *bytes = data;
    /* Scan full native words only. PS1 addresses and integers stay 32 bits. */
    for (size_t offset = 0; offset <= size && size - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
        uint64_t word, relocated;
        memcpy(&word, bytes + offset, sizeof(word));
        relocated = relocate_word(pending, word, old_image, new_image, old_size, stack);
        if (word != relocated) memcpy(bytes + offset, &relocated, sizeof(relocated));
    }
}
size_t Memories_NativeMemoryUncarried(MemoriesNativeMemoryState *pending, uintptr_t old_image)
{
    size_t missing = 0;
    for (unsigned i = 0; i < pending->count; ++i)
        if (pending->payload[i])
            missing += Memories_NativeSymbolsScan(pending->payload[i], pending->saved[i].length, old_image, 0, 1);
    return missing;
}
/* Copy only guest game globals and managed heap payloads. Subsystems keep
 * explicit serializers; OS objects in native globals are never copied. */
void Memories_NativeMemoryRestorePayloads(MemoriesNativeMemoryState *pending,
                                         uintptr_t old_image, uintptr_t new_image, size_t old_size)
{
    for (unsigned i = 0; i < pending->count; ++i) {
        if (!pending->payload[i]) continue;
        void *host = (void *)pending->current[i];
        memcpy(host, pending->payload[i], pending->saved[i].length);
        Memories_NativeMemoryRelocate(pending, host, pending->saved[i].length, old_image, new_image, old_size, 0);
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
