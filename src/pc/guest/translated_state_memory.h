#ifndef MEMORIES_TRANSLATED_STATE_MEMORY_H
#define MEMORIES_TRANSLATED_STATE_MEMORY_H
#include "state.h"
typedef struct MemoriesNativeMemoryState MemoriesNativeMemoryState;
int Memories_NativeMemorySave(MemoriesState *state);
MemoriesNativeMemoryState *Memories_NativeMemoryPrepare(MemoriesState *state, char *why, size_t size);
int Memories_NativeMemoryApply(MemoriesNativeMemoryState *pending);
/* `old_size`: the saved image's size, which classifies an address; `stack`:
 * the words are on the native stack (translated_state_symbols.h). */
void Memories_NativeMemoryRelocate(MemoriesNativeMemoryState *pending, void *data, size_t size,
                                  uintptr_t old_image, uintptr_t new_image, size_t old_size, int stack);
void Memories_NativeMemoryFree(MemoriesNativeMemoryState *pending);
/* How many words of the saved regions point into another build's image
 * where the symbol tables cannot carry them (as the old image's size,
 * Memories_NativeSymbolsOldSize, classifies them). */
size_t Memories_NativeMemoryUncarried(MemoriesNativeMemoryState *pending, uintptr_t old_image);
void Memories_NativeMemoryRestorePayloads(MemoriesNativeMemoryState *pending,
                                         uintptr_t old_image, uintptr_t new_image, size_t old_size);
void Memories_NativeMemoryRemapGuests(MemoriesNativeMemoryState *pending, uint32_t from, uint32_t to, uint32_t size);
#endif
