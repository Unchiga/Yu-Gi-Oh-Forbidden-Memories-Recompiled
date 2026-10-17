#include "../../types.h"
#include "translated_runtime.h"
#include "pc/memory_span.h"
#include "pc/compat/ot_chain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUNCTION_LIMIT 8192
/* External native data uses explicit guest-visible spans, never truncation. */
#define EXTERNAL_BASE (MEMORIES_GUEST_RAM + MEMORIES_RAM_SIZE)
#define EXTERNAL_END 0xf0000000u
/* Automatic globals/heaps are a subset of the external mapping arena. */
#define AUTOMATIC_BASE 0xd0000000u
#define AUTOMATIC_END 0xe0000000u
struct Function {
    u32 guest;
    uintptr_t host;
};
MemoriesMemory *GuestRuntime_ActiveMemory;
_Static_assert(offsetof(MemoriesMemory, ram) == 0, "IR fast resolver requires RAM first");
static GuestRuntimeRegion *regions;
static unsigned *region_order; /* region indices sorted by guest base */
static unsigned *host_order;   /* region indices sorted by native base */
static size_t region_capacity;
static u32 automatic_cursor = AUTOMATIC_BASE;
static struct Function functions[FUNCTION_LIMIT];
static unsigned function_host_order[FUNCTION_LIMIT];
static unsigned region_count, function_count;
/* Direct page candidates avoid hash collisions across model arenas. Entries
 * retain indices, not reallocatable pointers; complete spans are checked.
 * Removal invalidates candidates by epoch without clearing megabytes. */
#define REGION_PAGE_SHIFT 12
#define REGION_PAGE_COUNT ((EXTERNAL_END - EXTERNAL_BASE + 4095u) >> REGION_PAGE_SHIFT)
static struct { unsigned index, epoch; } region_pages[REGION_PAGE_COUNT];
static unsigned region_epoch = 1;
static void invalidate_region_pages(void)
{
    if (!++region_epoch) {
        memset(region_pages, 0, sizeof(region_pages));
        region_epoch = 1;
    }
}
static void *(*function_resolver)(u32);
static char fatal_detail[512];

const char *GuestRuntime_FatalDetail(void) { return fatal_detail; }

/* PS1 C often carries addresses through signed s32 locals. A subsequent
 * integer-to-pointer cast sign-extends on ARM64; preserve its guest bits.
 * Ordinary macOS user pointers cannot occupy this negative address range. */
static uintptr_t guest_bits(uintptr_t address)
{
    if (address > UINT32_MAX &&
        (address >> 32) == UINT32_MAX && (address & MEMORIES_GUEST_RAM))
        return (u32)address;
    return address;
}

/* Keep fatal diagnostics out of the per-access resolver. Inlining this
 * region summary spills SIMD registers even on valid native accesses. */
static __attribute__((cold, noinline, noreturn))
void invalid(const char *operation, uintptr_t address, size_t length)
{
    size_t bytes = 0, heap_bytes = 0;
    unsigned i;
    for (i = 0; i < region_count; ++i) {
        bytes += regions[i].length;
        if (regions[i].flags & MEMORIES_REGION_HEAP) heap_bytes += regions[i].length;
    }
    /* Prepared before abort; the signal handler only copies this text. */
    snprintf(fatal_detail, sizeof(fatal_detail),
             "%s at 0x%llx (%zu bytes); regions=%u live=%zu heap=%zu cursor=0x%x",
             operation, (unsigned long long)address, length,
             region_count, bytes, heap_bytes, automatic_cursor);
    fprintf(stderr, "translated runtime: %s\n", fatal_detail);
    abort();
}

