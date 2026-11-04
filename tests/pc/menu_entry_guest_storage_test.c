#include "types.h"
#include "game/display_object.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void MainMenu_StartFrontendEntryTransition(s32 mode);
static int slide_hooks;
/* The host owns timing/placement; this fixture checks the guest stores
 * before that hook, especially the adjacent four-byte entry pointers. */
void TitleMenu_StartSlide(void) { slide_hooks++; }
static s16 half(const u8 *object, size_t offset)
{
    s16 value;memcpy(&value, object + offset, sizeof(value));return value;
}
int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    const u32 addresses[] = {0x800f06ceu,0x800f0708u};
    u8 *entries, *objects[2];
    unsigned i;
    assert(memory && GuestRuntime_Bind(memory) == 0);
    entries = Memories_Resolve(memory, 0x80184568u, 11 * 4, 4);
    assert(entries);
    for (i = 0; i < 2; ++i) {
        objects[i] = Memories_Resolve(memory, addresses[i], sizeof(DisplayObject), 2);
        assert(objects[i]);
        Memories_WriteLE32(entries + 4 * i, addresses[i]);
    }
    MainMenu_StartFrontendEntryTransition(0);
    for (i = 0; i < 2; ++i) {
        s16 offset = i ? 480 : -160;
        assert(half(objects[i], offsetof(DisplayObject, field_34.h.field_36)) == offset);
        assert(half(objects[i], offsetof(DisplayObject, field_38.h.field_38)) == 160);
        assert(half(objects[i], offsetof(DisplayObject, field_30.h.field_30)) == offset);
        assert(objects[i][offsetof(DisplayObject, field_60)] == 16);
        assert(Memories_ReadLE32(entries + 4 * i) == addresses[i]);
    }
    MainMenu_StartFrontendEntryTransition(1);
    for (i = 0; i < 2; ++i) {
        s16 offset = i ? 480 : -160;
        assert(half(objects[i], offsetof(DisplayObject, field_34.h.field_36)) == 160);
        assert(half(objects[i], offsetof(DisplayObject, field_38.h.field_38)) == offset);
        assert(half(objects[i], offsetof(DisplayObject, field_30.h.field_30)) == 160);
    }
    assert(*(u8 *)Memories_Resolve(memory, 0x80184599u, 1, 1) == 1);
    assert(slide_hooks == 2);
    GuestRuntime_Reset();free(memory);
    puts("Real menu entry transition: four-byte cursor preserves adjacent entries for entrance/exit");
    return 0;
}
