#define _DARWIN_C_SOURCE
#define MEMORIES_TRANSLATED_MMAN_IMPLEMENTATION
#include "pc/compat/fs.h"
#include "translated_state_backend.h"
#include "state_arm64.h"
#include "state_io.h"
#include "state_requests.h"
#include "state_subsystems.h"
#include "translated_state_memory.h"
#include "translated_runtime.h"
#include "pc/audio/spu.h"
#include "pc/cards/pack_shop.h"
#include "pc/saves/deck_menu.h"
#include "pc/platform/paths.h"
#include "pc/platform/menu.h"
#include "pc/debug/crash.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STACK_SIZE 0x00800000u
#define STACK_TOKEN 0xe1000000u
/* ARM64 has a different stack/register ABI from the existing v3 i386 file. */
#define STATE_VERSION 0x00040040u
typedef struct NativeEntry {
    MemoriesArm64Context context;
    uint64_t image_base, image_size, stack_base, stack_size;
    uint8_t uuid[16];
} NativeEntry;

MemoriesArm64Context Memories_Arm64Boundary;
static MemoriesArm64Context service;
static NativeEntry saved_entry;
static uint8_t *game_stack, *stack_mapping;
static size_t mapping_size;
static int (*game_entry)(void), game_result;
static MemoriesStateRequests requests = MEMORIES_STATE_REQUESTS_INIT;
static int last_slot;
static uint8_t *pending_image;
static size_t pending_size;
static MemoriesNativeMemoryState *pending_memory;
static uintptr_t restore_image_base;
static size_t restore_image_size;

void Memories_NativeStateRelocateField(const char *tag, void *data, size_t size)
{
    if (!pending_memory || !strcmp(tag, "soft_gpu") || !strcmp(tag, "gte") ||
        !strcmp(tag, "arm64-jump-floats")) return;
    Memories_NativeMemoryRelocate(pending_memory, data, size, saved_entry.image_base,
                                 restore_image_base, restore_image_size);
}

#ifdef __SWITCH__
/* The image runs from crt0's entry (switch.ld's __start__ is absolute: 0,
 * wherever the loader put the image) to __end__; the GNU build ID (linked
 * with --build-id, tools/pc/switch_deps.py) names the build as the Mach-O
 * UUID does. Its note is in the image's read-only data. */
