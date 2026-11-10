#ifndef MEMORIES_TRANSLATED_RUNTIME_H
#define MEMORIES_TRANSLATED_RUNTIME_H
#include "pc/memory.h"
/* Experimental native backend helpers used by translated LLVM IR. Registering
 * data, resolving and encoding are safe from any thread (translated_runtime.c);
 * Bind, Reset, RegisterFunction and walking the regions are not. */
/* Fatal diagnostics are formatted before abort, never in a signal handler.
 * This end writes them, so the handler's translated code never reads the
 * text: "translated runtime: <detail>\n" to `fd`, and nothing when there is
 * none. Safe in a signal handler. */
void GuestRuntime_WriteFatalDetail(int fd);
int GuestRuntime_Bind(MemoriesMemory *memory);
void GuestRuntime_Reset(void);
void GuestRuntime_SetFunctionResolver(void *(*resolver)(uint32_t));
int GuestRuntime_IsBound(void);
MemoriesMemory *GuestRuntime_Memory(void);
/* Read by generated fast paths; only Bind/Reset may change this context. */
extern MemoriesMemory *GuestRuntime_ActiveMemory;
int GuestRuntime_RegisterData(void *host, size_t length, uint32_t guest);
void GuestRuntime_RegisterAutomatic(void *host, size_t length);
enum {
    MEMORIES_REGION_GLOBAL = 1, MEMORIES_REGION_GAME = 2,
    MEMORIES_REGION_CONSTANT = 4, MEMORIES_REGION_HEAP = 8, MEMORIES_REGION_MAPPING = 16
};
typedef struct GuestRuntimeRegion {
    uintptr_t host;
    size_t length;
    uint32_t guest, flags;
    uint64_t identity;
} GuestRuntimeRegion;
void GuestRuntime_RegisterGlobal(void *host, size_t length, uint64_t identity, unsigned flags);
/* Bytes of the token arena (where registered host data gets its guest
 * address) not taken yet: a code mod is refused rather than registered
 * past it, which aborts. Fragmentation may leave less in one piece. */
size_t GuestRuntime_AutomaticSpace(void);
void GuestRuntime_RegisterAllocation(void *host, size_t length);
/* As GuestRuntime_RegisterAllocation, but -1 instead of the abort when the
 * arena has no room. For GuestRuntime_TryCalloc (try_alloc.h). */
int GuestRuntime_TryRegisterAllocation(void *host, size_t length);
unsigned GuestRuntime_RegionCount(void);
const GuestRuntimeRegion *GuestRuntime_Region(unsigned index);
int GuestRuntime_RegisterAllocationAt(void *host, size_t length, uint32_t guest);
int GuestRuntime_RegisterMapping(void *host, size_t length, uint32_t guest);
int GuestRuntime_ReserveRegions(size_t count);
int GuestRuntime_UnregisterData(void *host);
/* realloc for a registered heap block: the new block is registered, the old one
 * not, and no other thread can be handed the old block before that is done. */
void *GuestRuntime_ReallocRegistered(void *host, size_t length);
int GuestRuntime_RegisterFunction(uint32_t guest, void (*host)(void));
void *GuestRuntime_ResolveData(void *address, size_t length);
/* Clear a resolved texture-map span and count entries whose value changed. */
unsigned GuestRuntime_Clear16(void *address, size_t count);
/* Flatten an OT's packet chains, preserving packet order and length bits. */
void GuestRuntime_FlattenOt(void *tags, unsigned count, uint32_t end,
                            uint32_t *first, uint32_t *last, unsigned *nearest);
void *GuestRuntime_ResolveFunction(void *address);
uint32_t GuestRuntime_EncodePointer(void *host);
/* Generated, typed dispatch for calls leaving an interpreted MIPS module. */
uint32_t GuestRuntime_InvokeNative(unsigned index, const uint32_t *arguments);
#endif
