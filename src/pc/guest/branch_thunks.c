/* Indirect calls and jumps that may land in guest code, without DEP.
 *
 * Tables in the retail data image hold MIPS function addresses (the text
 * opcode handlers, callbacks an overlay installs), and native code calls
 * through them. Every unit the build compiles sends its indirect calls and
 * jumps through the thunks below: clang's -mretpoline-external-thunk and
 * GCC's -mindirect-branch=thunk-extern -mindirect-branch-register load the
 * target into a register and call or jump to __x86_indirect_thunk_<register>
 * (tools/pc/build_game32.py, tools/pc/build_mod.py). A target in guest memory
 * goes to Memories_GuestBranchResolver (image.c: the native function of that
 * address, or the MIPS interpreter's entry for an overlay callback), anything
 * else is jumped to as it is. A module function that C calls by name is
 * pinned to its guest address, so that call is direct and no thunk sees it:
 * the build gives each such name a host stub that pushes the address and
 * enters Memories_GuestBranchDirect below, which resolves it the same way.
 *
 * Before these, the only way in was the fault of executing guest RAM, which
 * needs DEP for the process: with DEP off, the MIPS bytes of a handler ran as
 * x86 code (the title's Options: the text handler at 0x80038b4c jumped to
 * 0x902b4950). The fault handler in image.c stays as the second net.
 *
 * The contract, for the compilers' code and for mods built by build_mod.py:
 * every register is kept, the target register too (a compiler may pick a
 * callee-saved one and keep it across the call), and the stack is as it was,
 * the caller's return address on top for a call, so a native function (or a
 * reference build's Memories_MipsThunk) starts exactly as it would from a direct call (or from
 * the fault handler's redirect). Flags are not kept: they are dead at an
 * indirect call or jump. Nothing is stored below the stack pointer, where the
 * Windows interrupt clock pushes while the thread is suspended.
 *
 * Fast path: one test. Bits 21-28 and 30 are clear in every guest range
 * (physical 0x10000..0x200000, 0x80000000..0x80200000, 0xA0000000..
 * 0xA0200000); executable code never has them all clear, and the few host
 * addresses that do (0x20000000..0x20200000) come back from the resolver
 * unchanged. Slow path: a copy of the return address becomes the slot the
 * final `ret` takes the destination from (and what a stack walk reads as
 * the frame's return address), eax/ecx/edx/flags are saved, the resolver is
 * called on a 16-byte aligned stack, and everything is restored. */
#include "image.h"

void *(*Memories_GuestBranchResolver)(unsigned address);

#if defined(__i386__)
#ifdef _WIN32
#define SYMBOL(name) "_" #name
#define FUNCTION(name) ""
#define END(name) ""
#else
#define SYMBOL(name) #name
#define FUNCTION(name) ".type " #name ", @function\n"
#define END(name) ".size " #name ", . - " #name "\n"
#endif

/* `load` reads the target once eax/ecx/edx are saved and ebp is the frame:
 * the register itself, or for ebp its saved copy. */