extern const unsigned char _start[], __rodata_start[], __end__[];
static int executable(NativeEntry *entry)
{
    const unsigned char *at;
    entry->image_base = (uintptr_t)_start;
    entry->image_size = (uintptr_t)__end__ - (uintptr_t)_start;
    for (at = __rodata_start; at + 16 + 20 <= __end__; at += 4) {
        uint32_t name_size, data_size, type;
        memcpy(&name_size, at, 4);
        memcpy(&data_size, at + 4, 4);
        memcpy(&type, at + 8, 4);
        if (name_size == 4 && data_size >= 16 && data_size <= 64 && type == 3 /* NT_GNU_BUILD_ID */ &&
            !memcmp(at + 12, "GNU", 4)) {
            memcpy(entry->uuid, at + 16, sizeof(entry->uuid));
            return 0;
        }
    }
    return -1;
}
#else
static int executable(NativeEntry *entry)
{
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const uint8_t *cursor;
    uint64_t end = 0;
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    int uuid = 0;
    if (!header || header->magic != MH_MAGIC_64) return -1;
    entry->image_base = (uintptr_t)header;
    cursor = (const uint8_t *)(header + 1);
    for (unsigned i = 0; i < header->ncmds; ++i) {
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
            if (segment->vmaddr && segment->vmsize && segment->vmaddr + slide >= entry->image_base &&
                segment->vmaddr + slide + segment->vmsize > end) end = segment->vmaddr + slide + segment->vmsize;
        } else if (command->cmd == LC_UUID) {
            memcpy(entry->uuid, ((const struct uuid_command *)command)->uuid, sizeof(entry->uuid));
            uuid = 1;
        }
        cursor += command->cmdsize;
    }
    if (!uuid || end <= entry->image_base) return -1;
    entry->image_size = end - entry->image_base;
    return 0;
}
#endif
static void hold(int enabled)
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGALRM);
    sigprocmask(enabled ? SIG_BLOCK : SIG_UNBLOCK, &signals, NULL);
    Spu_Hold(enabled);
}
static int boundary(void)
{
    return game_stack && Memories_Arm64Boundary.sp >= (uintptr_t)game_stack &&
           Memories_Arm64Boundary.sp < (uintptr_t)game_stack + STACK_SIZE && !(Memories_Arm64Boundary.sp & 15);
}
static void run_game(void)
{
    game_result = game_entry();
    Memories_Arm64Restore(&service, 1);
}
static void apply(void)
{
    MemoriesState state = {1, NULL, pending_image, pending_size};
    NativeEntry now = {0};
    size_t length;
    const uint8_t *chunk;
    uintptr_t old_image = saved_entry.image_base;
    struct timespec started, done;
    clock_gettime(CLOCK_MONOTONIC, &started);
    executable(&now);
    restore_image_base = now.image_base;
    restore_image_size = now.image_size;
    hold(1);
    if (Memories_NativeMemoryApply(pending_memory)) {
        fprintf(stderr, "memories-pc: cannot recreate ARM64 state allocations\n");
        abort();
    }
    /* Packed subsystem chunks concatenate fields without padding. Relocate
     * each field through StateChunk, so a callback after a byte-sized field
     * is still a full pointer and adjacent counters never straddle fields. */
    chunk = Memories_StateFindChunk(&state, "arm64-stack", &length);
    Memories_NativeMemoryRelocate(pending_memory, (void *)chunk, length,
                                 old_image, now.image_base, now.image_size);
    Memories_NativeMemoryRestorePayloads(pending_memory, old_image, now.image_base, now.image_size);
    /* These serializers restore host-owned shop state and remap its text
     * before the saved PS1 memory is installed, as in the fixed backend. */
    DeckMenu_ShopState(&state);
    PackShop_State(&state);
    chunk = Memories_StateFindChunk(&state, "memory", &length);
    memcpy(GuestRuntime_Memory(), chunk, length);
    Memories_StateSubsystems(&state);
    /* A subsystem may recreate a guest-visible arena and remap saved
     * references. Apply its updated PS1 image after those serializers. */
    chunk = Memories_StateFindChunk(&state, "memory", &length);
    memcpy(GuestRuntime_Memory(), chunk, length);
    Memories_NativeMemoryRelocate(pending_memory, &saved_entry.context, 13 * sizeof(uint64_t),
                                 old_image, now.image_base, now.image_size);
    chunk = Memories_StateFindChunk(&state, "arm64-stack", &length);
    memcpy((void *)(uintptr_t)saved_entry.context.sp, chunk, length);
    Memories_NativeMemoryFree(pending_memory); pending_memory = NULL;
    free(pending_image); pending_image = NULL; pending_size = 0;
    /* Complete the saved VSync boundary exactly as the fixed backend does;
     * otherwise the CPU sees a stale last_vsync after restoring its turn. */
    extern int LibEtc_StateResumed(void);
    const int value = LibEtc_StateResumed();
    clock_gettime(CLOCK_MONOTONIC, &done);
    fprintf(stderr, "memories-pc: ARM64 state loaded in %ld ms\n",
            (long)((done.tv_sec - started.tv_sec) * 1000 + (done.tv_nsec - started.tv_nsec) / 1000000));
    hold(0);
    Memories_Arm64Restore(&saved_entry.context, value);
}
int Memories_NativeStateStackRange(uintptr_t *low, uintptr_t *high)
{
    if (!game_stack) return 0;
    *low = (uintptr_t)game_stack;
    *high = *low + STACK_SIZE;
    return 1;
}

