/* The C library a code mod may call: a short, fixed list the host lends it
 * (exports.h). A mod is one object file for both systems, built without any
 * system headers (notes/modding.md), so it cannot link against glibc or the
 * Windows C runtime itself; the names below resolve to whichever one this
 * executable was linked with. They are the functions whose behaviour the two
 * agree on. Files are only ever opened by the host (open_asset, open_data),
 * so a mod gets the calls that use a FILE and not fopen.
 *
 * The compiler's own helpers are here too: 32-bit x86 code does 64-bit
 * division and some conversions by calling them, and both toolchains
 * (libgcc, compiler-rt) have the same ones under the same names; 64-bit
 * Windows code probes a stack frame over 4 KiB with ___chkstk_ms. So are
 * the game's indirect-branch thunks, which build_mod.py has every indirect
 * call of a mod go through (src/pc/guest/branch_thunks.c): seven registers
 * on i386, r11 alone on x86-64 (clang's retpoline uses no other), and on
 * AArch64 the SLS thunks of x0-x29 but x18, which a mod's own weak copies
 * are bound to.
 *
 * Adding a name here is a promise to every mod built afterwards; removing one
 * breaks the mods that use it. The SDK's headers (sdk/include) declare
 * exactly this list. */
#include "exports.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*Function)(void);

#if defined(__i386__)
/* The compiler's helpers have no header, and only 32-bit x86 has these. */
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern unsigned long long __udivmoddi4(unsigned long long, unsigned long long, unsigned long long *);
extern long long __divmoddi4(long long, long long, long long *);
extern long long __fixdfdi(double);
extern long long __fixsfdi(float);
extern unsigned long long __fixunsdfdi(double);
extern unsigned long long __fixunssfdi(float);
extern double __floatdidf(long long);
extern float __floatdisf(long long);
extern double __floatundidf(unsigned long long);
extern float __floatundisf(unsigned long long);
extern void __x86_indirect_thunk_eax(void);
extern void __x86_indirect_thunk_ebp(void);
extern void __x86_indirect_thunk_ebx(void);
extern void __x86_indirect_thunk_ecx(void);
extern void __x86_indirect_thunk_edi(void);
extern void __x86_indirect_thunk_edx(void);
extern void __x86_indirect_thunk_esi(void);
#elif defined(__x86_64__) && defined(_WIN32)
extern void ___chkstk_ms(void);
extern void __x86_indirect_thunk_r11(void);
#elif defined(__aarch64__) && !defined(__APPLE__) && !defined(MEMORIES_TRANSLATED)   /* the macOS game links dylibs */
/* The game's SLS thunks (branch_thunks.c), for the weak copies clang puts
 * in each AArch64 unit of a mod (object_loader.c binds those to these). */
#define T(n) extern void __llvm_slsblr_thunk_x##n(void);
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T(10) T(11) T(12) T(13) T(14) T(15) T(16) T(17)
T(19) T(20) T(21) T(22) T(23) T(24) T(25) T(26) T(27) T(28) T(29)
#undef T
#endif

/* rand as the C standard's own example has it, so a mod draws the same
 * numbers on both systems (glibc's and the Windows runtime's differ). One
 * sequence for all mods; the game's own random numbers are separate. The
 * seed is 32 bits on every target (the C standard's `unsigned long` is 64 on
 * LP64), which is what the save states' "mod-rng" chunk has always held. */
static uint32_t mod_seed = 1;
static int mod_rand(void)
{
    mod_seed = mod_seed * 1103515245u + 12345u;
    return (int)(mod_seed / 65536u % 32768u);
}
static void mod_srand(unsigned seed) { mod_seed = seed; }
#if defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
/* Direct imports from translated dylibs use the mod RNG, not the game RNG. */
int Mods_ModRand(void) { return mod_rand(); }
void Mods_ModSrand(unsigned seed) { mod_srand(seed); }
#endif
/* For save states (state.c's "mod-rng" chunk): a mod's draws after a load
 * follow the game that saved, as the game's own do (the "rng" chunk). */
void *Mods_RandSeed(unsigned *size)
{
    *size = (unsigned)sizeof(mod_seed);
    return &mod_seed;
}

