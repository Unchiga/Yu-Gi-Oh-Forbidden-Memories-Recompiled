/* Function hooks (mod API 4): a code mod replaces or wraps one of the game's
 * own functions, by address, and gets the function it displaced back to call.
 *
 * Every game unit is compiled with -fpatchable-function-entry=8,6
 * (tools/pc/build_game32.py): six bytes of nops before the function's entry
 * and two at it. A hooked function's six bytes become `jmp *slot`, an
 * indirect jump through a pointer kept here, and its two become `jmp -8`,
 * back into them. The six are FF 25 and four bytes on both x86 widths: on
 * i386 the slot's address (`jmp *[abs32]`), on x86-64 its distance from the
 * entry (`jmp *[rip+disp32]`, rip being the entry), which reaches it because
 * the slots are in this executable's own image, as the function is. The
 * two-byte store is the only one made to code a thread may be running, and
 * it is one aligned-enough write; everything after that changes only `slot`
 * and the mods' `original` pointers, which are words.
 *
 * AArch64 (the arm64 game, -fpatchable-function-entry=4,3): three nops
 * before the entry and one at it. The three become `adrp x16, slot; ldr x16,
 * [x16, :lo12:slot]; br x16` (the slot within 4 GB: it is in the game's own
 * image), the entry `b .-12` and back to `nop`. NOP and B are among the
 * instructions the architecture lets one core change while another runs
 * them, and the entry is one aligned word; x16 (IP0) may be clobbered at a
 * call. Every write is followed by cache maintenance. The game's text is a
 * file mapping: where the system refuses to make it writable (EACCES, an
 * SELinux execmod denial), it is copied once into anonymous memory moved
 * over the same range, and patched there (anonymize_text below); the log
 * says which way hooks were written.
 *
 * Hooks on one function chain in the order they were made: the one made
 * last is called first, and its `original` leads to the one before, down to
 * the game's own code. Only applied mods are in a chain; Hooks_Relink
 * rebuilds every chain when a mod is applied or removed. A function no
 * applied mod hooks gets its two nops back. */
#define _GNU_SOURCE   /* mremap */
#include "pc/compat/fs.h"   /* getenv: the UTF-8 boundary */
#include "hooks.h"
#include "mods.h"
#include "../compat/mman.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__) && defined(__linux__)
#include <link.h>
#endif
#if defined(__aarch64__) && !defined(_WIN32)
#include <unistd.h>   /* sysconf */
#endif

/* The translated game (macOS, Android arm64) registers each hookable
 * function's body instead (Hooks_Register); the fixed arm64 game
 * (build_game32.py's) is patched. */
#if defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
#define ARM64_HOOKS 1
static struct { void *function, *body; } registered[8192];
static unsigned registered_count;
void Hooks_Register(void *function, void *body)
{
    for (unsigned i = 0; i < registered_count; i++)
        if (registered[i].function == function) return;
    if (registered_count == 8192) abort();
    registered[registered_count].function = function;
    registered[registered_count++].body = body;
}
#elif defined(__aarch64__) && defined(__linux__)
#define A64_PATCH 1
#endif

#if defined(A64_PATCH)
#define PRE 12    /* the three nop words before the entry */
#define ENTRY 4   /* the one at it */
#define A64_NOP 0xD503201Fu
#define A64_B_BACK3 0x17FFFFFDu   /* b .-12 */
#else
#define PRE 6     /* nops before the entry */
#define ENTRY 2
#endif
#define TARGETS_MAX 1024
#define HOOKS_MAX 2048

typedef struct {
    unsigned char *entry;
    unsigned char saved[ENTRY];   /* the entry's own nop(s) */
    void *volatile slot;     /* where `jmp *slot` goes */
    int patched;
} Target;

typedef struct {
    int owner, token, target;
    void *replacement;
    void **original;
} Hook;

static Target targets[TARGETS_MAX];
static int target_count;
static Hook hooks[HOOKS_MAX];
static int hook_count, serial;

