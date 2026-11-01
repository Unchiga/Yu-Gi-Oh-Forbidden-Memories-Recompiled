#include "types.h"
#include "game/model.h"
#include "game/model_handler_registry.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
extern void func_8004D58C(s32, u8 *);
extern s32 func_8004D134(s32, u16 *, u8 *, s32 *, s32 *);
extern int GsLinkAnim(u32 *G32 *, u32 *);
static unsigned driver_calls;
void GsU_00000000(void) { driver_calls++; }
void *func_800603DC(u32 type) { assert(type == 0x02000000); return GsU_00000000; }
int DrawSync(int mode) { assert(!mode); return 0; }
/* What a mod's model is checked by (src/pc/cards/battle_models.h): nothing here. */
void BattleModels_Page(unsigned authored) { (void)authored; }
int BattleModels_Image(int clut, int slot, int x, int y, int w, int h)
{ (void)clut; (void)slot; (void)x; (void)y; (void)w; (void)h; return 1; }
static u8 *area(u32 address, size_t size) {
    u8 *p = Memories_Resolve(GuestRuntime_Memory(), address, size, 4); assert(p); return p;
}
int main(void) {
    MemoriesMemory *memory = calloc(1, sizeof(*memory)); assert(memory && !GuestRuntime_Bind(memory));
    u8 *hmd = area(0x801b0000, 24), *block = area(0x801b0100, 32);
    u8 *section = area(0x801b0200, 32), *flags = area(0x801b0300, 16);
    Memories_WriteLE32(hmd + 16, 0x801b0100); Memories_WriteLE32(hmd + 20, 0x80100094);
    Memories_WriteLE32(block, 0xffffffff); Memories_WriteLE32(block + 4, 0x801b0200);
    Memories_WriteLE32(block + 8, 1); block[15] = 3;
    Memories_WriteLE32(section + 8, 0x801b0300); Memories_WriteLE32(section + 12, 0x801b0400);
    Memories_WriteLE32(section + 16, 0x801b0500);
    Memories_WriteLE32(flags, 2); Memories_WriteLE32(flags + 4, 0x100); Memories_WriteLE32(flags + 8, 0);
    ModelSlot *slot = (ModelSlot *)area(0x800f2c40, sizeof(ModelSlot));
    func_8004D58C(0, (u8 *)(uintptr_t)0x801b0000);
    assert((uintptr_t)slot->field_DD8 == 0x801b0400 && (uintptr_t)slot->field_DDC == 0x801b0500);
    assert(slot->field_BEC[0] == 1);
    assert(slot->field_2C8[9][57] == 0xffff && slot->field_750[9].max == 0);
    block[15] = 2;
    Memories_WriteLE32(section + 4, 0x801b0600);
    Memories_WriteLE32(section + 8, 0x801b0700);
    func_8004D58C(0, (u8 *)(uintptr_t)0x801b0000);
    assert((uintptr_t)slot->field_DE0 == 0x801b0600 && (uintptr_t)slot->field_DE4 == 0x801b0700);
    /* Scratch pointers are adjacent u32 values, as written by GsScanUnit. */
    u8 *scratch = area(0x1f800000, 32), *hdr = area(0x801b0800, 16), *base = area(0x801b0900, 32);
    Memories_WriteLE32(scratch, 0x801b0800); Memories_WriteLE32(scratch + 4, 0x80100094);
    Memories_WriteLE32(scratch + 20, 0x801b0900); Memories_WriteLE32(scratch + 24, 0xdeadbeef);
    hdr[2] = 1; base[12] = 3;
    u16 kind = 9; s32 best = 0, total = 0;
    assert(func_8004D134(0, &kind, (u8 *)(uintptr_t)0x1f800000, &best, &total) == 1);
    assert(best == 3 && total == 0x20);
    assert(Memories_ReadLE32(hmd + 20) == 0x80100094 && Memories_ReadLE32(scratch + 24) == 0xdeadbeef);
    assert(!GuestRuntime_RegisterFunction(0x80089e20, GsU_00000000));
    u8 *primitive = area(0x801b1000, 12), *event = area(0x801b1100, 8);
    Memories_WriteLE32(primitive + 4, 0xdeadbeef);
    Memories_WriteLE32(event, 0x02000000); Memories_WriteLE32(event + 4, 0x801b1000);
    Memories_WriteLE32(scratch, 0x801b1200); area(0x801b1200, 4);
    Model_ProcessType2Unit(0, (ModelTypeUnit *)(uintptr_t)0x801b1100, (u8 *)(uintptr_t)0x1f800000);
    assert(driver_calls == 1 && Memories_ReadLE32(primitive) == 0x80089e20);
    assert(Memories_ReadLE32(primitive + 4) == 0xdeadbeef);
    u8 *animation = area(0x801b1300, 32), *sequences = area(0x801b1400, 16);
    animation[6] = 2; animation[12] = 2; animation[20] = 2;
    Memories_WriteLE32(sequences + 8, 0xcafebabe);
    assert(GsLinkAnim((u32 *G32 *)(uintptr_t)0x801b1400, (u32 *)(uintptr_t)0x801b1300) == 2);
    assert(Memories_ReadLE32(sequences) == 0x801b1308 && Memories_ReadLE32(sequences + 4) == 0x801b1310);
    assert(Memories_ReadLE32(sequences + 8) == 0xcafebabe);
    GuestRuntime_Reset(); free(memory); puts("Real HMD row/channel readers preserve adjacent guest words");
}
