#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS (Android, Linux) */
#endif
#include "../../types.h"
#include "translated_runtime.h"
#include "pc/memory_span.h"
#include "pc/compat/ot_chain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define FUNCTION_LIMIT 8192
/* External native data uses explicit guest-visible spans, never truncation. */
#define EXTERNAL_BASE (MEMORIES_GUEST_RAM + MEMORIES_RAM_SIZE)
#define EXTERNAL_END 0xf0000000u
/* Automatic globals/heaps are a subset of the external mapping arena, from
 * where the console's direct segments end up to the game's stacks, which have
 * fixed tokens from 0xE0000000 and register after the mods have loaded. A save
 * state holds its globals by token, so they stay where they have always been:
 * tokens start at AUTOMATIC_START, go on to the end, and only then come back to
 * the base. */
#define AUTOMATIC_BASE 0xc0000000u
#define AUTOMATIC_START 0xd0000000u
#define AUTOMATIC_END 0xe0000000u
_Static_assert(AUTOMATIC_BASE >= MEMORIES_GUEST_DIRECT_END, "the arena must not reach the direct segments");
struct Function {
    u32 guest;
    uintptr_t host;
};
MemoriesMemory *GuestRuntime_ActiveMemory;
_Static_assert(offsetof(MemoriesMemory, ram) == 0, "IR fast resolver requires RAM first");
/* The region tables are used by every thread of the game, not only the one
 * that runs it: the picture reader, Import, the HD pack, the update check all
 * allocate, and all resolve pointers.
 *
 * Writers take registry_lock. The tables are one mapping that never moves, so a
 * reader on another thread can never be left holding freed memory; only the
 * pages in use take real memory.
 *
 * Readers take no lock. A region keeps its slot in `regions` for as long as it
 * lives, and a slot is only used again long after it was freed (FREE_KEPT), so
 * the page cache in GuestRuntime_ResolveData reads a slot without any barrier:
 * for a live pointer, nothing writes to the slot it finds. The two sorted orders
 * do move, so what searches them (a cache miss, GuestRuntime_EncodePointer) goes
 * by registry_sequence, odd during a change, and looks again if it changed. */
#define REGION_LIMIT (1u << 22)
#define FREE_KEPT 4096u
static GuestRuntimeRegion *regions;
static unsigned *region_order; /* live slots sorted by guest base */
static unsigned *host_order;   /* live slots sorted by native base */
static unsigned *free_slots;   /* a ring: oldest first */
static unsigned free_first, free_count;
static void *region_tables;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
/* Each on a line of its own: the hit path reads slot_count and region_epoch all
 * the time, and writers change the others. */
static _Alignas(64) unsigned registry_sequence;
static _Thread_local int changing; /* this thread is between change_begin and change_end */
static _Alignas(64) u32 automatic_cursor = AUTOMATIC_START;
static struct Function functions[FUNCTION_LIMIT];
static unsigned function_host_order[FUNCTION_LIMIT];
static _Alignas(64) unsigned region_count; /* live: the length of the two orders */
static _Alignas(64) unsigned slot_count;   /* slots of `regions` ever used */
static unsigned function_count;
/* Direct page candidates avoid hash collisions across model arenas. Entries
 * retain slots, which do not move; complete spans are checked.
 * Removal invalidates candidates by epoch without clearing megabytes. */
#define REGION_PAGE_SHIFT 12
#define REGION_PAGE_COUNT ((EXTERNAL_END - EXTERNAL_BASE + 4095u) >> REGION_PAGE_SHIFT)
static struct { unsigned index, epoch; } region_pages[REGION_PAGE_COUNT];
static _Alignas(64) unsigned region_epoch = 1;
static void invalidate_region_pages(void)
{
    if (!++region_epoch) {
        memset(region_pages, 0, sizeof(region_pages));
        region_epoch = 1;
    }
}
static int reserve_regions(size_t count);
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
        bytes += regions[region_order[i]].length;
        if (regions[region_order[i]].flags & MEMORIES_REGION_HEAP) heap_bytes += regions[region_order[i]].length;
    }
    /* Prepared before abort; the signal handler only copies this text. */
    snprintf(fatal_detail, sizeof(fatal_detail),
             "%s at 0x%llx (%zu bytes); regions=%u live=%zu heap=%zu cursor=0x%x",
             operation, (unsigned long long)address, length,
             region_count, bytes, heap_bytes, automatic_cursor);
    fprintf(stderr, "translated runtime: %s\n", fatal_detail);
    abort();
}