/* The system's page, which mprotect works in: 4 KiB on x86, and 4, 16 or
 * 64 KiB on AArch64 (Android 15 phones can run 16 KiB ones). */
static uintptr_t page_size(void)
{
#if defined(__aarch64__) && !defined(_WIN32)
    static uintptr_t page;
    if (!page) {
        long system = sysconf(_SC_PAGESIZE);
        page = system > 0 ? (uintptr_t)system : 4096;
    }
    return page;
#else
    return 4096;
#endif
}

#if defined(A64_PATCH)
/* The game's text, copied into anonymous memory moved over the same range,
 * where writing it is an anonymous mapping's execmem and not a file's
 * execmod (the one an app's SELinux policy may refuse). Done once, when
 * the first write in place is refused; the bytes do not change. */
static uintptr_t text_start, text_end;
static int find_text(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        uintptr_t start = info->dlpi_addr + phdr->p_vaddr;
        if (phdr->p_type != PT_LOAD || !(phdr->p_flags & PF_X)) continue;
        if ((uintptr_t)data >= start && (uintptr_t)data < start + phdr->p_memsz) {
            uintptr_t page = page_size();
            text_start = start & ~(page - 1);
            text_end = (start + phdr->p_memsz + page - 1) & ~(page - 1);
        }
    }
    return 0;
}

static int anonymize_text(void *inside)
{
    static int tried;   /* once: a copy that failed fails again, and each would cost the text's size */
    void *copy, *moved;
    size_t length;
    if (tried) return 0;
    tried = 1;
    dl_iterate_phdr(find_text, inside);
    if (!text_end) return 0;
    length = text_end - text_start;
    copy = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) return 0;
    memcpy(copy, (void *)text_start, length);
    if (mprotect(copy, length, PROT_READ | PROT_EXEC)) {
        munmap(copy, length);
        return 0;
    }
    __builtin___clear_cache((char *)copy, (char *)copy + length);
    /* Linux unmaps the target before it moves the copy there, so a move
     * that fails then (out of memory) has taken the text with it, this
     * code included; it fails before that in practice (EINVAL, EFAULT). */
    moved = mremap(copy, length, length, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)text_start);
    if (moved != (void *)text_start) {
        munmap(copy, length);
        return 0;
    }
    return 1;
}

static void flush(unsigned char *from, size_t size) { __builtin___clear_cache((char *)from, (char *)from + size); }
#else
static void flush(unsigned char *from, size_t size) { (void)from; (void)size; }
#endif

/* 0 not yet said, 1 in place, 2 in the anonymous copy of the text. */
static int hook_path;

static int protect(unsigned char *from, size_t size, int on)
{
    uintptr_t page = page_size(), start = (uintptr_t)from & ~(page - 1), end = ((uintptr_t)from + size + page - 1) & ~(page - 1);
    return mprotect((void *)start, end - start, on ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC);
}

/* MEMORIES_TEST_ANON_TEXT=1 (builds that are not releases): take the first
 * write in place as refused, so the anonymous copy is tested where the
 * system allows the write (the emulator, a shell process). */
static int refuse_in_place(void)
{
#if defined(MEMORIES_TEST_HOOKS) && defined(A64_PATCH)
    const char *value = getenv("MEMORIES_TEST_ANON_TEXT");
    if (hook_path != 2 && value && *value && strcmp(value, "0")) {
        errno = EACCES;
        return 1;
    }
#endif
    return 0;
}

static int writable(unsigned char *from, size_t size, int on)
{
    if (!(on && refuse_in_place()) && !protect(from, size, on)) {
        if (on && !hook_path) {
            hook_path = 1;
#if defined(A64_PATCH)
            /* The arm64 game's two ways (above); the others have one. */
            fprintf(stderr, "memories-pc: hooks: the game's code is patched in place\n");
#endif
        }
        return 1;
    }
#if defined(A64_PATCH)
    if (on && errno == EACCES && hook_path != 2) {
        int was = errno;
        if (anonymize_text(from) && !protect(from, size, on)) {
            hook_path = 2;
            fprintf(stderr, "memories-pc: hooks: writing the game's code in place was refused (%s); it is patched "
                            "in an anonymous copy of the text\n", strerror(was));
            return 1;
        }
        fprintf(stderr, "memories-pc: hooks: the game's code cannot be made writable (%s), nor copied\n",
                strerror(was));
    }
#endif
    return 0;
}

