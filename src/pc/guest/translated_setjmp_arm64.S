/* Tail calls preserve the game's caller frame for the real native setjmp.
 * Full-width registers live in a host shadow, never in the 48-byte guest buf. */
#include "asm_arm64.h"
.text
.p2align 2
FUNCTION(Psx_setjmp)
C_NAME(Psx_setjmp):
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl C_NAME(GuestRuntime_JumpBuffer)
    ldp x29, x30, [sp], #16
    b C_NAME(Memories_Arm64Capture)
.p2align 2
FUNCTION(Psx_longjmp)
C_NAME(Psx_longjmp):
    stp x29, x30, [sp, #-32]!
    str x1, [sp, #16]
    mov x29, sp
    bl C_NAME(GuestRuntime_JumpBufferForRestore)
    ldr x1, [sp, #16]
    ldp x29, x30, [sp], #32
    b C_NAME(Memories_Arm64Restore)
