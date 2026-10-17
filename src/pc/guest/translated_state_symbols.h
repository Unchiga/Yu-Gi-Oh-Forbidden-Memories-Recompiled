#ifndef MEMORIES_TRANSLATED_STATE_SYMBOLS_H
#define MEMORIES_TRANSLATED_STATE_SYMBOLS_H
/* Carrying a translated game's save state to another build by the two
 * builds' symbol tables (translated_state_symbols.c says what is carried). */
#include <stddef.h>
#include <stdint.h>
/* Where the game's image is loaded (dladdr), or 0. */
uintptr_t Memories_NativeImageBase(void);
/* The running build's table (symbols/<id>.txt), malloc'd, or NULL. */
char *Memories_NativeSymbolsText(size_t *length);
/* Map offsets of the build whose table is `saved` (its image
 * `old_image_size` long) to this one's: 0 when both tables are version 2
 * and read whole. The Tables form takes this build's table as text (the
 * tests). */
int Memories_NativeSymbolsPrepare(const void *saved, size_t saved_length, uint64_t old_image_size,
                                  uint64_t new_image_size);
int Memories_NativeSymbolsPrepareTables(const void *saved, size_t saved_length, uint64_t old_image_size,
                                        const char *current, size_t current_length, uint64_t new_image_size);
int Memories_NativeSymbolsActive(void);
void Memories_NativeSymbolsClear(void);
/* The old image's size, as prepared (what an address is classified by). */
uint64_t Memories_NativeSymbolsOldSize(void);
/* An offset into the old image, in the new one: 0, or -1 when it cannot be
 * carried with certainty. `stack`: the word is on the native stack (or in
 * the saved registers), where a return address inside a function is
 * carried; anywhere else only a function's start is. */
int Memories_NativeSymbolsMap(uint64_t offset, int stack, uint64_t *mapped);
/* How many words of `data` (every 8 bytes, or with `aligned` false at
 * every byte) point into the old image at `old_image` where
 * Memories_NativeSymbolsMap refuses them. */
size_t Memories_NativeSymbolsScan(const uint8_t *data, size_t size, uint64_t old_image, int stack, int aligned);
#endif