#ifndef ARM64_HOOKS /* the translated game patches nothing (find_target) */
/* The six bytes before, and the two at, the entry a patchable function has.
 * GCC writes single-byte nops; clang writes 66 90 at the entry. */
static int patchable(const unsigned char *entry)
{
#if defined(A64_PATCH)
    uint32_t word;
    for (int i = -PRE; i <= 0; i += 4) {
        memcpy(&word, entry + i, 4);
        if (word != A64_NOP) return 0;
    }
    return ((uintptr_t)entry & 3) == 0;
#elif defined(__i386__) || defined(__x86_64__)
    for (int i = -PRE; i < 0; i++) if (entry[i] != 0x90) return 0;
    return (entry[0] == 0x90 && entry[1] == 0x90) || (entry[0] == 0x66 && entry[1] == 0x90);
#else
    (void)entry;
    return 0;
#endif
}
#endif

static int find_target(unsigned char *entry)
{
    int i;
    for (i = 0; i < target_count; i++) if (targets[i].entry == entry) return i;
#ifdef ARM64_HOOKS
    if (target_count == TARGETS_MAX) return -1;
    for (unsigned r = 0; r < registered_count; r++) if (registered[r].function == entry) {
        targets[i].entry = entry;
        targets[i].slot = registered[r].body;
        return target_count++;
    }
    return -1;
#else
    if (target_count == TARGETS_MAX || !patchable(entry)) return -1;
    targets[i].entry = entry;
    memcpy(targets[i].saved, entry, ENTRY);
    targets[i].slot = entry + ENTRY;
    targets[i].patched = 0;
    /* The jump before the entry is written once and never changes: until
     * the entry points at it, nothing runs it. */
    {
#if defined(A64_PATCH)
        uintptr_t pc = (uintptr_t)(entry - PRE), to = (uintptr_t)&targets[i].slot;
        int64_t page = ((int64_t)(to & ~(uintptr_t)0xfff) - (int64_t)(pc & ~(uintptr_t)0xfff)) >> 12;
        uint32_t code[3];
        if ((to & 7) || page < -(1 << 20) || page >= (1 << 20)) return -1;   /* never in the game: both in its image */
        code[0] = 0x90000010u | (((uint32_t)page & 3u) << 29) | ((((uint32_t)page >> 2) & 0x7ffffu) << 5);  /* adrp x16 */
        code[1] = 0xF9400210u | ((uint32_t)((to & 0xfff) >> 3) << 10);                                      /* ldr x16 */
        code[2] = 0xD61F0200u;                                                                                /* br x16 */
        if (!writable(entry - PRE, PRE + ENTRY, 1)) return -1;
        memcpy(entry - PRE, code, sizeof(code));
        flush(entry - PRE, PRE);
    }
#else
#if defined(__x86_64__)
        intptr_t distance = (intptr_t)&targets[i].slot - (intptr_t)entry;
        int32_t slot = (int32_t)distance;
        if (distance != slot) return -1;   /* never in the game: both are in its image */
#else
        uint32_t slot = (uint32_t)(uintptr_t)&targets[i].slot;
#endif
        if (!writable(entry - PRE, PRE + ENTRY, 1)) return -1;
        entry[-6] = 0xFF;
        entry[-5] = 0x25;   /* jmp *[slot] */
        memcpy(entry - 4, &slot, 4);
    }
#endif
    writable(entry - PRE, PRE + ENTRY, 0);
    return target_count++;
#endif
}

