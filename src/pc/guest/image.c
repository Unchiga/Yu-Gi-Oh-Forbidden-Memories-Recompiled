#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "image.h"
#include "image_loader.h"
#include "function_map.h"
#include "low_memory.h"
#include "mips.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/debug/profile.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#include <windows.h>
#else
#include "pc/compat/mman.h" /* <sys/mman.h> and Memories_MapFixed */
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <ucontext.h>
#endif

#if defined(_WIN32) && defined(__x86_64__)
/* The 64-bit Windows build keeps the same fixed addresses; the game's stored
 * pointers are 4 bytes wide through G32 (src/port_ptr.h), and the image is
 * linked below 4 GB (tools/pc/build_game32.py --target windows-x64). */
_Static_assert(sizeof(void *) == 8, "the 64-bit Windows guest image model is LLP64");
/* The faulting instruction, as the messages below name it. */
#define FAULT_PC "rip"
#elif defined(__aarch64__)
/* The Android arm64 build, the same way: G32 pointers, and the game's
 * library linked below 4 GB (--target android-arm64-v8a). */
_Static_assert(sizeof(void *) == 8, "the arm64 guest image model is LP64");
#define FAULT_PC "pc"
#else
_Static_assert(sizeof(void *) == 4, "the guest image model requires an ILP32 build");
#define FAULT_PC "eip"
#endif

/* The first 64 KiB. On the console that is kernel RAM, and retail code reaches
 * it through null pointers: CardList_CreateSlotTextBox clears a flag in
 * box->field_28 one call before that object exists, a read-modify-write of
 * address 8 that nothing notices. Hosts do not let a process map page zero,
 * so such an access faults; the handler then points the instruction's base
 * register at `low_memory` (the same pages guest RAM has at 0x80000000),
 * single-steps it, and puts the register back unless the instruction itself
 * replaced it. Each site is reported once. */
#ifndef _WIN32
static unsigned char *low_memory;
#else
/* Which 64 KiB pieces of the physical mirror Memories_GuestMap could map;
 * Windows holds the others (see there). */
static unsigned char low_piece_mapped[MEMORIES_GUEST_RAM_SIZE / 0x10000u];
#endif
#if defined(__i386__) || defined(__x86_64__)
static struct {
    int active, reg; /* reg: the ModRM register number, 0 (EAX) to 7 (EDI) */
    uint32_t original, patched;
} low_fixup;
#endif
/* Whether the retail scratchpad view (0x1F800000) is mapped (image.h);
 * where it is not, an access there takes the same register rebase as the
 * first 64 KiB, onto the port's view. Windows always maps it. */
int Memories_ScratchpadRetailView;
/* Memories_GuestMapError: the first step Memories_GuestMap failed at. */
static char map_error[160];
const char *Memories_GuestMapError(void) { return map_error; }
/* The mappings that held the range of that step (say_occupants). */
static char map_occupants[1024];
const char *Memories_GuestMapOccupants(void) { return map_occupants; }

static void report_low_access(uint32_t eip, uint32_t address)
{
    static uint32_t seen[32];
    static unsigned count;
    char text[128];
    unsigned i;
    int length;
    for (i = 0; i < count; i++) {
        if (seen[i] == eip) {
            return;
        }
    }
    if (count < sizeof(seen) / sizeof(seen[0])) {
        seen[count++] = eip;
    }
    if (address >= MEMORIES_GUEST_SCRATCHPAD_RETAIL)
        length = snprintf(text, sizeof(text), "memories-pc: scratchpad access through 0x%08x at " FAULT_PC " 0x%08x goes to 0x%08x\n",
                          (unsigned)address, (unsigned)eip, (unsigned)(address | 0x80000000u));
    else
        length = snprintf(text, sizeof(text), "memories-pc: null-pointer access to 0x%04x at " FAULT_PC " 0x%08x goes to kernel RAM, as on the console\n",
                          (unsigned)address, (unsigned)eip);
    (void)!write(2, text, (size_t)length);
}

#if defined(__i386__) || defined(__x86_64__)
/* Which register (ModRM number) does the faulting instruction address memory
 * through? -1 for none. */
#define REGISTER_ESI 6
#define REGISTER_EDI 7
static int low_access_register(const unsigned char *code, uint32_t esi)
{
    unsigned modrm, base;
    while (*code == 0x66 || *code == 0xf2 || *code == 0xf3 || *code == 0x2e || *code == 0x36 || *code == 0x3e ||
           *code == 0x26) {
        code++;
    }
    if ((*code >= 0xa4 && *code <= 0xa7) || *code == 0xaa || *code == 0xab) { /* string moves and stores */
        if (*code != 0xaa && *code != 0xab && esi < 0x10000u) {
            return REGISTER_ESI;
        }
        return REGISTER_EDI;
    }
    code += *code == 0x0f ? 2 : 1;
    modrm = *code++;
    if (modrm >> 6 == 3 || ((modrm >> 6) == 0 && (modrm & 7) == 5)) {
        return -1; /* register operand, or an absolute address */
    }
    base = modrm & 7;
    if (base == 4) {
        unsigned sib = *code;
        base = sib & 7;
        if (base == 5 && modrm >> 6 == 0) {
            return -1;
        }
    }
    return (int)base;
}
#endif

/* Tables in the retail data image hold MIPS function addresses, and native
 * code calls through them. The build generates Memories_FunctionMap (guest
 * address -> native function, sorted); a call to such an address resumes in
 * the native function. The caller's return address and cdecl arguments are
 * already on the stack, so the redirect is transparent. Two ways lead here:
 * the indirect-branch thunks every unit is compiled to use (branch_thunks.c,
 * through guest_branch_target below), and, as the second net, the fault of
 * executing guest RAM, which is mapped without execute permission
 * (on_guest_exception, on_fault). Anything else is fatal.
 * Returns where to resume, or NULL. */
static int in_guest_ram(uint32_t address)
{
    return (address >= 0x10000u && address < MEMORIES_GUEST_RAM_SIZE) ||
           address - MEMORIES_GUEST_RAM < MEMORIES_GUEST_RAM_SIZE || address - 0xa0000000u < MEMORIES_GUEST_RAM_SIZE;
}

static void *guest_call_target(uint32_t address)
{
    const MemoriesGuestFunction *entry;
    /* Normalize RAM mirrors before the shared residency-aware lookup. */
    if (in_guest_ram(address)) {
        address = MEMORIES_GUEST_RAM | (address & (MEMORIES_GUEST_RAM_SIZE - 1u));
    }
    entry = Memories_FindResidentFunction(address);
    if (entry) return (void *)(uintptr_t)entry->host;
#ifdef MEMORIES_REFERENCE
    if (Memories_MipsInOverlay(address)) {
        /* A callback into a loaded overlay: run it interpreted (a reference
         * build; a shipped game has no interpreter, and such a call is the
         * "no native function" report every caller makes of NULL). */
        Memories_MipsThunkTarget = address;
        return (void *)(uintptr_t)Memories_MipsThunk;
    }
#endif
    return NULL;
}