#define F(name) {#name, (Function)name}
#define AS(name, function) {#name, (Function)function}

/* Sorted by name (strcmp): Mods_Lookup searches it. */
static const struct { const char *name; Function function; } functions[] = {
#if defined(__i386__)
    F(__divdi3), F(__divmoddi4), F(__fixdfdi), F(__fixsfdi), F(__fixunsdfdi), F(__fixunssfdi),
    F(__floatdidf), F(__floatdisf), F(__floatundidf), F(__floatundisf), F(__moddi3), F(__udivdi3),
    F(__udivmoddi4), F(__umoddi3), F(__x86_indirect_thunk_eax), F(__x86_indirect_thunk_ebp),
    F(__x86_indirect_thunk_ebx), F(__x86_indirect_thunk_ecx), F(__x86_indirect_thunk_edi),
    F(__x86_indirect_thunk_edx), F(__x86_indirect_thunk_esi),
#elif defined(__x86_64__) && defined(_WIN32)
    F(___chkstk_ms), F(__x86_indirect_thunk_r11),
#elif defined(__aarch64__) && !defined(__APPLE__) && !defined(MEMORIES_TRANSLATED)   /* the macOS game links dylibs */
    F(__llvm_slsblr_thunk_x0), F(__llvm_slsblr_thunk_x1), F(__llvm_slsblr_thunk_x10), F(__llvm_slsblr_thunk_x11),
    F(__llvm_slsblr_thunk_x12), F(__llvm_slsblr_thunk_x13), F(__llvm_slsblr_thunk_x14), F(__llvm_slsblr_thunk_x15),
    F(__llvm_slsblr_thunk_x16), F(__llvm_slsblr_thunk_x17), F(__llvm_slsblr_thunk_x19), F(__llvm_slsblr_thunk_x2),
    F(__llvm_slsblr_thunk_x20), F(__llvm_slsblr_thunk_x21), F(__llvm_slsblr_thunk_x22), F(__llvm_slsblr_thunk_x23),
    F(__llvm_slsblr_thunk_x24), F(__llvm_slsblr_thunk_x25), F(__llvm_slsblr_thunk_x26), F(__llvm_slsblr_thunk_x27),
    F(__llvm_slsblr_thunk_x28), F(__llvm_slsblr_thunk_x29), F(__llvm_slsblr_thunk_x3), F(__llvm_slsblr_thunk_x4),
    F(__llvm_slsblr_thunk_x5), F(__llvm_slsblr_thunk_x6), F(__llvm_slsblr_thunk_x7), F(__llvm_slsblr_thunk_x8),
    F(__llvm_slsblr_thunk_x9),
#endif
    F(abs), F(acos), F(asin), F(atan), F(atan2), F(atan2f), F(atoi), F(bsearch), F(calloc), F(ceil),
    F(ceilf), F(cos), F(cosf), F(exp), F(expf), F(fabs), F(fabsf), F(fclose), F(fgets), F(floor),
    F(floorf), F(fmod), F(fmodf), F(fread), F(free), F(fseek), F(ftell), F(fwrite), F(labs), F(log),
    F(log10), F(logf), F(malloc), F(memchr), F(memcmp), F(memcpy), F(memmove), F(memset), F(pow),
    F(powf), F(qsort), AS(rand, mod_rand), F(realloc), F(sin), F(sinf), F(snprintf), F(sqrt),
    F(sqrtf), AS(srand, mod_srand), F(strcat), F(strchr), F(strcmp), F(strcpy), F(strlen),
    F(strncat), F(strncmp), F(strncpy), F(strrchr), F(strstr), F(strtod), F(strtol), F(strtoul),
    F(tan), F(tanf), F(vsnprintf),
};

Function Mods_LibcLookup(const char *name)
{
    unsigned low = 0, high = sizeof(functions) / sizeof(functions[0]);
    while (low < high) {
        unsigned middle = (low + high) / 2;
        int order = strcmp(name, functions[middle].name);
        if (!order) return functions[middle].function;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

int Mods_LibcSorted(void)
{
    unsigned i;
    for (i = 1; i < sizeof(functions) / sizeof(functions[0]); i++) {
        if (strcmp(functions[i - 1].name, functions[i].name) >= 0) return 0;
    }
    return 1;
}