void GuestRuntime_Reset(void)
{
    fatal_detail[0] = 0;
    GuestRuntime_ActiveMemory = NULL;
    function_resolver = NULL;
    region_count = function_count = 0;
    invalidate_region_pages();
    free(regions);
    free(region_order);
    free(host_order);
    regions = NULL;
    region_order = NULL;
    host_order = NULL;
    region_capacity = 0;
    automatic_cursor = AUTOMATIC_BASE;
}
int GuestRuntime_IsBound(void) { return GuestRuntime_ActiveMemory != NULL; }
MemoriesMemory *GuestRuntime_Memory(void) { return GuestRuntime_ActiveMemory; }
void GuestRuntime_SetFunctionResolver(void *(*resolver)(u32)) { function_resolver = resolver; }
int GuestRuntime_Bind(MemoriesMemory *memory)
{
    if (!memory) return -1;
    GuestRuntime_Reset();
    GuestRuntime_ActiveMemory = memory;
    return 0;
}
/* Function tokens use the cached RAM alias; data aliases are resolved by
 * Memories_Resolve. External native regions retain their assigned tokens. */
static u32 canonical(u32 address)
{
    if (address < MEMORIES_RAM_SIZE ||
        (address >= MEMORIES_GUEST_RAM && address - MEMORIES_GUEST_RAM < MEMORIES_RAM_SIZE) ||
        (address >= MEMORIES_GUEST_RAM_UNCACHED && address - MEMORIES_GUEST_RAM_UNCACHED < MEMORIES_RAM_SIZE))
        return MEMORIES_GUEST_RAM | (address & (MEMORIES_RAM_SIZE - 1u));
    return address;
}
int GuestRuntime_RegisterData(void *host, size_t length, u32 guest)
{
    unsigned i;
    uintptr_t start = (uintptr_t)host;
    if (!GuestRuntime_ActiveMemory || !host || !length ||
        length > UINTPTR_MAX - start || guest < EXTERNAL_BASE || guest >= EXTERNAL_END ||
        length > EXTERNAL_END - guest) return -1;
    /* Guest aliases take precedence in resolution/encoding. An external
     * mapping must not shadow them or register the memory context itself. */
    if ((guest < MEMORIES_GUEST_RAM_UNCACHED + MEMORIES_RAM_SIZE &&
         guest + length > MEMORIES_GUEST_RAM_UNCACHED) ||
        (guest < MEMORIES_GUEST_SCRATCHPAD + MEMORIES_SCRATCHPAD_SIZE &&
         guest + length > MEMORIES_GUEST_SCRATCHPAD) ||
        (start < (uintptr_t)GuestRuntime_ActiveMemory->ram + MEMORIES_RAM_SIZE &&
         start + length > (uintptr_t)GuestRuntime_ActiveMemory->ram) ||
        (start < (uintptr_t)GuestRuntime_ActiveMemory->scratchpad + MEMORIES_SCRATCHPAD_SIZE &&
         start + length > (uintptr_t)GuestRuntime_ActiveMemory->scratchpad)) return -1;
    for (i = 0; i < region_count; ++i) {
        const GuestRuntimeRegion *r = &regions[i];
        if ((start < r->host + r->length && r->host < start + length) ||
            (guest < r->guest + r->length && r->guest < guest + length)) return -1;
    }
    if (GuestRuntime_ReserveRegions((size_t)region_count + 1)) return -1;
    unsigned position = region_count;
    while (position && regions[region_order[position - 1]].guest > guest) {
        region_order[position] = region_order[position - 1];
        --position;
    }
    region_order[position] = region_count;
    position = region_count;
    while (position && regions[host_order[position - 1]].host > start) {
        host_order[position] = host_order[position - 1];
        --position;
    }
    host_order[position] = region_count;
    regions[region_count++] = (GuestRuntimeRegion){start, length, guest, 0, 0};
    return 0;
}
int GuestRuntime_ReserveRegions(size_t count)
{
    size_t capacity = region_capacity ? region_capacity : 128;
    GuestRuntimeRegion *grown;
    unsigned *order;
    if (count <= region_capacity) return 0;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(*regions)) return -1;
    while (capacity < count) {
        if (capacity > SIZE_MAX / 2) return -1;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*regions)) return -1;
    order = realloc(region_order, capacity * sizeof(*order));
    if (!order) return -1;
    region_order = order;
    order = realloc(host_order, capacity * sizeof(*order));
    if (!order) return -1;
    host_order = order;
    grown = realloc(regions, capacity * sizeof(*regions));
    if (!grown) return -1;
    regions = grown;
    region_capacity = capacity;
    return 0;
}
int GuestRuntime_RegisterFunction(u32 guest, void (*host)(void))
{
    unsigned i;
    guest = canonical(guest);
    if (!GuestRuntime_ActiveMemory || !host || (guest & 3u) || guest < 0x80010000u ||
        guest >= EXTERNAL_BASE || function_count == FUNCTION_LIMIT) return -1;
    for (i = 0; i < function_count; ++i)
        if (functions[i].guest == guest && functions[i].host == (uintptr_t)host) return -1;
    /* Equal host addresses retain registration order: aliases encode to the
     * first token, as they did with the linear lookup. */
    unsigned position = function_count;
    while (position && functions[function_host_order[position - 1]].host > (uintptr_t)host) {
        function_host_order[position] = function_host_order[position - 1];
        --position;
    }
    function_host_order[position] = function_count;
    functions[function_count++] = (struct Function){guest, (uintptr_t)host};
    return 0;
}
void GuestRuntime_RegisterAutomatic(void *host, size_t length)
{
    u32 candidate = automatic_cursor;
    unsigned i;
    int moved;
    if (!host || !length) return;
    for (i = 0; i < region_count; ++i)
        if (regions[i].host == (uintptr_t)host && regions[i].length == length) return;
    /* Try the cursor first, then the complete arena. A larger temporary
     * allocation may skip a small free span and leave the cursor above it.
     * Failing at the upper end must not hide that reusable lower span. */
    int wrapped = 0;
    do {
        moved = 0;
        if (length > AUTOMATIC_END - candidate) {
            if (wrapped) invalid("guest token arena exhausted", (uintptr_t)host, length);
            candidate = AUTOMATIC_BASE;
            wrapped = 1;
            moved = 1;
            continue;
        }
        for (i = 0; i < region_count; ++i) {
            const GuestRuntimeRegion *r = &regions[i];
            if (candidate < r->guest + r->length && r->guest < candidate + length) {
                size_t next = ((size_t)r->guest + r->length + 15u) & ~(size_t)15u;
                candidate = next >= AUTOMATIC_END ? AUTOMATIC_END : (u32)next;
                moved = 1;
                break;
            }
        }
    } while (moved);
    if (GuestRuntime_RegisterData(host, length, candidate)) invalid("cannot register native allocation", (uintptr_t)host, length);
    automatic_cursor = (u32)(((size_t)candidate + length + 15u) & ~(size_t)15u);
}
unsigned GuestRuntime_RegionCount(void) { return region_count; }
const GuestRuntimeRegion *GuestRuntime_Region(unsigned index)
{
    return index < region_count ? &regions[index] : NULL;
}
size_t GuestRuntime_AutomaticSpace(void)
{
    size_t used = 0;
    unsigned i;
    for (i = 0; i < region_count; ++i)
        if (regions[i].guest >= AUTOMATIC_BASE && regions[i].guest < AUTOMATIC_END)
            used += ((size_t)regions[i].length + 15u) & ~(size_t)15u;
    return used >= AUTOMATIC_END - AUTOMATIC_BASE ? 0 : AUTOMATIC_END - AUTOMATIC_BASE - used;
}
void GuestRuntime_RegisterGlobal(void *host, size_t length, uint64_t identity, unsigned flags)
{
    unsigned i;
    GuestRuntime_RegisterAutomatic(host, length);
    for (i = 0; i < region_count; ++i) if (regions[i].host == (uintptr_t)host) {
        if (!regions[i].identity) regions[i].identity = identity;
        regions[i].flags |= MEMORIES_REGION_GLOBAL | flags;
        return;
    }
}
void GuestRuntime_RegisterAllocation(void *host, size_t length)
{
    unsigned i;
    GuestRuntime_RegisterAutomatic(host, length);
    for (i = 0; i < region_count; ++i) if (regions[i].host == (uintptr_t)host) {
        regions[i].flags |= MEMORIES_REGION_HEAP;
        return;
    }
}
int GuestRuntime_RegisterAllocationAt(void *host, size_t length, uint32_t guest)
{
    if (GuestRuntime_RegisterData(host, length, guest)) return -1;
    regions[region_count - 1].flags = MEMORIES_REGION_HEAP;
    return 0;
}
int GuestRuntime_RegisterMapping(void *host, size_t length, uint32_t guest)
{
    if (GuestRuntime_RegisterData(host, length, guest)) return -1;
    regions[region_count - 1].flags = MEMORIES_REGION_MAPPING;
    return 0;
}
int GuestRuntime_UnregisterData(void *host)
{
    unsigned i;
    for (i = 0; i < region_count; ++i) if (regions[i].host == (uintptr_t)host) {
        if (regions[i].guest >= AUTOMATIC_BASE && regions[i].guest < automatic_cursor)
            automatic_cursor = regions[i].guest;
        regions[i] = regions[--region_count];
        /* Swap removal changes the moved entry's index in both orders. */
        unsigned *orders[] = {region_order, host_order};
        for (unsigned order = 0; order < 2; ++order) {
            unsigned position = 0;
            for (unsigned n = 0; n <= region_count; ++n) {
                unsigned index = orders[order][n];
                if (index == i) continue;
                orders[order][position++] = index == region_count ? i : index;
            }
        }
        invalidate_region_pages();
        return 0;
    }
    return -1;
}
void *GuestRuntime_ResolveData(void *pointer, size_t length)
{
    uintptr_t address = guest_bits((uintptr_t)pointer);
    unsigned i;
    void *host;
    /* Full-width host pointers already address native storage. */
    if (address > UINT32_MAX) return pointer;
    if (!GuestRuntime_ActiveMemory) invalid("memory context is unbound", address, length);
    host = Memories_ResolveSpan(GuestRuntime_ActiveMemory, (u32)address, length, 1);
    if (host) return host;
    if (address < EXTERNAL_BASE || address >= EXTERNAL_END)
        invalid("invalid guest data span", address, length);
    unsigned page = (address - EXTERNAL_BASE) >> REGION_PAGE_SHIFT;
    if (region_pages[page].epoch == region_epoch) {
        unsigned cached = region_pages[page].index;
        if (cached < region_count) {
            const GuestRuntimeRegion *r = &regions[cached];
            if (address >= r->guest) {
                size_t offset = address - r->guest;
                if (offset < r->length && length <= r->length - offset) {
                    return (void *)(r->host + offset);
                }
            }
        }
    }
    /* Small regions can share a page. A sorted fallback stays logarithmic
     * instead of scanning every registered global on a candidate miss. */
    unsigned low = 0, high = region_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (regions[region_order[middle]].guest <= address) low = middle + 1;
        else high = middle;
    }
    if (low) {
        i = region_order[low - 1];
        const GuestRuntimeRegion *r = &regions[i];
        size_t offset = address - r->guest;
        if (offset < r->length && length <= r->length - offset) {
            region_pages[page].index = i;
            region_pages[page].epoch = region_epoch;
            return (void *)(r->host + offset);
        }
    }
    invalid("invalid guest data span", address, length);
    return NULL;
}
void *GuestRuntime_ResolveFunction(void *pointer)
{
    uintptr_t address = guest_bits((uintptr_t)pointer);
    u32 guest;
    unsigned i;
    if (address > UINT32_MAX) return pointer;
    guest = canonical((u32)address);
    if (function_resolver) {
        void *target = function_resolver(guest);
        if (target) return target;
    }
    for (i = 0; i < function_count; ++i)
        if (functions[i].guest == guest) return (void *)functions[i].host;
    invalid("unknown guest function", address, 0);
    return NULL;
}
unsigned GuestRuntime_Clear16(void *address, size_t count)
{
    unsigned changed = 0;
    if (count > SIZE_MAX / sizeof(u16)) invalid("invalid clear span", (uintptr_t)address, count);
    u16 *values = GuestRuntime_ResolveData(address, count * sizeof(u16));
    for (size_t i = 0; i < count; ++i) changed += values[i] != 0;
    memset(values, 0, count * sizeof(u16));
    return changed;
}
static uint32_t *resolve_ot_word(uint32_t guest, void *context)
{
    (void)context;
    return GuestRuntime_ResolveData((void *)(uintptr_t)guest, sizeof(uint32_t));
}
void GuestRuntime_FlattenOt(void *address, unsigned count, u32 end,
                            u32 *first_out, u32 *last_out, unsigned *nearest_out)
{
    MemoriesOtChain chain;
    int result;
    if (!count || count > 0x4000u) invalid("invalid ordering table span", (uintptr_t)address, count);
    u32 base = GuestRuntime_EncodePointer(address);
    const u32 *tags = GuestRuntime_ResolveData(address, (size_t)count * sizeof(u32));
    result = Memories_FlattenOtChain(tags, base, count, end, resolve_ot_word, NULL, &chain);
    if (result) invalid(result == -2 ? "cyclic ordering table" : "invalid ordering table span", base, count);
    *(u32 *)GuestRuntime_ResolveData(first_out, sizeof(u32)) = chain.first;
    *(u32 *)GuestRuntime_ResolveData(last_out, sizeof(u32)) = chain.last;
    *(unsigned *)GuestRuntime_ResolveData(nearest_out, sizeof(unsigned)) = chain.nearest;
}
u32 GuestRuntime_EncodePointer(void *pointer)
{
    uintptr_t host = guest_bits((uintptr_t)pointer);
    uintptr_t ram, scratch;
    if (host <= UINT32_MAX) return (u32)host;
    if (!GuestRuntime_ActiveMemory) invalid("memory context is unbound", host, 0);
    ram = (uintptr_t)GuestRuntime_ActiveMemory->ram;
    scratch = (uintptr_t)GuestRuntime_ActiveMemory->scratchpad;
    /* Prefer an address inside storage to another allocation's one-past.
     * Native globals may be adjacent while guest tokens are aligned apart.
     * RAM and scratchpad are also adjacent in MemoriesMemory. */
    if (host >= ram && host - ram < MEMORIES_RAM_SIZE) return MEMORIES_GUEST_RAM + (u32)(host - ram);
    if (host >= scratch && host - scratch < MEMORIES_SCRATCHPAD_SIZE) return MEMORIES_GUEST_SCRATCHPAD_RETAIL + (u32)(host - scratch);
    /* Model transforms and packet sorting repeatedly encode native pointers.
     * Searching every function and global here makes that cost grow with the
     * whole game, even though the pointer belongs to one small model arena. */
    unsigned low = 0, high = function_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (functions[function_host_order[middle]].host < host) low = middle + 1;
        else high = middle;
    }
    if (low < function_count && functions[function_host_order[low]].host == host)
        return functions[function_host_order[low]].guest;
    low = 0; high = region_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (regions[host_order[middle]].host <= host) low = middle + 1;
        else high = middle;
    }
    if (low) {
        const GuestRuntimeRegion *r = &regions[host_order[low - 1]];
        size_t offset = host - r->host;
        if (offset < r->length) return r->guest + (u32)offset;
    }
    /* Keep one-past encodings for legal pointer arithmetic only after all
     * containing regions and function entries have been considered. */
    if (host == ram + MEMORIES_RAM_SIZE) return MEMORIES_GUEST_RAM + MEMORIES_RAM_SIZE;
    if (host == scratch + MEMORIES_SCRATCHPAD_SIZE) return MEMORIES_GUEST_SCRATCHPAD_RETAIL + MEMORIES_SCRATCHPAD_SIZE;
    if (low) {
        const GuestRuntimeRegion *r = &regions[host_order[low - 1]];
        if (host - r->host == r->length) return r->guest + (u32)r->length;
    }
    invalid("unregistered native pointer cannot fit guest storage", host, 0);
    return 0;
}
