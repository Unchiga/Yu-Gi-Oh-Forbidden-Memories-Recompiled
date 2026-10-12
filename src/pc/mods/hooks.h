#ifndef MEMORIES_MOD_HOOKS_H
#define MEMORIES_MOD_HOOKS_H
/* Function hooks for code mods (hooks.c, mod API 4). */
/* Replace `function`, a game function, with `replacement` for `owner`'s mod
 * while it is applied; *original (when given) is kept pointing at what the
 * replacement should call to run the function it displaced. A token, or 0
 * when the function cannot be hooked (not a game function, or full). */
int Hooks_Add(int owner, void *function, void *replacement, void **original);
void Hooks_Remove(int owner, int token);
void Hooks_Clear(int owner);
/* Rebuild every chain from the mods now applied (Mods_Active). */
void Hooks_Relink(void);
int Hooks_IsHooked(const void *function);
/* Hook `index` (from 0) of any mod, applied or not: 0 past the last, else 1
 * with its mod and the function it hooks (the Mods window's overlaps). */
int Hooks_At(int index, int *owner, const void **function);
#if (defined(__APPLE__) || defined(__SWITCH__)) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
/* Native wrappers retain the original ABI; no executable-memory patching. */
void Hooks_Register(void *function, void *body);
void *Hooks_Resolve(void *function, void *body);
#endif
#endif