/* Writers hold registry_lock. A reader waits out a change, for as long as it
 * takes: the writer may have lost its core. Only a signal handler on the
 * writer's own thread (the clock's tick runs game code, with MEMORIES_CLOCK=
 * interrupt or MEMORIES_PROFILE) cannot, and has the slots, always whole. */
static void change_begin(void)
{
    changing = 1;
    __atomic_store_n(&registry_sequence, registry_sequence + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
static void change_end(void)
{
    __atomic_store_n(&registry_sequence, registry_sequence + 1, __ATOMIC_RELEASE);
    changing = 0;
}
static unsigned read_begin(void)
{
    for (unsigned tries = 0;; ++tries) {
        unsigned sequence = __atomic_load_n(&registry_sequence, __ATOMIC_ACQUIRE);
        if (!(sequence & 1)) return sequence;
        if (tries > 256) sched_yield();
    }
}
static int read_changed(unsigned sequence)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&registry_sequence, __ATOMIC_RELAXED) != sequence;
}

static size_t tables_size(void)
{
    return (size_t)REGION_LIMIT * (sizeof(*regions) + 3 * sizeof(*region_order));
}
static int map_tables(void)
{
    char *at;
    if (region_tables) return 0;
#ifdef _WIN32
    at = VirtualAlloc(NULL, tables_size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!at) return -1;
#else
    at = mmap(NULL, tables_size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (at == MAP_FAILED) return -1;
#endif
    regions = (GuestRuntimeRegion *)at;
    at += (size_t)REGION_LIMIT * sizeof(*regions);
    region_order = (unsigned *)at;
    host_order = region_order + REGION_LIMIT;
    free_slots = host_order + REGION_LIMIT;
    region_tables = regions;
    return 0;
}
static void unmap_tables(void)
{
    if (!region_tables) return;
#ifdef _WIN32
    VirtualFree(region_tables, 0, MEM_RELEASE);
#else
    munmap(region_tables, tables_size());
#endif
    region_tables = NULL;
    regions = NULL;
    region_order = host_order = free_slots = NULL;
}

void GuestRuntime_Reset(void)
{
    fatal_detail[0] = 0;
    GuestRuntime_ActiveMemory = NULL;
    function_resolver = NULL;
    region_count = slot_count = function_count = 0;
    free_first = free_count = 0;
    invalidate_region_pages();
    unmap_tables();
    automatic_cursor = AUTOMATIC_START;
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
/* The first place in `order` whose region's `key` (guest or host base) is not
 * below `value`. */
static unsigned guest_place(const unsigned *order, unsigned count, u32 value)
{
    unsigned low = 0, high = count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (regions[order[middle]].guest < value) low = middle + 1;
        else high = middle;
    }
    return low;
}
static unsigned host_place(const unsigned *order, unsigned count, uintptr_t value)
{
    unsigned low = 0, high = count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (regions[order[middle]].host < value) low = middle + 1;
        else high = middle;
    }
    return low;
}
/* The slot, or -1. */
static int register_data(void *host, size_t length, u32 guest)
{
    unsigned slot, position;
    uintptr_t start = (uintptr_t)host;
    GuestRuntimeRegion *fresh;
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
    /* Regions do not overlap, so only the ones either side can. */
    if (reserve_regions((size_t)region_count + 1)) return -1;
    position = host_place(host_order, region_count, start);
    if (position < region_count && regions[host_order[position]].host < start + length) return -1;
    if (position && regions[host_order[position - 1]].host + regions[host_order[position - 1]].length > start) return -1;
    position = guest_place(region_order, region_count, guest);
    if (position < region_count && regions[region_order[position]].guest < (size_t)guest + length) return -1;
    if (position && (size_t)regions[region_order[position - 1]].guest + regions[region_order[position - 1]].length > guest)
        return -1;
    if (free_count > FREE_KEPT) {
        slot = free_slots[free_first];
        free_first = (free_first + 1) % REGION_LIMIT;
        --free_count;
    } else {
        if (slot_count >= REGION_LIMIT) return -1;
        slot = slot_count;
    }
    /* Everything but the length first: until that is there, no span fits. */
    fresh = &regions[slot];
    fresh->host = start;
    fresh->guest = guest;
    fresh->flags = 0;
    fresh->identity = 0;
    __atomic_store_n(&fresh->length, length, __ATOMIC_RELEASE);
    change_begin();
    position = guest_place(region_order, region_count, guest);
    memmove(region_order + position + 1, region_order + position, (region_count - position) * sizeof(*region_order));
    region_order[position] = slot;
    position = host_place(host_order, region_count, start);
    memmove(host_order + position + 1, host_order + position, (region_count - position) * sizeof(*host_order));
    host_order[position] = slot;
    if (slot == slot_count) ++slot_count;
    ++region_count;
    change_end();
    return (int)slot;
}
int GuestRuntime_RegisterData(void *host, size_t length, u32 guest)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_data(host, length, guest);
    pthread_mutex_unlock(&registry_lock);
    return slot < 0 ? -1 : 0;
}
static int reserve_regions(size_t count)
{
    return count > REGION_LIMIT || map_tables() ? -1 : 0;
}
int GuestRuntime_ReserveRegions(size_t count)
{
    int result;
    pthread_mutex_lock(&registry_lock);
    result = reserve_regions(count);
    pthread_mutex_unlock(&registry_lock);
    return result;
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
/* The first entry of region_order that ends past `address`. Regions do not
 * overlap, so their ends are in the same order as their bases. */
static unsigned first_region_past(u32 address)
{
    unsigned low = 0, high = region_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        const GuestRuntimeRegion *r = &regions[region_order[middle]];
        if ((size_t)r->guest + r->length <= address) low = middle + 1;
        else high = middle;
    }
    return low;
}
/* The slot; -1 with the arena full when the caller can do without (`fatal` 0),
 * -2 with nothing to register. */
