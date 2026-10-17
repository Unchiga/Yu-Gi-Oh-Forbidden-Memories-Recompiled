/* Symbol spelling for the translated ARM64 assembly on both object formats:
 * Mach-O (macOS) prefixes C names with an underscore and addresses a page
 * with @PAGE/@PAGEOFF; ELF (Android) has neither and uses :lo12:. */
#ifndef MEMORIES_PC_GUEST_ARM64_ASM_H
#define MEMORIES_PC_GUEST_ARM64_ASM_H
#ifdef __APPLE__
#define SYM(name) _##name
#define PAGE(name) _##name@PAGE
#define PAGEOFF(name) _##name@PAGEOFF
#else
#define SYM(name) name
#define PAGE(name) name
#define PAGEOFF(name) :lo12:name
#endif
#endif