static void set_entry(Target *target, int hooked)
{
#if defined(ARM64_HOOKS)
    target->patched = hooked;
    return;
#elif defined(A64_PATCH)
    uint32_t value;
    if (hooked == target->patched) return;
    if (hooked) value = A64_B_BACK3;   /* b .-12: back to the adrp */
    else memcpy(&value, target->saved, 4);
    if (!writable(target->entry, ENTRY, 1)) return;
    __atomic_store_n((uint32_t *)target->entry, value, __ATOMIC_SEQ_CST);
#else
    uint16_t value;
    if (hooked == target->patched) return;
    if (hooked) value = (uint16_t)(0xEB | (0xF8 << 8));   /* jmp -8: back to the jmp *[slot] */
    else memcpy(&value, target->saved, 2);
    if (!writable(target->entry, ENTRY, 1)) return;
    __atomic_store_n((uint16_t *)target->entry, value, __ATOMIC_SEQ_CST);
#endif
    flush(target->entry, ENTRY);
    writable(target->entry, ENTRY, 0);
    target->patched = hooked;
}

/* The game's own code: after the nop(s), or the body registered. */
static void *original_body(Target *target)
{
#ifdef ARM64_HOOKS
    for (unsigned r = 0; r < registered_count; r++)
        if (registered[r].function == target->entry) return registered[r].body;
    abort();
#else
    return target->entry + ENTRY;
#endif
}

#ifdef ARM64_HOOKS
void *Hooks_Resolve(void *function, void *body)
{
    for (int t = 0; t < target_count; t++) if (targets[t].entry == function)
        return __atomic_load_n(&targets[t].slot, __ATOMIC_SEQ_CST);
    return body;
}
#endif

void Hooks_Relink(void)
{
    for (int t = 0; t < target_count; t++) {
        void *body = original_body(&targets[t]);
        void *next = body;
        for (int h = 0; h < hook_count; h++) {
            if (hooks[h].target != t || !Mods_Active(hooks[h].owner)) continue;
            if (hooks[h].original) __atomic_store_n(hooks[h].original, next, __ATOMIC_SEQ_CST);
            next = hooks[h].replacement;
        }
        /* An unapplied mod's `original` still leads somewhere real. */
        for (int h = 0; h < hook_count; h++) {
            if (hooks[h].target == t && !Mods_Active(hooks[h].owner) && hooks[h].original)
                __atomic_store_n(hooks[h].original, body, __ATOMIC_SEQ_CST);
        }
        __atomic_store_n(&targets[t].slot, next, __ATOMIC_SEQ_CST);
        set_entry(&targets[t], next != body);
    }
}

int Hooks_Add(int owner, void *function, void *replacement, void **original)
{
    int target;
    if (!function || !replacement || function == replacement || hook_count == HOOKS_MAX || serial == 0x7fffffff) return 0;
    target = find_target(function);
    if (target < 0) return 0;
    if (original) *original = original_body(&targets[target]);
    hooks[hook_count++] = (Hook){owner, ++serial, target, replacement, original};
    Hooks_Relink();
    return serial;
}

void Hooks_Remove(int owner, int token)
{
    for (int i = 0; i < hook_count; i++) {
        if (hooks[i].owner != owner || hooks[i].token != token) continue;
        memmove(hooks + i, hooks + i + 1, (size_t)(--hook_count - i) * sizeof(*hooks));
        Hooks_Relink();
        return;
    }
}

void Hooks_Clear(int owner)
{
    int kept = 0;
    for (int i = 0; i < hook_count; i++) if (hooks[i].owner != owner) hooks[kept++] = hooks[i];
    if (kept == hook_count) return;
    hook_count = kept;
    Hooks_Relink();
}

int Hooks_At(int index, int *owner, const void **function)
{
    if (index < 0 || index >= hook_count) return 0;
    *owner = hooks[index].owner;
    *function = targets[hooks[index].target].entry;
    return 1;
}

int Hooks_IsHooked(const void *function)
{
    int i;
    for (i = 0; i < target_count; i++)
        if (targets[i].entry == function) return targets[i].patched;
    return 0;
}
