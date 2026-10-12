#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "pc/compat/fs.h"
#include "../../types.h"
#include "image.h"
#include "mips.h"
#include "translated_image.h"
#include "translated_runtime.h"
#include "function_map.h"
#include <pthread.h>
#ifdef __SWITCH__
/* src/pc/platform/switch.c: libnx's types cannot meet the game's here. */
int Switch_ThreadStack(void **end, size_t *size);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static MemoriesMemory *guest_memory;
int Memories_ScratchpadRetailView = 1;
extern void GuestRuntime_RegisterAllGlobals(void);
void *(*Memories_GuestBranchResolver)(unsigned);

static void *resolve_function(u32 address)
{
    const MemoriesGuestFunction *entry = Memories_FindResidentFunction(address);
    if (entry) return (void *)(uintptr_t)entry->host;
    if (Memories_MipsInOverlay(address)) {
        Memories_MipsThunkTarget = address;
        return (void *)(uintptr_t)Memories_MipsThunk;
    }
    return NULL;
}
static void *resolve_branch(unsigned address)
{
    return GuestRuntime_ResolveFunction((void *)(uintptr_t)address);
}
/* The calling thread's stack: its top and size. */
static int thread_stack(void **end, size_t *size)
{
#ifdef __SWITCH__
    return Switch_ThreadStack(end, size);
#else
    *end = pthread_get_stackaddr_np(pthread_self());
    *size = pthread_get_stacksize_np(pthread_self());
#endif
    return 0;
}
const char *Memories_GuestMapError(void) { return ""; }
int Memories_GuestMap(void)
{
    unsigned i;
    void *stack_end;
    size_t stack_size;
    guest_memory = calloc(1, sizeof(*guest_memory));
    if (!guest_memory || GuestRuntime_Bind(guest_memory)) return -1;
    if (thread_stack(&stack_end, &stack_size)) return -1;
    if (!stack_end || !stack_size || GuestRuntime_RegisterData((u8 *)stack_end - stack_size,
                                                              stack_size, 0xe0000000u)) return -1;
    for (i = 0; i < Memories_FunctionMapCount; ++i) {
        const MemoriesGuestFunction *entry = &Memories_FunctionMap[i];
        unsigned previous;
        for (previous = 0; previous < i; ++previous)
            if (Memories_FunctionMap[previous].guest == entry->guest &&
                Memories_FunctionMap[previous].host == entry->host) break;
        if (previous < i) continue;
        if (GuestRuntime_RegisterFunction(entry->guest, entry->host)) {
            fprintf(stderr, "translated runtime: cannot register guest function 0x%08x\n", entry->guest);
            return -1;
        }
    }
    GuestRuntime_RegisterAllGlobals();
    GuestRuntime_SetFunctionResolver(resolve_function);
    Memories_GuestBranchResolver = resolve_branch;
    return 0;
}
int Memories_GuestLoadExeData(const unsigned char *data, size_t size, const char *name)
{
    u32 entry;
    if (Memories_LoadTranslatedExe(guest_memory, data, size, &entry)) {
        fprintf(stderr, "%s: invalid or truncated PS-X EXE\n", name);
        return -1;
    }
    printf("Native arm64: loaded PS-X EXE, guest entry 0x%08x\n", entry);
    return 0;
}
void Memories_Unimplemented(const char *name)
{
    fprintf(stderr, "Native arm64: unimplemented game routine %s; stopping\n", name);
    fflush(stderr);
    exit(70);
}
