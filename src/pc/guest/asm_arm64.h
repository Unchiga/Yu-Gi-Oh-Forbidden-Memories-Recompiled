/* C symbol names and page addressing in the translated build's ARM64
 * assembly (translated_*_arm64.S), for both object formats it is built in:
 * Mach-O (macOS) prefixes C names with an underscore and spells a page as
 * @PAGE/@PAGEOFF; ELF (the Switch) takes the name as it is and :lo12:. */
#ifndef MEMORIES_PC_GUEST_ASM_ARM64_H
#define MEMORIES_PC_GUEST_ASM_ARM64_H
#ifdef __APPLE__
#define C_NAME(name) _##name
#define PAGE(name) _##name@PAGE
#define PAGE_OFFSET(name) _##name@PAGEOFF
#define FUNCTION(name) .globl _##name
#else
#define C_NAME(name) name
#define PAGE(name) name
#define PAGE_OFFSET(name) :lo12:name
#define FUNCTION(name) .globl name; .type name, %function
#endif
#endif