static void report_guest_fault(uint32_t address, uint32_t eip)
{
    char text[128];
    int length;
    if (eip == address) {
        length = snprintf(text, sizeof(text), "memories-pc: call into guest code at 0x%08x, which has no native function\n",
                          (unsigned)address);
    } else {
        length = snprintf(text, sizeof(text), "memories-pc: bad memory access at 0x%08x (" FAULT_PC " 0x%08x)\n",
                          (unsigned)address, (unsigned)eip);
    }
    (void)!write(2, text, (size_t)length);
}

/* Memories_GuestBranchResolver: where an indirect call or jump the thunks
 * caught goes. Outside guest RAM and its mirrors (and below 0x10000, so that
 * a call through a null pointer still faults as one; on Windows also in the
 * pieces of the physical mirror that Windows holds, which are not guest
 * RAM), the address itself. A guest address with no native function is
 * never jumped to: with DEP off its MIPS bytes would run as x86 code. */
static void *guest_branch_target(unsigned address)
{
    char text[64];
    void *target;
    if (!in_guest_ram(address)) {
        return (void *)(uintptr_t)address;
    }
#ifdef _WIN32
    if (address < MEMORIES_GUEST_RAM_SIZE && !low_piece_mapped[address >> 16]) {
        return (void *)(uintptr_t)address;
    }
#endif
    if ((target = guest_call_target(address)) != NULL) {
        return target;
    }
    report_guest_fault(address, address);
    snprintf(text, sizeof(text), "0x%08x has no native function", address);
    Crash_ReportFatal("call into guest code", text);
    Profile_Flush();
    _exit(70);
}

/* The thunks let a host target through after one test of its address
 * bits (branch_thunks.c). The executable has a fixed base where that test
 * passes; were its code somewhere the test fails, calls would still work,
 * through the resolver, only slower: say so. */
static void check_code_address(void)
{
    if (!((uintptr_t)check_code_address & 0x5fe00000u)) {
        fprintf(stderr, "memories-pc: the executable's code (0x%08x) is where the branch thunks take the slow path\n",
                (unsigned)(uintptr_t)check_code_address);
    }
}

/* Guest RAM mapped executable (MEMORIES_TEST_EXEC_GUEST=1), as it is where
 * DEP is off: then only the thunks keep a guest call from running MIPS bytes,
 * which makes "the game works without DEP" testable on any machine. Only
 * builds that are not releases have it (MEMORIES_TEST_HOOKS, set by
 * tools/pc/build_game32.py without --release): a shipped executable that
 * can map memory writable and executable is one more thing virus scanners'
 * heuristics hold against it. */
#ifdef MEMORIES_TEST_HOOKS
static int guest_ram_executable(void)
{
    const char *value = getenv("MEMORIES_TEST_EXEC_GUEST");
    if (!value || !*value || !strcmp(value, "0")) return 0;
    fprintf(stderr, "memories-pc: guest RAM is mapped executable (MEMORIES_TEST_EXEC_GUEST)\n");
    return 1;
}
#endif

#if defined(_WIN32) && defined(__x86_64__)
void __x86_indirect_thunk_r11(void); /* branch_thunks.c */
static DWORD64 *context_register(CONTEXT *context, int number)
{
    switch (number) {
    case 0: return &context->Rax;
    case 1: return &context->Rcx;
    case 2: return &context->Rdx;
    case 3: return &context->Rbx;
    case 4: return &context->Rsp;
    case 5: return &context->Rbp;
    case 6: return &context->Rsi;
    case 7: return &context->Rdi;
    case 8: return &context->R8;
    case 9: return &context->R9;
    case 10: return &context->R10;
    case 11: return &context->R11;
    case 12: return &context->R12;
    case 13: return &context->R13;
    case 14: return &context->R14;
    default: return &context->R15;
    }
}

/* low_access_register with x86-64's REX prefix: which register (0 RAX to
 * 15 R15) does the faulting instruction address memory through? -1 for
 * none, a RIP-relative or absolute address, and for VEX-encoded
 * instructions, which game code at -O0 does not use for such accesses.
 * A load through a G32 pointer has no base register: clang addresses it
 * as disp32 plus the zero-extended pointer as the index (`movl
 * 0x1c(,%rax), %eax`). Then the index is the register, and *scale its
 * scale, which the fix-up divides the offset to guest RAM by. */
static int low_access_register64(const unsigned char *code, uint64_t rsi, unsigned *scale)
{
    unsigned rex = 0, modrm, base;
    *scale = 1;
    while (*code == 0x66 || *code == 0x67 || *code == 0xf2 || *code == 0xf3 || *code == 0x2e || *code == 0x36 ||
           *code == 0x3e || *code == 0x26 || *code == 0x64 || *code == 0x65) {
        code++;
    }
    if ((*code & 0xf0) == 0x40) rex = *code++;
    if ((*code >= 0xa4 && *code <= 0xa7) || *code == 0xaa || *code == 0xab) { /* string moves and stores */
        if (*code != 0xaa && *code != 0xab && rsi < 0x10000u) {
            return REGISTER_ESI;
        }
        return REGISTER_EDI;
    }
    if (*code == 0xc4 || *code == 0xc5) return -1;
    if (*code == 0x0f) {
        code += code[1] == 0x38 || code[1] == 0x3a ? 3 : 2;
    } else {
        code++;
    }
    modrm = *code++;
    if (modrm >> 6 == 3 || ((modrm >> 6) == 0 && (modrm & 7) == 5)) {
        return -1; /* register operand, or RIP-relative */
    }
    base = modrm & 7;
    if (base == 4) {
        unsigned sib = *code, index = (sib >> 3 & 7) | (rex & 2u) << 2;
        base = sib & 7;
        if (base == 5 && modrm >> 6 == 0) {
            if (index == 4) return -1; /* no index either: an absolute address */
            *scale = 1u << (sib >> 6);
            return (int)index;
        }
    }
    return (int)(base | (rex & 1u) << 3);
}

/* MEMORIES_X64_HIGH_HEAP=1 (a check for the 64-bit build): every address
 * range below 4 GB that is still free once guest RAM is mapped is reserved,
 * so that the host heap, and everything else Windows hands out later, lands
 * above 4 GB. A host pointer that reaches a 4-byte guest slot then loses its
 * upper half, and the access through what is left lands in one of these
 * reservations: the fault is reported as that. The fixed regions mapped
 * later (the game stack, the text arena, the interpreter's stack, mod
 * arenas) take their range back out of a reservation as they are mapped
 * (Memories_ReleaseLowPlaceholder, from compat/mman.h's mmap). The process
 * heap's own segments predate this and would go on serving small blocks
 * from below 4 GB, so they are filled up first, and the blocks kept. */
typedef struct HighHeapRange { uintptr_t low, high; } HighHeapRange;
static HighHeapRange high_heap[512];
static unsigned high_heap_count;

static void reserve_free(uintptr_t low, uintptr_t high)
{
    low = (low + 0xffffu) & ~(uintptr_t)0xffffu;
    high &= ~(uintptr_t)0xffffu;
    if (high <= low || high_heap_count == sizeof(high_heap) / sizeof(high_heap[0])) return;
    if (VirtualAlloc((void *)low, high - low, MEM_RESERVE, PAGE_NOACCESS) == (void *)low) {
        high_heap[high_heap_count].low = low;
        high_heap[high_heap_count++].high = high;
    }
}

