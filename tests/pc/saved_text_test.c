/* What goes in a saved block is copied with saved_text, compiled here as the
 * translated build compiles src/pc (malloc renamed, strdup not): the copy has a token,
 * so it is in a saved region and moves with a state. EncodePointer stops the program
 * for a block it does not know. */
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *GuestRuntime_malloc(size_t length);
#define malloc GuestRuntime_malloc
#include "pc/mods/saved_text.inc"
#undef malloc

int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    char *copy;
    unsigned token;
    assert(memory && !GuestRuntime_Bind(memory));
    copy = saved_text("test-model");
    assert(copy && !strcmp(copy, "test-model"));
    token = GuestRuntime_EncodePointer(copy);
    assert(token && GuestRuntime_ResolveData((void *)(uintptr_t)token, 11) == copy);
    puts("Saved text: a copy with a token");
    return 0;
}
