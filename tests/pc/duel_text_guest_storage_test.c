#include "types.h"
#include "ygo_types.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
extern void func_80037DA4(DuelEffectChannel *);
extern void func_800384E4(DuelEffectChannel *);
/* These branches are deliberately not used by the stream advance case. */
int Cards_BaseId(int id) { (void)id; abort(); }
int Cards_Type(int id) { (void)id; abort(); }
const u8 *Cards_TypeNameText(int type) { (void)type; abort(); }
const u8 *Cards_NameText(int id) { (void)id; abort(); }
const u8 *Cards_RowNameText(int id) { (void)id; abort(); }
const u8 *Cards_DescriptionText(int id) { (void)id; abort(); }
int Stars_CardSingle(int id) { (void)id; abort(); }
int Stars_NoStarUsed(void) { abort(); }
void Stars_MarkIcon(int id) { (void)id; abort(); }
void TypeIcons_MarkIcon(int type) { (void)type; abort(); }
const u8 *Text_Resolve(int id, const u8 *retail) { (void)id; (void)retail; abort(); }
void func_80036C14(DuelEffectChannel *p, s32 id) { (void)p; (void)id; abort(); }
/* Color rendering is covered by its own native contract; keep these hooks
 * inert while this test exercises the translated stream pointer updates. */
void CardTextColors_Apply(DuelEffectChannel *channel, int part, int star)
{ (void)channel; (void)part; (void)star; }
void CardTextColors_Restore(DuelEffectChannel *channel) { (void)channel; }
void DuelUi_NameStream(void *channel, int on) { (void)channel; (void)on; }
int main(void) {
    MemoriesMemory *memory = calloc(1, sizeof(*memory)); assert(memory && !GuestRuntime_Bind(memory));
    DuelEffectChannel *channel = Memories_Resolve(memory, 0x801b1000, sizeof(*channel), 4);
    u8 *stream = Memories_Resolve(memory, 0x801b2000, 16, 4);
    u8 *words = (u8 *)channel;
    channel->stream_58 = 0;
    Memories_WriteLE32(words, 0x801b2000); Memories_WriteLE32(words + 4, 0x801b125c);
    stream[0] = 0x10;
    func_80037DA4((DuelEffectChannel *)(uintptr_t)0x801b1000);
    assert(Memories_ReadLE32(words) == 0x801b2001 && Memories_ReadLE32(words + 4) == 0x801b125c);
    stream[1] = 1;
    func_800384E4((DuelEffectChannel *)(uintptr_t)0x801b1000);
    assert(Memories_ReadLE32(words) == 0x801b2002 && Memories_ReadLE32(words + 4) == 0x801b125c);
    assert(channel->flags_34 & 0x1000);
    GuestRuntime_Reset(); free(memory); puts("Real duel text commands advance four-byte streams and preserve adjacent pointers");
}