void Memories_ReleaseLowPlaceholder(void *address, size_t length)
{
    uintptr_t low = (uintptr_t)address & ~(uintptr_t)0xffffu, high = (uintptr_t)address + length;
    unsigned i;
    for (i = 0; i < high_heap_count; i++) {
        HighHeapRange range = high_heap[i];
        if (high <= range.low || low >= range.high) continue;
        /* One reservation is freed whole: give it back, then keep its
         * parts on either side. */
        VirtualFree((void *)range.low, 0, MEM_RELEASE);
        high_heap[i] = high_heap[--high_heap_count];
        if (range.low < low) reserve_free(range.low, low);
        if (high < range.high) reserve_free((high + 0xffffu) & ~(uintptr_t)0xffffu, range.high);
        i = (unsigned)-1; /* the table changed: look again */
    }
}

/* Code mods' images (src/pc/mods/object_loader.c): the 64-bit game's mods
 * go right after its own image, below 4 GB with bit 30 set, so that a mod's
 * function fits a 4-byte guest slot and passes the branch thunks' fast path
 * as the game's own do, and its calls reach the game with 32-bit offsets.
 * Held from Memories_GuestMap with the other fixed regions; each image takes
 * its piece back as compat/mman.h's mmap maps it. */
#define MOD_CODE_SIZE 0x08000000u
static uintptr_t mod_code_start, mod_code_end;

int Memories_ModCodeRange(uintptr_t *start, uintptr_t *end)
{
    if (!mod_code_end) return 0;
    *start = mod_code_start;
    *end = mod_code_end;
    return 1;
}

static void reserve_mod_code(uintptr_t base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    uintptr_t start = (base + nt->OptionalHeader.SizeOfImage + 0xffffu) & ~(uintptr_t)0xffffu;
    if (start + MOD_CODE_SIZE > 0x80000000u) return;   /* not where the game was linked: no range */
    mod_code_start = start;
    mod_code_end = start + MOD_CODE_SIZE;
    reserve_free(mod_code_start, mod_code_end);
}

static void fill_low_heap(void)
{
    static const size_t sizes[] = {0x10000, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048, 4096,
                                   8192, 16384};
    unsigned i, count;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (count = 0; count < 1000000u; count++) {
            void *block = malloc(sizes[i]);
            if (!block) break;
            if ((uintptr_t)block >= 0x100000000ull) {
                free(block);
                break;
            }
            /* kept: the low block must not be handed out again */
        }
    }
}

static void reserve_low_memory(void)
{
    const char *value = getenv("MEMORIES_X64_HIGH_HEAP");
    uintptr_t at = 0x10000u, reserved = 0;
    unsigned i;
    if (!value || !*value || !strcmp(value, "0")) return;
    while (at < 0x100000000ull) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t end;
        if (!VirtualQuery((void *)at, &info, sizeof(info))) break;
        end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (end > 0x100000000ull) end = 0x100000000ull;
        if (info.State == MEM_FREE) reserve_free(at, end);
        at = end;
    }
    fill_low_heap();
    for (i = 0; i < high_heap_count; i++) reserved += high_heap[i].high - high_heap[i].low;
    {
        void *small = malloc(64), *large = malloc(1u << 20);
        fprintf(stderr, "memories-pc: MEMORIES_X64_HIGH_HEAP: %u MiB below 4 GB reserved in %u ranges; malloc(64) "
                        "%p, malloc(1 MiB) %p\n", (unsigned)(reserved >> 20), high_heap_count, small, large);
        free(small);
        free(large);
    }
}

static int in_high_heap_reservation(uintptr_t address)
{
    unsigned i;
    for (i = 0; i < high_heap_count; i++) {
        if (address >= high_heap[i].low && address < high_heap[i].high) return 1;
    }
    return 0;
}