int Memories_NativeStateRunGame(int (*entry)(void))
{
    MemoriesArm64Context initial = {0};
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int resumed;
    if (!entry || !GuestRuntime_IsBound()) { errno = EINVAL; return 1; }
    mapping_size = STACK_SIZE + 2 * page;
    stack_mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_mapping == MAP_FAILED) return 1;
    game_stack = stack_mapping + page;
    if (mprotect(stack_mapping, page, PROT_NONE) || mprotect(game_stack + STACK_SIZE, page, PROT_NONE) ||
        GuestRuntime_RegisterData(game_stack, STACK_SIZE, STACK_TOKEN)) {
        munmap(stack_mapping, mapping_size); game_stack = NULL; return 1;
    }
    game_entry = entry;
    initial.sp = ((uintptr_t)game_stack + STACK_SIZE - 16) & ~(uintptr_t)15;
    initial.x19_x30[11] = (uintptr_t)run_game;
    resumed = Memories_Arm64Capture(&service);
    if (!resumed) Memories_Arm64Restore(&initial, 1);
    if (resumed == 2) apply();
    GuestRuntime_UnregisterData(game_stack);
    munmap(stack_mapping, mapping_size);
    game_stack = NULL;
    return game_result;
}
void Memories_NativeStateRequest(int what, int slot)
{
    Memories_StateQueue(&requests, what, slot);
}
int Memories_NativeStateSave(const char *path)
{
    NativeEntry entry = {0};
    uint32_t header[2] = {STATE_VERSION, Memories_StateBuildId()};
    char partial[1024];
    MemoriesState state = {0, NULL, NULL, 0};
    MemoriesStateField field;
    int failed, error = 0, length;
    if (!boundary()) { errno = EINVAL; return -2; }
    if (!path) { errno = EINVAL; return -1; }
    length = snprintf(partial, sizeof(partial), "%s.partial", path);
    if (length < 0 || (size_t)length >= sizeof(partial)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (executable(&entry)) { errno = EINVAL; return -1; }
    entry.context = Memories_Arm64Boundary;
    entry.stack_base = (uintptr_t)game_stack;
    entry.stack_size = STACK_SIZE;
    state.file = fopen(partial, "w+b");
    if (!state.file) return -1;
    hold(1);
    Memories_StateWrite(&state, "YFMSTATE", 8);
    Memories_StateWrite(&state, header, sizeof(header));
    field = (MemoriesStateField){&entry, sizeof(entry)};
    Memories_StateChunk(&state, "arm64-entry", &field, 1);
    field = (MemoriesStateField){GuestRuntime_Memory(), sizeof(MemoriesMemory)};
    Memories_StateChunk(&state, "memory", &field, 1);
    failed = Memories_NativeMemorySave(&state);
    if (failed) error = errno ? errno : EIO;
    Memories_StateSubsystems(&state);
    field = (MemoriesStateField){(void *)(uintptr_t)entry.context.sp,
                                (uintptr_t)game_stack + STACK_SIZE - entry.context.sp};
    Memories_StateChunk(&state, "arm64-stack", &field, 1);
    if (Memories_StateSeal(&state) && !failed) {
        failed = -1;
        error = errno ? errno : EIO;
    }
    if (ferror(state.file) && !failed) {
        failed = -1;
        error = errno ? errno : EIO;
    }
    hold(0);
    if (fclose(state.file) && !failed) {
        failed = -1;
        error = errno ? errno : EIO;
    }
    if (!failed && rename(partial, path)) {
        failed = -1;
        error = errno;
    }
    if (failed) {
        remove(partial);
        errno = error;
    } else fprintf(stderr, "memories-pc: ARM64 state saved: %s\n", path);
    return failed ? -1 : 0;
}
int Memories_NativeStateLoad(const char *path, char *why, size_t size)
{
    FILE *file;
    long length;
    uint8_t *image = NULL;
    uint32_t header[2];
    NativeEntry now = {0};
    MemoriesState state;
    size_t have, at;
    const uint8_t *chunk;
    const char *reason = "invalid or unreadable ARM64 save state";
    if (!boundary()) return -2;
    file = path ? fopen(path, "rb") : NULL;
    if (!file) goto refused;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 16 || fseek(file, 0, SEEK_SET)) {
        fclose(file); goto refused;
    }
    image = malloc((size_t)length);
    if (!image) { fclose(file); goto refused; }
    if (fread(image, 1, (size_t)length, file) != (size_t)length) { fclose(file); goto refused; }
    fclose(file);
    if (memcmp(image, "YFMSTATE", 8)) goto refused;
    memcpy(header, image + 8, sizeof(header));
    if (header[0] != STATE_VERSION) { reason = "this state uses another architecture or state format"; goto refused; }
    if (header[1] != Memories_StateBuildId()) { reason = "this state was made with a different ARM64 build"; goto refused; }
    state = (MemoriesState){1, NULL, image, (size_t)length};
    if (!Memories_StateIntegrity(&state)) { reason = "the ARM64 state is truncated or its integrity check failed"; goto refused; }
    for (at = 16; at < state.image_size;) {
        uint32_t bytes;
        if (state.image_size - at < 20) goto refused;
        memcpy(&bytes, image + at + 16, sizeof(bytes));
        if (bytes > state.image_size - at - 20) goto refused;
        at += 20 + bytes;
    }
    chunk = Memories_StateFindChunk(&state, "arm64-entry", &have);
    if (!chunk || have != sizeof(saved_entry)) goto refused;
    memcpy(&saved_entry, chunk, have);
    if (executable(&now) || memcmp(now.uuid, saved_entry.uuid, sizeof(now.uuid)) || now.image_size != saved_entry.image_size) {
        reason = "this state was made with a different ARM64 executable"; goto refused;
    }
    if (saved_entry.stack_size != STACK_SIZE || saved_entry.context.sp < saved_entry.stack_base ||
        saved_entry.context.sp - saved_entry.stack_base >= STACK_SIZE || (saved_entry.context.sp & 15)) goto refused;
    chunk = Memories_StateFindChunk(&state, "arm64-stack", &have);
    if (!chunk || have != STACK_SIZE - (saved_entry.context.sp - saved_entry.stack_base)) goto refused;
    chunk = Memories_StateFindChunk(&state, "memory", &have);
    if (!chunk || have != sizeof(MemoriesMemory)) goto refused;
    if (!Memories_StateCompatibleMods(&state)) { reason = "this state has different mods"; goto refused; }
    if (!Memories_StateCompatibleLanguage(&state, path, why, size)) { free(image); return -1; }
    pending_memory = Memories_NativeMemoryPrepare(&state, why, size);
    if (!pending_memory) { free(image); return -1; }
    pending_image = image;
    pending_size = state.image_size;
    last_slot = Memories_StateSlotFromPath(path);
    fprintf(stderr, "memories-pc: ARM64 state loading: %s\n", path);
    Memories_Arm64Restore(&service, 2);
refused:
    free(image);
    if (why && size) snprintf(why, size, "%s", reason);
    return -1;
}
static int save_requested(const char *path, int tell)
{
    Paths_WriteBegin();
    if (Memories_NativeStateSave(path)) return Memories_StateSaveFailure(path, tell);
    Paths_WriteDone(path);
    return 0;
}
static void load_requested(const char *path, int slot)
{
    char why[900] = "not at a game state boundary";
    static const char *const ok[] = {"OK"};
    (void)slot; /* The native loader records the slot before switching stacks. */
    if (Memories_NativeStateLoad(path, why, sizeof(why))) {
        fprintf(stderr, "memories-pc: ARM64 state: %s\n", why);
        Menu_ShowNotice("Save state not loaded", why, ok, 1, 0, NULL);
    }
}
void Memories_NativeStatePoint(unsigned frames)
{
    static const MemoriesStateActions actions = {save_requested, load_requested};
    if (boundary()) Memories_StateProcess(&requests, frames, &actions);
}
int Memories_NativeStateStartupDone(void) { return requests.startup_done; }
int Memories_NativeStateLastSlot(void) { return last_slot; }
void Memories_NativeStateRemapRange(MemoriesState *state, uint32_t from, uint32_t to, uint32_t size)
{
    static const char *const tags[] = {"memory", "arm64-stack"};
    if (!state || !state->loading || !size) return;
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        size_t length;
        uint8_t *chunk = (uint8_t *)Memories_StateFindChunk(state, tags[i], &length);
        if (!chunk) continue;
        for (size_t at = 0; at + sizeof(uint32_t) <= length; at += sizeof(uint32_t)) {
            uint32_t word;
            memcpy(&word, chunk + at, sizeof(word));
            if (word >= from && word - from <= size) {
                word = to + (word - from);
                memcpy(chunk + at, &word, sizeof(word));
            }
        }
    }
    for (unsigned i = 0; i < 12; ++i) {
        uint64_t word = saved_entry.context.x19_x30[i];
        if ((word >> 32) != 0 && (word >> 32) != UINT32_MAX) continue;
        if ((uint32_t)word >= from && (uint32_t)word - from <= size)
            saved_entry.context.x19_x30[i] = (word & 0xffffffff00000000ull) | (to + ((uint32_t)word - from));
    }
    Memories_NativeMemoryRemapGuests(pending_memory, from, to, size);
}