#define THUNK(reg, load)                                                                  \
    ".p2align 4\n"                                                                        \
    ".globl " SYMBOL(__x86_indirect_thunk_##reg) "\n"                                     \
    FUNCTION(__x86_indirect_thunk_##reg)                                                  \
    SYMBOL(__x86_indirect_thunk_##reg) ":\n"                                              \
    "    testl $0x5fe00000, %" #reg "\n"                                                  \
    "    jz 1f\n"                                                                         \
    "    jmp *%" #reg "\n"                                                                \
    "1:  pushl (%esp)\n"                                                                  \
    "    pushl %ebp\n"                                                                    \
    "    movl %esp, %ebp\n"                                                               \
    "    pushfl\n"                                                                        \
    "    pushl %eax\n"                                                                    \
    "    pushl %ecx\n"                                                                    \
    "    pushl %edx\n"                                                                    \
    "    movl " load ", %eax\n"                                                           \
    "    jmp memories_branch_resolve\n"                                                   \
    END(__x86_indirect_thunk_##reg)

__asm__(".text\n"
        /* eax: the target. The frame: ebp+4 the slot, ebp-4 flags, then
         * eax, ecx, edx. */
        ".p2align 4\n"
        "memories_branch_resolve:\n"
#if defined(__PIC__) && defined(__ELF__)
        /* Position-independent (Android's shared object): the resolver's
         * address from the GOT; the flags are saved already. */
        "    call 3f\n"
        "3:  popl %ecx\n"
        "    addl $_GLOBAL_OFFSET_TABLE_+(.-3b), %ecx\n"
        "    movl " SYMBOL(Memories_GuestBranchResolver) "@GOT(%ecx), %ecx\n"
        "    movl (%ecx), %ecx\n"
#else
        "    movl " SYMBOL(Memories_GuestBranchResolver) ", %ecx\n"
#endif
        "    testl %ecx, %ecx\n"
        "    jz 2f\n"
        "    andl $-16, %esp\n"
        "    subl $12, %esp\n"
        "    pushl %eax\n"
        "    call *%ecx\n"
        "2:  movl %eax, 4(%ebp)\n"
        "    leal -16(%ebp), %esp\n"
        "    popl %edx\n"
        "    popl %ecx\n"
        "    popl %eax\n"
        "    popfl\n"
        "    popl %ebp\n"
        "    ret\n"
        /* The entry of a host stub the build writes for a module function
         * that C calls by name (tools/pc/build_game32.py, guest_branches.c):
         * the stub pushes the guest address, which becomes the slot. */
        ".p2align 4\n"
        ".globl " SYMBOL(Memories_GuestBranchDirect) "\n"
        FUNCTION(Memories_GuestBranchDirect)
        SYMBOL(Memories_GuestBranchDirect) ":\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    pushfl\n"
        "    pushl %eax\n"
        "    pushl %ecx\n"
        "    pushl %edx\n"
        "    movl 4(%ebp), %eax\n"
        "    jmp memories_branch_resolve\n"
        END(Memories_GuestBranchDirect)
        THUNK(eax, "%eax")
        THUNK(ecx, "%ecx")
        THUNK(edx, "%edx")
        THUNK(ebx, "%ebx")
        THUNK(esi, "%esi")
        THUNK(edi, "%edi")
        THUNK(ebp, "(%ebp)"));
#elif defined(__x86_64__) && defined(_WIN32)
/* The 64-bit Windows build (build_game32.py --target windows-x64). On
 * x86-64, clang's -mretpoline-external-thunk loads every indirect target
 * into r11, so there is one thunk, and r11 is a scratch register in both
 * ABIs: no argument travels in it, and neither a callee nor a caller
 * expects it kept. The flags are dead too. Everything else is kept.
 *
 * Fast path: a target with bits 21-28 or 30 set, as on i386 (the image is
 * at 0x40000000), or with anything in its upper half (system and SDL DLLs
 * sit at 0x7ff...): jumped to. The upper half is tested through the stack,
 * since pop leaves the flags as shr set them.
 *
 * Slow path: as on i386, a copy of the return address becomes the slot the
 * final `ret` takes the destination from. The resolver is C, so the Win64
 * volatile registers that can carry arguments or results (rax, rcx, rdx,
 * r8-r10, xmm0-xmm5) and the flags are saved around it, and it is called on
 * a 16-byte aligned stack with its 32-byte shadow space. */
__asm__(".text\n"
        /* rax: the target. The frame: rbp+8 the slot, rbp-8 the flags, then
         * rax, rcx, rdx, r8, r9, r10 down to rbp-56. */
        ".p2align 4\n"
        "memories_branch_resolve:\n"
        "    andq $-16, %rsp\n"
        "    subq $128, %rsp\n"
        "    movdqu %xmm0, 32(%rsp)\n"
        "    movdqu %xmm1, 48(%rsp)\n"
        "    movdqu %xmm2, 64(%rsp)\n"
        "    movdqu %xmm3, 80(%rsp)\n"
        "    movdqu %xmm4, 96(%rsp)\n"
        "    movdqu %xmm5, 112(%rsp)\n"
        "    movq Memories_GuestBranchResolver(%rip), %r11\n"
        "    testq %r11, %r11\n"
        "    jz 2f\n"
        "    movl %eax, %ecx\n"
        "    call *%r11\n"
        "2:  movq %rax, 8(%rbp)\n"
        "    movdqu 32(%rsp), %xmm0\n"
        "    movdqu 48(%rsp), %xmm1\n"
        "    movdqu 64(%rsp), %xmm2\n"
        "    movdqu 80(%rsp), %xmm3\n"
        "    movdqu 96(%rsp), %xmm4\n"
        "    movdqu 112(%rsp), %xmm5\n"
        "    leaq -56(%rbp), %rsp\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    popq %rbp\n"
        "    ret\n"
        /* The entry of a host stub the build writes for a module function
         * that C calls by name (guest_branches.c): the stub makes the slot
         * and writes the guest address into its low half. */
        ".p2align 4\n"
        ".globl Memories_GuestBranchDirect\n"
        "Memories_GuestBranchDirect:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    movl 8(%rbp), %eax\n"
        "    jmp memories_branch_resolve\n"
        ".p2align 4\n"
        ".globl __x86_indirect_thunk_r11\n"
        "__x86_indirect_thunk_r11:\n"
        "    testl $0x5fe00000, %r11d\n"
        "    jz 1f\n"
        "    jmp *%r11\n"
        "1:  pushq %r11\n"
        "    shrq $32, %r11\n"
        "    popq %r11\n"
        "    jz 3f\n"
        "    jmp *%r11\n"
        "3:  pushq (%rsp)\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    movq %r11, %rax\n"
        "    jmp memories_branch_resolve\n");
#elif defined(__aarch64__)
/* 64-bit ARM (AArch64: Android arm64-v8a, Linux arm64). No compiler routes
 * indirect calls through named thunks as the x86 ones do, but clang's
 * -mharden-sls=blr turns every `blr xN` into `bl
 * __llvm_slsblr_thunk_xN` and emits those thunks as weak definitions of its
 * own, which the strong ones below replace at link; with
 * -fno-optimize-sibling-calls and -fno-jump-tables no `br xN` is left in a
 * unit (tools/pc/build_game32.py checks the objects). The contract is the
 * x86 one in AAPCS64 terms: the thunk is reached by `bl`, so LR is already
 * the caller's return address. X16 and X17 (IP0/IP1, which AAPCS64 lets a
 * call's veneer change; X16 carries the resolved target) and the flags may
 * change; on the slow path so may X9-X15, the temporaries no call keeps.
 * Everything else is kept, the argument registers X0-X8 and Q0-Q7 too.
 * The stack is as the caller left it. X18 is the platform register, never
 * a target (tests/pc/branch_thunks_test.c checks all of it).
 *
 * Fast path: a target with anything in its upper half (the system's
 * libraries and the heap sit far above 4 GB), or with bits 21-28 or 30 set
 * (the game image is linked at a low address that has them), is branched to
 * at once. Slow path: X0-X8, Q0-Q7 (the argument registers), X29 and LR are
 * saved (224 bytes: the stack stays 16-byte aligned), the resolver is
 * called with the target's low half, and everything but X16/X17 is put
 * back before the branch. */
#define A64_THUNK_SCRATCH(n, scratch, move)                                               \
    ".p2align 2\n"                                                                        \
    ".globl __llvm_slsblr_thunk_x" #n "\n"                                                \
    ".type __llvm_slsblr_thunk_x" #n ", %function\n"                                      \
    "__llvm_slsblr_thunk_x" #n ":\n"                                                      \
    "    lsr " scratch ", x" #n ", #32\n"                                                 \
    "    cbnz " scratch ", 1f\n"                                                          \
    "    tst x" #n ", #0x1fe00000\n"                                                      \
    "    b.ne 1f\n"                                                                       \
    "    tst x" #n ", #0x40000000\n"                                                      \
    "    b.ne 1f\n"                                                                       \
    move                                                                                  \
    "    b memories_branch_resolve\n"                                                     \
    "1:  br x" #n "\n"                                                                    \
    ".size __llvm_slsblr_thunk_x" #n ", . - __llvm_slsblr_thunk_x" #n "\n"
#define A64_THUNK(n) A64_THUNK_SCRATCH(n, "x16", "    mov x16, x" #n "\n")

__asm__(".text\n"
        /* X16: the target; LR: the caller's return address. */
        ".p2align 2\n"
        "memories_branch_resolve:\n"
        "    stp x29, x30, [sp, #-224]!\n"
        "    mov x29, sp\n"
        "    stp x0, x1, [sp, #16]\n"
        "    stp x2, x3, [sp, #32]\n"
        "    stp x4, x5, [sp, #48]\n"
        "    stp x6, x7, [sp, #64]\n"
        "    str x8, [sp, #80]\n"
        "    stp q0, q1, [sp, #96]\n"
        "    stp q2, q3, [sp, #128]\n"
        "    stp q4, q5, [sp, #160]\n"
        "    stp q6, q7, [sp, #192]\n"
        "    adrp x17, :got:Memories_GuestBranchResolver\n"
        "    ldr x17, [x17, #:got_lo12:Memories_GuestBranchResolver]\n"
        "    ldr x17, [x17]\n"
        "    cbz x17, 2f\n"
        "    mov w0, w16\n"
        "    blr x17\n"
        "    mov x16, x0\n"
        "2:  ldp q6, q7, [sp, #192]\n"
        "    ldp q4, q5, [sp, #160]\n"
        "    ldp q2, q3, [sp, #128]\n"
        "    ldp q0, q1, [sp, #96]\n"
        "    ldr x8, [sp, #80]\n"
        "    ldp x6, x7, [sp, #64]\n"
        "    ldp x4, x5, [sp, #48]\n"
        "    ldp x2, x3, [sp, #32]\n"
        "    ldp x0, x1, [sp, #16]\n"
        "    ldp x29, x30, [sp], #224\n"
        "    br x16\n"
        /* The entry of a host stub the build writes for a module function
         * that C calls by name (tools/pc/build_game32.py, guest_branches.c):
         * the stub leaves the guest address in X16 and keeps LR. */
        ".p2align 2\n"
        ".globl Memories_GuestBranchDirect\n"
        ".type Memories_GuestBranchDirect, %function\n"
        "Memories_GuestBranchDirect:\n"
        "    b memories_branch_resolve\n"
        ".size Memories_GuestBranchDirect, . - Memories_GuestBranchDirect\n"
        A64_THUNK(0) A64_THUNK(1) A64_THUNK(2) A64_THUNK(3) A64_THUNK(4) A64_THUNK(5) A64_THUNK(6)
        A64_THUNK(7) A64_THUNK(8) A64_THUNK(9) A64_THUNK(10) A64_THUNK(11) A64_THUNK(12) A64_THUNK(13)
        A64_THUNK(14) A64_THUNK(15)
        A64_THUNK_SCRATCH(16, "x17", "")
        A64_THUNK_SCRATCH(17, "x16", "    mov x16, x17\n")
        A64_THUNK(19) A64_THUNK(20) A64_THUNK(21) A64_THUNK(22) A64_THUNK(23) A64_THUNK(24)
        A64_THUNK(25) A64_THUNK(26) A64_THUNK(27) A64_THUNK(28) A64_THUNK(29));
#endif