static int register_automatic(void *host, size_t length, int fatal)
{
    u32 candidate = automatic_cursor;
    unsigned i, position;
    int slot;
    if (!host || !length) return -2;
    if (region_tables) {
        i = host_place(host_order, region_count, (uintptr_t)host);
        if (i < region_count && regions[host_order[i]].host == (uintptr_t)host && regions[host_order[i]].length == length)
            return (int)host_order[i];
    }
    /* Try the cursor first, then the complete arena. A larger temporary
     * allocation may skip a small free span and leave the cursor above it.
     * Failing at the upper end must not hide that reusable lower span.
     * The walk goes up the regions in address order, each looked at once: a
     * big allocation may have to step over all of them. */
    int wrapped = 0;
    position = first_region_past(candidate);
    for (;;) {
        const GuestRuntimeRegion *r;
        size_t next;
        if (length > AUTOMATIC_END - candidate) {
            if (wrapped) {
                if (!fatal) return -1;
                invalid("guest token arena exhausted", (uintptr_t)host, length);
            }
            candidate = AUTOMATIC_BASE;
            wrapped = 1;
            position = first_region_past(candidate);
            continue;
        }
        /* Rounding up to 16 can pass a short region whole. */
        while (position < region_count) {
            r = &regions[region_order[position]];
            if ((size_t)r->guest + r->length > candidate) break;
            ++position;
        }
        if (position == region_count) break;
        r = &regions[region_order[position]];
        if (r->guest >= (size_t)candidate + length) break;
        next = ((size_t)r->guest + r->length + 15u) & ~(size_t)15u;
        candidate = next >= AUTOMATIC_END ? AUTOMATIC_END : (u32)next;
        ++position;
    }
    slot = register_data(host, length, candidate);
    if (slot < 0) invalid("cannot register native allocation", (uintptr_t)host, length);
    automatic_cursor = (u32)(((size_t)candidate + length + 15u) & ~(size_t)15u);
    return slot;
}
void GuestRuntime_RegisterAutomatic(void *host, size_t length)
{
    pthread_mutex_lock(&registry_lock);
    register_automatic(host, length, 1);
    pthread_mutex_unlock(&registry_lock);
}
/* In address order. */
unsigned GuestRuntime_RegionCount(void) { return region_count; }
const GuestRuntimeRegion *GuestRuntime_Region(unsigned index)
{
    return index < region_count ? &regions[region_order[index]] : NULL;
}
size_t GuestRuntime_AutomaticSpace(void)
{
    size_t used = 0;
    unsigned i;
    pthread_mutex_lock(&registry_lock);
    for (i = 0; i < region_count; ++i) {
        const GuestRuntimeRegion *r = &regions[region_order[i]];
        if (r->guest >= AUTOMATIC_BASE && r->guest < AUTOMATIC_END)
            used += ((size_t)r->length + 15u) & ~(size_t)15u;
    }
    pthread_mutex_unlock(&registry_lock);
    return used >= AUTOMATIC_END - AUTOMATIC_BASE ? 0 : AUTOMATIC_END - AUTOMATIC_BASE - used;
}
void GuestRuntime_RegisterGlobal(void *host, size_t length, uint64_t identity, unsigned flags)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_automatic(host, length, 1);
    if (slot >= 0) {
        if (!regions[slot].identity) regions[slot].identity = identity;
        regions[slot].flags |= MEMORIES_REGION_GLOBAL | flags;
    }
    pthread_mutex_unlock(&registry_lock);
}
void GuestRuntime_RegisterAllocation(void *host, size_t length)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_automatic(host, length, 1);
    if (slot >= 0) regions[slot].flags |= MEMORIES_REGION_HEAP;
    pthread_mutex_unlock(&registry_lock);
}
int GuestRuntime_TryRegisterAllocation(void *host, size_t length)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_automatic(host, length, 0);
    if (slot >= 0) regions[slot].flags |= MEMORIES_REGION_HEAP;
    pthread_mutex_unlock(&registry_lock);
    return slot == -1 ? -1 : 0;
}
int GuestRuntime_RegisterAllocationAt(void *host, size_t length, uint32_t guest)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_data(host, length, guest);
    if (slot >= 0) regions[slot].flags = MEMORIES_REGION_HEAP;
    pthread_mutex_unlock(&registry_lock);
    return slot < 0 ? -1 : 0;
}
int GuestRuntime_RegisterMapping(void *host, size_t length, uint32_t guest)
{
    int slot;
    pthread_mutex_lock(&registry_lock);
    slot = register_data(host, length, guest);
    if (slot >= 0) regions[slot].flags = MEMORIES_REGION_MAPPING;
    pthread_mutex_unlock(&registry_lock);
    return slot < 0 ? -1 : 0;
}
static int unregister_data(void *host)
{
    unsigned position, slot;
    u32 guest;
    if (!region_count) return -1;
    position = host_place(host_order, region_count, (uintptr_t)host);
    if (position == region_count || regions[host_order[position]].host != (uintptr_t)host) return -1;
    slot = host_order[position];
    guest = regions[slot].guest;
    if (guest >= AUTOMATIC_BASE && guest < automatic_cursor) automatic_cursor = guest;
    change_begin();
    memmove(host_order + position, host_order + position + 1, (region_count - position - 1) * sizeof(*host_order));
    position = guest_place(region_order, region_count, guest);
    memmove(region_order + position, region_order + position + 1, (region_count - position - 1) * sizeof(*region_order));
    --region_count;
    invalidate_region_pages();
    __atomic_store_n(&regions[slot].length, 0, __ATOMIC_RELEASE);
    regions[slot].host = 0;
    regions[slot].guest = 0;
    regions[slot].flags = 0;
    regions[slot].identity = 0;
    free_slots[(free_first + free_count) % REGION_LIMIT] = slot;
    ++free_count;
    change_end();
    return 0;
}
int GuestRuntime_UnregisterData(void *host)
{
    int result;
    pthread_mutex_lock(&registry_lock);
    result = unregister_data(host);
    pthread_mutex_unlock(&registry_lock);
    return result;
}
void *GuestRuntime_ReallocRegistered(void *host, size_t length)
{
    void *moved;
    if (!GuestRuntime_IsBound()) return realloc(host, length);
    /* Held across realloc: the moment it frees the old block another thread can
     * be handed it, and the old region must be gone before that one registers. */
    pthread_mutex_lock(&registry_lock);
    moved = realloc(host, length);
    if (moved) {
        int slot;
        unregister_data(host);
        slot = register_automatic(moved, length, 1);
        if (slot >= 0) regions[slot].flags |= MEMORIES_REGION_HEAP;
    }
    pthread_mutex_unlock(&registry_lock);
    return moved;
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
        if (cached < __atomic_load_n(&slot_count, __ATOMIC_RELAXED)) {
            const GuestRuntimeRegion *r = &regions[cached];
            u32 base = __atomic_load_n(&r->guest, __ATOMIC_RELAXED);
            if (address >= base) {
                size_t offset = address - base, span = __atomic_load_n(&r->length, __ATOMIC_RELAXED);
                if (offset < span && length <= span - offset)
                    return (void *)(__atomic_load_n(&r->host, __ATOMIC_RELAXED) + offset);
            }
        }
    }
    if (changing) {
        for (i = 0; i < slot_count; ++i) {
            const GuestRuntimeRegion *r = &regions[i];
            if (address >= r->guest && address - r->guest < r->length && length <= r->length - (address - r->guest))
                return (void *)(r->host + (address - r->guest));
        }
        invalid("invalid guest data span", address, length);
    }
    /* Small regions can share a page. A sorted fallback stays logarithmic
     * instead of scanning every registered global on a candidate miss. */
    for (;;) {
        unsigned sequence = read_begin(), low = 0, high = __atomic_load_n(&region_count, __ATOMIC_ACQUIRE);
        uintptr_t found_host = 0;
        size_t found_length = 0;
        u32 found_guest = 0;
        while (low < high) {
            unsigned middle = low + (high - low) / 2;
            if (regions[region_order[middle]].guest <= address) low = middle + 1;
            else high = middle;
        }
        i = 0;
        if (low) {
            i = region_order[low - 1];
            found_host = regions[i].host;
            found_length = regions[i].length;
            found_guest = regions[i].guest;
        }
        if (read_changed(sequence)) continue;
        if (low) {
            size_t offset = address - found_guest;
            if (offset < found_length && length <= found_length - offset) {
                region_pages[page].index = i;
                region_pages[page].epoch = region_epoch;
                return (void *)(found_host + offset);
            }
        }
        break;
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
    uintptr_t ram, scratch, found_host, found_length;
    u32 found_guest;
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
    if (changing) {
        for (unsigned slot = 0; slot < slot_count; ++slot) {
            const GuestRuntimeRegion *r = &regions[slot];
            if (host >= r->host && host - r->host < r->length) return r->guest + (u32)(host - r->host);
        }
    }
    for (;;) {
        unsigned sequence = changing ? 0 : read_begin();
        found_host = found_length = found_guest = 0;
        low = 0;
        high = __atomic_load_n(&region_count, __ATOMIC_ACQUIRE);
        while (low < high) {
            unsigned middle = low + (high - low) / 2;
            if (regions[host_order[middle]].host <= host) low = middle + 1;
            else high = middle;
        }
        if (low) {
            const GuestRuntimeRegion *r = &regions[host_order[low - 1]];
            found_host = r->host;
            found_length = r->length;
            found_guest = r->guest;
        }
        if (changing || !read_changed(sequence)) break;
    }
    if (low && host - found_host < found_length) return found_guest + (u32)(host - found_host);
    /* Keep one-past encodings for legal pointer arithmetic only after all
     * containing regions and function entries have been considered. */
    if (host == ram + MEMORIES_RAM_SIZE) return MEMORIES_GUEST_RAM + MEMORIES_RAM_SIZE;
    if (host == scratch + MEMORIES_SCRATCHPAD_SIZE) return MEMORIES_GUEST_SCRATCHPAD_RETAIL + MEMORIES_SCRATCHPAD_SIZE;
    if (low && host - found_host == found_length) return found_guest + (u32)found_length;
    invalid("unregistered native pointer cannot fit guest storage", host, 0);
    return 0;
}