static LONG CALLBACK on_guest_exception(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    uintptr_t address;
    void *target;
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        DWORD64 *reg;
        if (!low_fixup.active) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        low_fixup.active = 0;
        reg = context_register(context, low_fixup.reg);
        if (*reg == low_fixup.patched) {
            *reg = low_fixup.original;
        }
        context->EFlags &= ~0x100; /* trap flag */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uintptr_t)record->ExceptionInformation[1];
    if (address < MEMORIES_GUEST_RAM_SIZE && context->Rip != address && !low_fixup.active) {
        unsigned scale;
        int reg = low_access_register64((const unsigned char *)(uintptr_t)context->Rip, context->Rsi, &scale);
        if (reg >= 0 && *context_register(context, reg) < MEMORIES_GUEST_RAM_SIZE) {
            report_low_access((uint32_t)context->Rip, (uint32_t)address);
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = (uint32_t)*context_register(context, reg);
            low_fixup.patched = low_fixup.original + MEMORIES_GUEST_RAM / scale;
            *context_register(context, reg) = low_fixup.patched;
            context->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (context->Rip == address && address < 0x100000000ull &&
        (target = guest_call_target((uint32_t)address)) != NULL) {
        context->Rip = (DWORD64)(uintptr_t)target;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (in_high_heap_reservation(address)) {
        char text[160];
        snprintf(text, sizeof(text), "access to 0x%08llx at rip 0x%llx: a host pointer above 4 GB that lost its "
                                     "upper half in a 4-byte guest slot?", (unsigned long long)address,
                 (unsigned long long)context->Rip);
        fprintf(stderr, "memories-pc: %s\n", text);
        Crash_ReportFatal("truncated host pointer", text);
        Profile_Flush();
        _exit(70);
    }
    if (context->Rip == address || address < 0x10000u ||
        (address >= MEMORIES_GUEST_RAM && address < MEMORIES_GUEST_RAM + 0x00800000u)) {
        report_guest_fault((uint32_t)address, (uint32_t)context->Rip);
    }
    if (context->Rip == address || context->Rip - (uintptr_t)__x86_indirect_thunk_r11 < 0x40 ||
        context->Rip - (uintptr_t)GetModuleHandleW(NULL) < 0x10000000u) {
        /* A fault the game will not survive, in its own code or in a call
         * to where nothing is (or, a #GP in the branch thunk, to a
         * non-canonical address, reported as 0xFFFFFFFF...). R11 is where
         * a call through the thunk was going. The crash report that follows
         * lists the callers, from the unwind tables and symbolized (crash.c,
         * Win32_UnwindCallers). */
        fprintf(stderr, "memories-pc: fault at rip 0x%llx (r11 0x%llx); the crash report lists its callers\n",
                (unsigned long long)context->Rip, (unsigned long long)context->R11);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#elif defined(_WIN32)
static DWORD *context_register(CONTEXT *context, int number)
{
    switch (number) {
    case 0: return &context->Eax;
    case 1: return &context->Ecx;
    case 2: return &context->Edx;
    case 3: return &context->Ebx;
    case 4: return &context->Esp;
    case 5: return &context->Ebp;
    case 6: return &context->Esi;
    default: return &context->Edi;
    }
}

/* The common low access, a plain 32-bit MOV to or from [base + disp], is
 * done here through guest RAM instead of the rebase-and-single-step path:
 * 32-bit processes on 64-bit Windows can mishandle the trap when the
 * instruction's destination is its own base register. Returns 1 if done. */
static int emulate_low_mov(CONTEXT *context, uint32_t address)
{
    const unsigned char *code = (const unsigned char *)(uintptr_t)context->Eip;
    unsigned modrm, mod, reg, rm;
    uint32_t *guest;
    if ((code[0] != 0x8b && code[0] != 0x89) || address > MEMORIES_GUEST_RAM_SIZE - 4) {
        return 0;
    }
    modrm = code[1];
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    if (mod == 3 || rm == 4 || (mod == 0 && rm == 5)) {
        return 0; /* register operand, SIB byte or absolute address */
    }
    guest = (uint32_t *)(uintptr_t)(MEMORIES_GUEST_RAM + address);
    if (code[0] == 0x8b) {
        *context_register(context, (int)reg) = *guest;
    } else {
        *guest = *context_register(context, (int)reg);
    }
    context->Eip += 2u + (mod == 1 ? 1u : mod == 2 ? 4u : 0u);
    return 1;
}

/* First in line for every exception in the process: take the guest's own
 * faults, leave everything else to the next handler (win32.c reports what
 * the executable raised). */
static LONG CALLBACK on_guest_exception(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    uint32_t address;
    void *target;
    Win32_UndoInterruptedFault(context); /* then handled as the faulting instruction's own */
    /* A 32-bit process on 64-bit Windows may see the trap as WoW64's own
     * STATUS_WX86_SINGLE_STEP. */
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP || record->ExceptionCode == 0x4000001eu) {
        DWORD *reg;
        if (!low_fixup.active) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        low_fixup.active = 0;
        reg = context_register(context, low_fixup.reg);
        if (*reg == low_fixup.patched) {
            *reg = low_fixup.original;
        }
        context->EFlags &= ~0x100; /* trap flag */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uint32_t)record->ExceptionInformation[1];
    /* The first 64 KiB and the parts of the physical mirror Windows holds
     * (see Memories_GuestMap) are reached the same way: through guest RAM
     * at 0x80000000, which the same offsets address. */
    if (address < MEMORIES_GUEST_RAM_SIZE && context->Eip != address && !low_fixup.active) {
        int reg = low_access_register((const unsigned char *)(uintptr_t)context->Eip, context->Esi);
        if (reg >= 0 && *context_register(context, reg) < MEMORIES_GUEST_RAM_SIZE) {
            report_low_access(context->Eip, address);
            if (emulate_low_mov(context, address)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = *context_register(context, reg);
            low_fixup.patched = low_fixup.original + MEMORIES_GUEST_RAM;
            *context_register(context, reg) = low_fixup.patched;
            context->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (context->Eip == address && (target = guest_call_target(address)) != NULL) {
        context->Eip = (DWORD)(uintptr_t)target;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (context->Eip == address || address < 0x10000u ||
        (address >= MEMORIES_GUEST_RAM && address < MEMORIES_GUEST_RAM + 0x00800000u)) {
        report_guest_fault(address, context->Eip);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

#ifdef _WIN32
/* The Linux layout as far as Windows allows: one pagefile-backed section
 * holds guest RAM and is viewed at 0x80000000 and 0xA0000000. The physical
 * mirror (0x10000..0x200000) competes with what Windows puts there before
 * the program starts (process parameters, locale tables, the WoW64 stack),
 * so it is mapped in 64 KiB pieces where the address space is free; an
 * access to a piece Windows holds faults and goes through guest RAM
 * instead (on_guest_exception). Such accesses into pages Windows has
 * mapped readable would not fault; the sites that fault are reported. */
static DWORD view_access = FILE_MAP_ALL_ACCESS;

static int view_at(HANDLE section, uint32_t address, size_t length, DWORD offset)
{
    void *wanted = (void *)(uintptr_t)address;
    if (MapViewOfFileEx(section, view_access, 0, offset, length, wanted) != wanted) {
        fprintf(stderr, "cannot map guest memory at 0x%08x (error %lu)\n", (unsigned)address,
                GetLastError());
        return -1;
    }
    return 0;
}

/* Everything of a 64 KiB scratchpad view past its first page, inaccessible. */
static int view_tail_closed(uint32_t address)
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)(address + 0x1000u), 0xf000u, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "cannot protect 0x%08x-0x%08x (error %lu)\n", (unsigned)address + 0x1000u,
                (unsigned)address + 0x10000u, GetLastError());
        return -1;
    }
    return 0;
}

/* Calls into guest code go through the branch thunks: the game works
 * without DEP. Where DEP is on, guest RAM mapped without execute permission
 * is a second safety net: a call that escaped the thunks faults into
 * on_guest_exception instead of running MIPS bytes. The game is a 32-bit
 * process, which follows the system's DEP policy (only 64-bit processes
 * always have DEP): under OptIn, the default, the executable's --nxcompat
 * turns it on. The game does not change the policy itself: a program that
 * calls SetProcessDEPPolicy is what virus scanners' heuristics look for. */
int Memories_GuestMap(void)
{
    HANDLE section;
    DWORD protection = PAGE_READWRITE;
    int result;
#ifdef MEMORIES_TEST_HOOKS
    if (guest_ram_executable()) {
        protection = PAGE_EXECUTE_READWRITE;
        view_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
    }
#endif
    Memories_GuestBranchResolver = guest_branch_target;
    check_code_address();
    /* Guest RAM, then 64 KiB (the view granularity) for the scratchpad. */
    section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, protection, 0, MEMORIES_GUEST_RAM_SIZE + 0x10000u, NULL);
    AddVectoredExceptionHandler(1, on_guest_exception);
    if (section == NULL) {
        fprintf(stderr, "guest RAM: CreateFileMapping failed (error %lu)\n", GetLastError());
        return -1;
    }
    /* The mirror first: tested on Windows 11, a view below 0x200000 fails
     * with ERROR_INVALID_ADDRESS once the high views exist. */
    {
        uint32_t piece, held = 0;
        for (piece = 0x10000u; piece < MEMORIES_GUEST_RAM_SIZE; piece += 0x10000u) {
            if (MapViewOfFileEx(section, view_access, 0, piece, 0x10000u, (void *)(uintptr_t)piece) == NULL) {
                held += 0x10000u;
            } else {
                low_piece_mapped[piece >> 16] = 1;
            }
        }
        if (held) {
            fprintf(stderr, "memories-pc: %u KiB of the physical RAM mirror are taken by Windows; accesses there fault into guest RAM\n",
                    (unsigned)(held / 1024));
        }
    }
    /* The scratchpad's two views (image.h); Windows leaves 0x1F800000 free.
     * A view is 64 KiB; past the scratchpad's page it is made inaccessible,
     * so the I/O registers from 0x1F801000 (and 0x9F801000) fault as before
     * instead of reading 0. */
    result = view_at(section, MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, 0) ||
             view_at(section, 0xa0000000u, MEMORIES_GUEST_RAM_SIZE, 0) ||
             view_at(section, MEMORIES_GUEST_SCRATCHPAD, 0x10000u, MEMORIES_GUEST_RAM_SIZE) ||
             view_at(section, MEMORIES_GUEST_SCRATCHPAD_RETAIL, 0x10000u, MEMORIES_GUEST_RAM_SIZE) ||
             view_tail_closed(MEMORIES_GUEST_SCRATCHPAD) || view_tail_closed(MEMORIES_GUEST_SCRATCHPAD_RETAIL);
    Memories_ScratchpadRetailView = !result;
    /* The views keep the section alive. */
    CloseHandle(section);
#if defined(__x86_64__)
    {
        /* Native function addresses go into 4-byte guest slots: the image
         * must be where it was linked (0x40000000, no ASLR), below 4 GB. */
        uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        const char *report = getenv("MEMORIES_X64_MAP_REPORT");
        if (base >= 0x100000000ull || (uintptr_t)Memories_GuestMap >= 0x100000000ull) {
            fprintf(stderr, "memories-pc: the executable was loaded at %p, above 4 GB; the 64-bit game needs it at "
                            "its link address, 0x40000000\n", (void *)base);
            result = -1;
        }
        if (report && *report && strcmp(report, "0")) {
            fprintf(stderr, "memories-pc: image %p; guest RAM 0x80000000 and 0xA0000000 %s; scratchpad 0x9F800000 and 0x1F800000\n",
                    (void *)base, result ? "NOT mapped" : "mapped");
        }
    }
    if (!result) {
        /* The fixed regions mapped later (mod code, mod arenas, the text
         * arena, the low memory region, a reference build's interpreter
         * stack, the game stack) are held from here: in a 64-bit process the
         * window's GL driver and audio load below 4 GB too, and took the game
         * stack's range before it was mapped. Each takes its range back as
         * compat/mman.h's mmap maps it. */
        reserve_mod_code((uintptr_t)GetModuleHandleW(NULL));
        reserve_free(0x90000000u, 0x91000000u);
        reserve_free(0x9C000000u, 0x9D000000u);
        reserve_free(MEMORIES_LOW_MEMORY_BASE, MEMORIES_LOW_MEMORY_BASE + MEMORIES_LOW_MEMORY_SIZE);
#ifdef MEMORIES_REFERENCE
        reserve_free(0x9FF00000u, 0x9FF40000u); /* mips.c's stack: nothing else maps there */
#endif
        reserve_free(0xB0000000u, 0xB0800000u);
        reserve_low_memory();
    }
#endif
    return result ? -1 : 0;
}

int Memories_KernelRefusesTakenRange(void) { return -1; }
#else
static int view_protection = PROT_READ | PROT_WRITE;

/* What already holds part of [low, high) (/proc/self/maps), for a guest
 * range that cannot be mapped: in an Android app ART's heap can reach the
 * guest's ranges (notes/pc-build.md, "Android arm64"). With `keep`, the
 * mappings also go to map_occupants (Memories_GuestMapOccupants) as
 * "start-end perms name", for the player's report. */
static void say_occupants(uint32_t low, uint64_t high, int keep)
{
    char text[512];
    unsigned long long start, end;
    size_t used = strlen(map_occupants);
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    while (fgets(text, sizeof(text), maps)) {
        char perms[8];
        int name = 0;
        if (sscanf(text, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &name) < 3 || start >= high ||
            end <= low)
            continue;
        fprintf(stderr, "memories-pc: occupied by %s", text);
        if (keep && used + 1 < sizeof(map_occupants)) {
            int added = snprintf(map_occupants + used, sizeof(map_occupants) - used, "%llx-%llx %s %.*s\n", start,
                                 end, perms, name ? (int)strcspn(text + name, "\r\n") : 0, name ? text + name : "");
            if (added > 0)
                used = used + (size_t)added < sizeof(map_occupants) ? used + (size_t)added : sizeof(map_occupants) - 1;
        }
    }
    fclose(maps);
}

static int map_at(uint32_t address, size_t length, int fd, off_t offset)
{
    void *wanted = (void *)(uintptr_t)address;
    int flags = MAP_FIXED_NOREPLACE | (fd < 0 ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_SHARED);
    if (Memories_MapFixed(wanted, length, fd < 0 ? PROT_READ | PROT_WRITE : view_protection, flags, fd, offset) !=
        wanted) {
        /* EEXIST: something holds part of the range (a kernel before 4.17,
         * which takes the address as a hint, put it elsewhere: told the
         * same way by Memories_MapFixed). */
        int error = errno, first = !map_error[0];
        const char *why = error == EEXIST ? "something else in this process already holds them" : strerror(error);
        fprintf(stderr, "cannot map guest memory at 0x%08x: %s\n", (unsigned)address, why);
        if (first)
            snprintf(map_error, sizeof(map_error), "the addresses from 0x%08X to 0x%08llX could not be had (%s)",
                     (unsigned)address, (unsigned long long)address + length - 1, why);
        say_occupants(address, (uint64_t)address + length, first);
        return -1;
    }
    return 0;
}

int Memories_KernelRefusesTakenRange(void)
{
    /* MAP_FIXED_NOREPLACE over a page that is surely mapped (this file's
     * own data): EEXIST where the kernel knows the flag; a kernel before
     * 4.17 maps a page elsewhere, never over it (without MAP_FIXED the
     * address is only a hint). */
    long page = sysconf(_SC_PAGESIZE);
    void *held, *got;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
#ifdef MEMORIES_TEST_HOOKS
    const char *old = getenv("MEMORIES_TEST_OLD_KERNEL");
    if (old && old[0] && old[0] != '0') flags &= ~MAP_FIXED_NOREPLACE;
#endif
    if (page <= 0) return -1;
    held = (void *)((uintptr_t)map_error & ~(uintptr_t)(page - 1));
    got = mmap(held, (size_t)page, PROT_NONE, flags, -1, 0);
    if (got == MAP_FAILED) return errno == EEXIST ? 1 : -1;
    if (got == held) return -1; /* cannot be (no MAP_FIXED); never unmapped, it would be this file's data */
    munmap(got, (size_t)page);
    return 0;
}

#if defined(__i386__)
/* ModRM/SIB register numbers to gregs[]. */
static const int register_slot[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};

static void on_step(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    greg_t *reg;
    (void)number; (void)info;
    if (low_fixup.active) {
        low_fixup.active = 0;
        reg = &user->uc_mcontext.gregs[register_slot[low_fixup.reg]];
        if ((uint32_t)*reg == low_fixup.patched) {
            *reg = (greg_t)low_fixup.original;
        }
    } else {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigaction(SIGTRAP, &action, NULL);
        raise(SIGTRAP);
        return;
    }
    user->uc_mcontext.gregs[REG_EFL] &= ~0x100; /* trap flag */
}

static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    uint32_t eip = (uint32_t)user->uc_mcontext.gregs[REG_EIP];
    void *target;
    /* The first 64 KiB go through the low_memory view of guest RAM; the
     * retail scratchpad view, where it could not be mapped, through the
     * port's (image.h). Both are the same register rebase. */
    if (eip != address && !low_fixup.active &&
        ((address < 0x10000u && low_memory) ||
         (!Memories_ScratchpadRetailView && address - MEMORIES_GUEST_SCRATCHPAD_RETAIL < 0x1000u))) {
        int low = address < 0x10000u;
        int reg = low_access_register((const unsigned char *)(uintptr_t)eip, (uint32_t)user->uc_mcontext.gregs[REG_ESI]);
        uint32_t base = reg >= 0 ? (uint32_t)user->uc_mcontext.gregs[register_slot[reg]] : 0;
        if (reg >= 0 && (low ? base < 0x10000u : base - MEMORIES_GUEST_SCRATCHPAD_RETAIL < 0x1000u)) {
            report_low_access(eip, address);
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = base;
            low_fixup.patched = base + (low ? (uint32_t)(uintptr_t)low_memory : 0x80000000u);
            user->uc_mcontext.gregs[register_slot[reg]] = (greg_t)low_fixup.patched;
            user->uc_mcontext.gregs[REG_EFL] |= 0x100;
            return;
        }
    }
    if (eip == address && (target = guest_call_target(address)) != NULL) {
        user->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)target;
        return;
    }
    report_guest_fault(address, eip);
    Crash_HandleSignal(number, info, context);
}
#elif defined(__aarch64__)
/* 64-bit ARM (AArch64), for the two faults the i386 handler above takes.
 * AArch64 has no single-step trap, so a fixed-up instruction runs once out
 * of line instead.
 *
 * A low or retail-scratchpad access runs once out of line: the handler
 * moves the register that addresses the low page (the base Rn, bits 5-9 in
 * every load/store class, or for a register offset with no shift the index
 * Rm, bits 16-20) up by 0x80000000, onto the same RAM in KSEG0, and
 * resumes in a stub: the instruction, then `eor Xr, Xr, #0x80000000`
 * (which also undoes the move after a writeback), then a `b` back behind
 * the original. A direct branch, because no register is free at an
 * arbitrary load (X16/X17 are only scratch at a call); so the stub page is
 * mapped within the branch's reach (+-128 MiB) of the faulting code, one
 * page per region the faults come from. When the instruction loads the
 * moved register the loaded value stands and there is no `eor`. Not
 * handled, and fatal as before: a store of the moved register itself, an
 * SP base, and PC-relative loads (which do not fault low).
 *
 * A call into guest code: the target of a guest address, as on i386. LR
 * holds the caller's return address already, and X0-X7 the arguments. */
#define A64_NOP 0xd503201fu
#define A64_EOR_2_31 0xd2610000u /* eor Xd, Xn, #0x80000000 (N=1, immr=33, imms=0) */
#define A64_B 0x14000000u

static struct {
    uint32_t *page;
} fixup_pages[8];

static int a64_in_reach(uintptr_t from, uintptr_t to)
{
    intptr_t delta = (intptr_t)(to - from);
    return delta >= -(intptr_t)0x08000000 && delta < (intptr_t)0x08000000;
}

/* A stub page within reach of `pc` (and of `pc + 4`, where it returns). */
static uint32_t *fixup_page_near(uintptr_t pc)
{
    unsigned i;
    uintptr_t hint;
    for (i = 0; i < sizeof(fixup_pages) / sizeof(fixup_pages[0]) && fixup_pages[i].page; i++) {
        if (a64_in_reach(pc, (uintptr_t)fixup_pages[i].page) &&
            a64_in_reach((uintptr_t)fixup_pages[i].page + 8, pc + 4)) {
            return fixup_pages[i].page;
        }
    }
    if (i == sizeof(fixup_pages) / sizeof(fixup_pages[0])) return NULL;
    /* Below the code first (the image's own gap), then above it. */
    for (hint = (pc & ~(uintptr_t)0xfffff) - 0x00400000u; hint; hint = 0) {
        void *page = mmap((void *)hint, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED && a64_in_reach(pc, (uintptr_t)page) && a64_in_reach((uintptr_t)page + 8, pc + 4)) {
            fixup_pages[i].page = page;
            return page;
        }
        if (page != MAP_FAILED) munmap(page, 0x10000);
        page = mmap((void *)((pc & ~(uintptr_t)0xfffff) + 0x00400000u), 0x10000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED && a64_in_reach(pc, (uintptr_t)page) && a64_in_reach((uintptr_t)page + 8, pc + 4)) {
            fixup_pages[i].page = page;
            return page;
        }
        if (page != MAP_FAILED) munmap(page, 0x10000);
    }
    return NULL;
}

/* The register a load or store addresses memory through that holds
 * `want(value)`, or -1. Sets *loads_it when the access loads that register,
 * *stores_it when it stores it. */
static int a64_access(uint32_t code, const uint64_t *regs, int low, int *loads_it, int *stores_it)
{
    unsigned rn = (code >> 5) & 31, rt = code & 31, rt2 = (code >> 10) & 31, rm = (code >> 16) & 31;
    int vector = (code >> 26) & 1, load, pair = 0, regoff = 0, chosen;
    if ((code & 0x0a000000u) != 0x08000000u) return -1; /* not the load/store class (op0 x1x0) */
    if ((code & 0x3a000000u) == 0x28000000u) {          /* LDP/STP/LDNP/STNP and the pre/post-indexed pairs */
        pair = 1;
        load = (code >> 22) & 1;
    } else if ((code & 0x3b000000u) == 0x38000000u) {   /* LDR/STR (immediate pre/post, unscaled, register) */
        load = ((code >> 22) & 3) != 0;
        regoff = (code & 0x00200c00u) == 0x00200800u;   /* bit 21 set, bits 11-10 = 10: register offset */
        /* Only an index taken as unsigned (UXTW, or LSL/UXTX) is rebased below:
         * SXTW would sign-extend the index moved up by 0x80000000. */
        if (regoff && ((code >> 12) & 1)) regoff = 2;   /* S: the index is shifted */
    } else if ((code & 0x3b000000u) == 0x39000000u) {   /* LDR/STR (unsigned immediate) */
        load = ((code >> 22) & 3) != 0;
    } else if ((code & 0x3f000000u) == 0x08000000u) {   /* exclusives, acquire/release */
        load = (code >> 22) & 1;
        if (!load && rm == rn) return -1;               /* the status register is the base */
    } else {
        return -1; /* SIMD structure loads, atomics and the rest: not seen here */
    }
    if (rn != 31 && (low ? regs[rn] < 0x10000u : regs[rn] - MEMORIES_GUEST_SCRATCHPAD_RETAIL < 0x1000u)) {
        chosen = (int)rn;
    } else if (regoff == 1 && rm != 31 && rm != rn && (((code >> 13) & 7) == 2 || ((code >> 13) & 7) == 3) &&
               (low ? regs[rm] < 0x10000u : regs[rm] - MEMORIES_GUEST_SCRATCHPAD_RETAIL < 0x1000u)) {
        chosen = (int)rm;
    } else {
        return -1;
    }
    if (regoff == 1 && rm == rn) return -1;
    if (!vector) {
        int hits = rt == (unsigned)chosen || (pair && rt2 == (unsigned)chosen);
        *loads_it = load && hits;
        *stores_it = !load && hits;
    }
    return chosen;
}

/* Returns 1 when the access will run from the stub. */
static int run_rebased(ucontext_t *user, uint32_t address)
{
    uint64_t *regs = (uint64_t *)user->uc_mcontext.regs;
    uintptr_t pc = (uintptr_t)user->uc_mcontext.pc;
    uint32_t code, *stub;
    int reg, loads_it = 0, stores_it = 0, low = address < 0x10000u;
    code = *(const uint32_t *)pc;
    reg = a64_access(code, regs, low, &loads_it, &stores_it);
    if (reg < 0 || stores_it) return 0;
    if (!(stub = fixup_page_near(pc))) return 0;
    report_low_access((uint32_t)pc, address);
    if (mprotect(stub, 0x10000, PROT_READ | PROT_WRITE)) return 0;
    stub[0] = code;
    stub[1] = loads_it ? A64_NOP : A64_EOR_2_31 | (uint32_t)reg << 5 | (uint32_t)reg;
    stub[2] = A64_B | (uint32_t)(((intptr_t)(pc + 4) - (intptr_t)(stub + 2)) >> 2 & 0x03ffffff);
    if (mprotect(stub, 0x10000, PROT_READ | PROT_EXEC)) return 0;
    __builtin___clear_cache((char *)stub, (char *)(stub + 3));
    regs[reg] += 0x80000000u;
    user->uc_mcontext.pc = (uint64_t)(uintptr_t)stub;
    return 1;
}

static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uintptr_t full = (uintptr_t)info->si_addr;
    uint32_t address = (uint32_t)full;
    uintptr_t pc = (uintptr_t)user->uc_mcontext.pc;
    void *target;
    if (pc != full && ((full < 0x10000u) ||
                       (!Memories_ScratchpadRetailView && full - MEMORIES_GUEST_SCRATCHPAD_RETAIL < 0x1000u))) {
        if (run_rebased(user, address)) return;
    }
    if (pc == full && full < 0x100000000ull && (target = guest_call_target(address)) != NULL) {
        user->uc_mcontext.pc = (uint64_t)(uintptr_t)target;
        return;
    }
    if (pc == full && full < 0x100000000ull) {
        report_guest_fault(address, (uint32_t)pc); /* a call into guest code with no native function */
    } else {
        /* Whole: a host address is 64 bits here (report_guest_fault's are 32). */
        char text[160];
        int length = snprintf(text, sizeof(text), "memories-pc: bad memory access at 0x%llx (pc 0x%llx, lr 0x%llx)\n",
                              (unsigned long long)full, (unsigned long long)pc,
                              (unsigned long long)user->uc_mcontext.regs[30]);
        (void)!write(2, text, (size_t)length);
    }
    Crash_HandleSignal(number, info, context);
}
#else
#error "image.c: no fault handler for this architecture"
#endif

#ifdef MEMORIES_TEST_HOOKS
/* MEMORIES_TEST_HOLD_SCRATCHPAD=rw or none: a page is held at 0x1F800000
 * before the guest is mapped, as an Android app's Java heap holds it:
 * read-write (filled with 0xA5) or with no access. The port then reaches
 * the scratchpad only at 0x9F800000, and a native access that still used
 * the retail address would reach the holder (rw) or fault (none). At exit
 * the port says whether anything wrote the read-write page. A test hook:
 * not in a release (see guest_ram_executable). */
static const unsigned char *held_page;

static void report_held_page(void)
{
    unsigned i, changed = 0;
    for (i = 0; i < 0x1000u; i++) changed += held_page[i] != 0xa5;
    fprintf(stderr, "memories-pc: the held page at 0x%08x: %s (%u bytes changed)\n", MEMORIES_GUEST_SCRATCHPAD_RETAIL,
            changed ? "WRITTEN" : "untouched", changed);
}

static void hold_retail_scratchpad(void)
{
    const char *value = getenv("MEMORIES_TEST_HOLD_SCRATCHPAD");
    void *wanted = (void *)(uintptr_t)MEMORIES_GUEST_SCRATCHPAD_RETAIL, *got;
    int writable;
    if (!value || !*value || !strcmp(value, "0")) return;
    writable = !strcmp(value, "rw");
    got = Memories_MapFixed(wanted, 0x1000, writable ? PROT_READ | PROT_WRITE : PROT_NONE,
                            MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got != wanted) {
        fprintf(stderr, "memories-pc: MEMORIES_TEST_HOLD_SCRATCHPAD: 0x%08x could not be held\n",
                MEMORIES_GUEST_SCRATCHPAD_RETAIL);
        return;
    }
    fprintf(stderr, "memories-pc: 0x%08x is held %s (MEMORIES_TEST_HOLD_SCRATCHPAD)\n",
            MEMORIES_GUEST_SCRATCHPAD_RETAIL, writable ? "read-write" : "with no access");
    if (writable) {
        memset(got, 0xa5, 0x1000);
        held_page = got;
        atexit(report_held_page);
    }
}

/* MEMORIES_TEST_HOLD_GUEST=1 (or an address, 0x80000000 by default): 2 MiB
 * there are held before the guest is mapped, as the large object space of
 * an Android app's Java runtime holds guest RAM on phones whose runtime
 * uses the concurrent copying collector (notes/pc-build.md, "Android
 * arm64"). Named as that space is where the kernel names anonymous
 * mappings (Android's), so the start-up report reads as the phone's would.
 * With MEMORIES_TEST_OLD_KERNEL=1 the guest's mappings then take the path
 * of a kernel before 4.17. A test hook: not in a release. */
static void hold_guest(void)
{
    const char *value = getenv("MEMORIES_TEST_HOLD_GUEST");
    unsigned long at;
    void *wanted, *got;
    if (!value || !*value || !strcmp(value, "0")) return;
    at = strtoul(value, NULL, 0);
    if (at <= 1) at = MEMORIES_GUEST_RAM;
    wanted = (void *)(uintptr_t)(at & ~0xFFFul);
    got = Memories_MapFixed(wanted, 0x200000, PROT_NONE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got != wanted) {
        fprintf(stderr, "memories-pc: MEMORIES_TEST_HOLD_GUEST: %p could not be held\n", wanted);
        return;
    }
#ifdef __linux__
    /* PR_SET_VMA, PR_SET_VMA_ANON_NAME: Android kernels, Linux 5.17+. */
    prctl(0x53564d41, 0, (unsigned long)(uintptr_t)got, 0x200000ul,
          (unsigned long)(uintptr_t)"dalvik-free list large object space (MEMORIES_TEST_HOLD_GUEST)");
#endif
    fprintf(stderr, "memories-pc: %p-%p is held (MEMORIES_TEST_HOLD_GUEST)\n", got, (void *)((char *)got + 0x200000));
}
#endif

int Memories_GuestMap(void)
{
    struct sigaction action;
    int fd, result;
#ifdef MEMORIES_TEST_HOOKS
    if (guest_ram_executable()) view_protection |= PROT_EXEC;
    hold_guest();
#endif
    Memories_GuestBranchResolver = guest_branch_target;
    check_code_address();
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
#if defined(__i386__)
    action.sa_sigaction = on_step;
    sigaction(SIGTRAP, &action, NULL);
#endif
    fd = memfd_create("memories-ram", 0);
    /* Guest RAM, then a page for the scratchpad. */
    if (fd >= 0 && ftruncate(fd, MEMORIES_GUEST_RAM_SIZE + 0x1000) != 0) {
        close(fd);
        fd = -1;
    }
#ifdef __ANDROID__
    /* Before memfd (Linux 3.17): the ashmem device (android.c). */
    if (fd < 0) fd = memories_ashmem_create("memories-ram", MEMORIES_GUEST_RAM_SIZE + 0x1000);
#endif
    if (fd < 0) {
        int error = errno;
        perror("guest RAM");
        snprintf(map_error, sizeof(map_error), "the system gave no shared memory for the game's RAM (%s)",
                 strerror(error));
        return -1;
    }
    result = map_at(MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0xa0000000u, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0x00010000u, MEMORIES_GUEST_RAM_SIZE - 0x10000u, fd, 0x10000) ||
             map_at(MEMORIES_GUEST_SCRATCHPAD, 0x1000, fd, MEMORIES_GUEST_RAM_SIZE);
    /* The retail view where the host allows it (image.h). */
#ifdef MEMORIES_TEST_HOOKS
    if (!result) hold_retail_scratchpad();
#endif
    if (!result) {
        void *wanted = (void *)(uintptr_t)MEMORIES_GUEST_SCRATCHPAD_RETAIL;
        void *got = Memories_MapFixed(wanted, 0x1000, view_protection, MAP_FIXED_NOREPLACE | MAP_SHARED, fd,
                                      MEMORIES_GUEST_RAM_SIZE);
        Memories_ScratchpadRetailView = got == wanted;
        if (!Memories_ScratchpadRetailView) {
            fprintf(stderr, "memories-pc: 0x%08x is taken here; the scratchpad is reached at 0x%08x\n",
                    MEMORIES_GUEST_SCRATCHPAD_RETAIL, MEMORIES_GUEST_SCRATCHPAD);
        }
    }
    low_memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (low_memory == MAP_FAILED) {
        low_memory = NULL;
    }
    close(fd);
#if defined(__aarch64__)
    void Memories_ReserveModCode(void);
    /* Native function addresses go into 4-byte guest slots: the game must
     * be where it was linked (android_loader.c), below 4 GB. */
    if ((uintptr_t)Memories_GuestMap >= 0x100000000ull) {
        fprintf(stderr, "memories-pc: the game's code is at %p, above 4 GB; the 64-bit game needs its link address\n",
                (void *)(uintptr_t)Memories_GuestMap);
        if (!map_error[0])
            snprintf(map_error, sizeof(map_error), "the game's code was loaded at %p, above 4 GB, not where it was built for",
                     (void *)(uintptr_t)Memories_GuestMap);
        result = -1;
    }
    if (!result) Memories_ReserveModCode();
#endif
    return result ? -1 : 0;
}

#if defined(__aarch64__)
/* Code mods' images (src/pc/mods/object_loader.c), as on 64-bit Windows:
 * below 4 GB with bit 30 set, so that a mod's function fits a 4-byte guest
 * slot and the branches between it and the game reach. The range is the
 * 64 MiB right after the game's own (libgame.so at 0xC0000000, in the
 * 64 MiB its loader reserves: build_game32.py's ANDROID_GAME_BASE and
 * ANDROID_GAME_SPAN), held here as an inaccessible mapping from the start,
 * before anything else in the process can take it; the loader maps each
 * image over a piece of it. */
#include <dlfcn.h>
#define MOD_CODE_GAME_SPAN 0x04000000u
#define MOD_CODE_SIZE 0x04000000u
static uintptr_t mod_code_start, mod_code_end;

int Memories_ModCodeRange(uintptr_t *start, uintptr_t *end)
{
    if (!mod_code_end) return 0;
    *start = mod_code_start;
    *end = mod_code_end;
    return 2;   /* held: the loader maps over it */
}

void Memories_ReserveModCode(void)
{
    Dl_info info;
    uintptr_t start;
    void *got;
    if (!dladdr((void *)Memories_GuestMap, &info) || !info.dli_fbase) return;
    start = (uintptr_t)info.dli_fbase + MOD_CODE_GAME_SPAN;
    if (start + MOD_CODE_SIZE > 0x100000000ull) return;
    got = Memories_MapFixed((void *)start, MOD_CODE_SIZE, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != (void *)start) {
        fprintf(stderr, "memories-pc: the code mods' range at %p is taken; no code mod can be loaded\n",
                (void *)start);
        say_occupants((uint32_t)start, (uint64_t)start + MOD_CODE_SIZE, 0);
        return;
    }
    mod_code_start = start;
    mod_code_end = start + MOD_CODE_SIZE;
}
#endif
#endif /* _WIN32 */

int Memories_GuestLoadExeData(const unsigned char *data, size_t length, const char *name)
{
    MemoriesExeImage image;
    /* Fixed backends historically accept data-only images without an entry. */
    if (Memories_ParseExe(data, length, 0, &image)) {
        fprintf(stderr, "%s: invalid or truncated PS-X EXE\n", name);
        return -1;
    }
    memcpy((void *)(uintptr_t)image.address, image.data, image.size);
    return 0;
}

typedef struct StubCount { const char *name; unsigned count; } StubCount;
static StubCount stub_calls[512];
static unsigned stub_call_count;

static int compare_stub_counts(const void *left, const void *right)
{
    const StubCount *a = left, *b = right;
    return a->count < b->count ? 1 : a->count > b->count ? -1 : strcmp(a->name, b->name);
}

static void print_stub_summary(void)
{
    unsigned at;
    qsort(stub_calls, stub_call_count, sizeof(stub_calls[0]), compare_stub_counts);
    for (at = 0; at < stub_call_count; at++) {
        LOG(LOG_STUB, "%s: %u calls", stub_calls[at].name, stub_calls[at].count);
    }
    Log_Drain();
}

void Memories_Unimplemented(const char *name)
{
    static int registered;
    const char *break_name = getenv("MEMORIES_STUB_BREAK");
    unsigned i;
#ifdef _WIN32
    if (break_name && !strcmp(break_name, name)) DebugBreak();
#else
    if (break_name && !strcmp(break_name, name)) raise(SIGTRAP);
#endif
    /* Survey aid only: results after the first line are not meaningful,
     * because the missing routine returned garbage. */
    if (getenv("MEMORIES_STUB_TRACE")) {
        for (i = 0; i < stub_call_count && strcmp(stub_calls[i].name, name); i++) {}
        if (i == stub_call_count && stub_call_count < sizeof(stub_calls) / sizeof(stub_calls[0])) {
            stub_calls[stub_call_count].name = name;
            stub_calls[stub_call_count++].count = 0;
        }
        if (i < stub_call_count) stub_calls[i].count++;
        if (!registered) {
            registered = 1;
            Log_Enable(LOG_STUB, 1);
            atexit(print_stub_summary);
        }
        LOG(LOG_STUB, "%s", name);
        return;
    }
    fflush(stdout);
    Crash_ReportFatal("unimplemented routine", name);
    Profile_Flush();
    _exit(70); /* not exit(): atexit handlers could re-enter game code */
}
