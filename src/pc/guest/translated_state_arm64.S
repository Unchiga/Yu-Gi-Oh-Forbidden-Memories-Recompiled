#include "pc/guest/arm64_asm.h"
/* Offsets are checked by the native context test. Capture owns no stack
 * frame: the recorded SP and LR describe the C caller's continuation. */
.text
.p2align 2
.globl SYM(Memories_Arm64Capture)
SYM(Memories_Arm64Capture):
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

.p2align 2
.globl SYM(Memories_Arm64Restore)
SYM(Memories_Arm64Restore):
    mov x9, x0
    ldp x19, x20, [x9, #0]
    ldp x21, x22, [x9, #16]
    ldp x23, x24, [x9, #32]
    ldp x25, x26, [x9, #48]
    ldp x27, x28, [x9, #64]
    ldp x29, x30, [x9, #80]
    ldr x10, [x9, #96]
    ldp d8, d9, [x9, #104]
    ldp d10, d11, [x9, #120]
    ldp d12, d13, [x9, #136]
    ldp d14, d15, [x9, #152]
    mov sp, x10
    mov w10, #1
    cmp w1, #0
    csel w0, w10, w1, eq
    ret

#ifdef MEMORIES_TRANSLATED
/* Record the game's caller before native presentation creates its frames.
 * x0 is the VSync mode and must survive this tail bridge unchanged. */
.p2align 2
.globl SYM(VSync)
SYM(VSync):
    adrp x9, PAGE(Memories_Arm64Boundary)
    add x9, x9, PAGEOFF(Memories_Arm64Boundary)
    stp x19, x20, [x9, #0]
    stp x21, x22, [x9, #16]
    stp x23, x24, [x9, #32]
    stp x25, x26, [x9, #48]
    stp x27, x28, [x9, #64]
    stp x29, x30, [x9, #80]
    mov x10, sp
    str x10, [x9, #96]
    stp d8, d9, [x9, #104]
    stp d10, d11, [x9, #120]
    stp d12, d13, [x9, #136]
    stp d14, d15, [x9, #152]
    b SYM(Memories_VSync)
#endif
#ifndef __APPLE__
/* void Memories_CallOnStack(void (*function)(void *), void *argument,
 * uintptr_t top): function(argument) with the stack pointer at `top`, then
 * back (translated_state_backend.c's Memories_OnHostStack, Android). The
 * frame record stays on the caller's stack. */
.text
.p2align 2
.globl Memories_CallOnStack
.type Memories_CallOnStack, %function
Memories_CallOnStack:
    stp x29, x30, [sp, #-32]!
    str x19, [sp, #16]
    mov x29, sp
    mov x19, sp
    and x2, x2, #0xfffffffffffffff0
    mov sp, x2
    mov x16, x0
    mov x0, x1
    blr x16
    mov sp, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.section .note.GNU-stack,"",%progbits
#endif
