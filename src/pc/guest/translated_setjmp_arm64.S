#include "pc/guest/arm64_asm.h"
/* Tail calls preserve the game's caller frame for the real native setjmp.
 * Full-width registers live in a host shadow, never in the 48-byte guest buf. */
.text
.p2align 2
.globl SYM(Psx_setjmp)
SYM(Psx_setjmp):
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl SYM(GuestRuntime_JumpBuffer)
    ldp x29, x30, [sp], #16
    b SYM(Memories_Arm64Capture)
.p2align 2
.globl SYM(Psx_longjmp)
SYM(Psx_longjmp):
    stp x29, x30, [sp, #-32]!
    str x1, [sp, #16]
    mov x29, sp
    bl SYM(GuestRuntime_JumpBufferForRestore)
    ldr x1, [sp, #16]
    ldp x29, x30, [sp], #32
    b SYM(Memories_Arm64Restore)
#ifndef __APPLE__
.section .note.GNU-stack,"",%progbits
#endif
