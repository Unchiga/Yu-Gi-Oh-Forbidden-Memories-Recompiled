/* The registers a signal handler reads from its ucontext (Linux and Android),
 * named per architecture: the program counter, the stack pointer and the
 * frame pointer that crash reports walk from: 32-bit x86 (the desktop builds
 * and Android x86) and AArch64 (glibc and bionic share the kernel's
 * sigcontext names). The mod SDK
 * (MEMORIES_MOD) has no system headers and no use for these: there the
 * header is empty, and compiles on its own as every SDK header must
 * (tools/pc/check_mod_abi.py). */
#ifndef MEMORIES_PC_PLATFORM_SIGNAL_CONTEXT_H
#define MEMORIES_PC_PLATFORM_SIGNAL_CONTEXT_H
#if !defined(_WIN32) && !defined(MEMORIES_MOD) && !defined(__SWITCH__) /* no signals there: crash.c */
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#define SIGNAL_CONTEXT_PC(user) ((user)->uc_mcontext->__ss.__pc)
#define SIGNAL_CONTEXT_SP(user) ((user)->uc_mcontext->__ss.__sp)
#define SIGNAL_CONTEXT_FP(user) ((user)->uc_mcontext->__ss.__fp)
#elif defined(__i386__)
#define SIGNAL_CONTEXT_PC(user) ((user)->uc_mcontext.gregs[REG_EIP])
#define SIGNAL_CONTEXT_SP(user) ((user)->uc_mcontext.gregs[REG_ESP])
#define SIGNAL_CONTEXT_FP(user) ((user)->uc_mcontext.gregs[REG_EBP])
#elif defined(__aarch64__)
/* AArch64 (Android arm64-v8a): X29 is the frame pointer; a frame record
 * holds the caller's X29 at [fp] and the return address at [fp+8]. */
#define SIGNAL_CONTEXT_PC(user) ((user)->uc_mcontext.pc)
#define SIGNAL_CONTEXT_SP(user) ((user)->uc_mcontext.sp)
#define SIGNAL_CONTEXT_FP(user) ((user)->uc_mcontext.regs[29])
#else
#error "signal_context.h: no register names for this architecture"
#endif

#endif
#endif
