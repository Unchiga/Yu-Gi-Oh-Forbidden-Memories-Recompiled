#include "types.h"
#include "game/model.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
extern void func_8004EB00(void);
static unsigned called;
static unsigned updates;
void func_8005A188(s32 index) { assert(index == 1); updates++; }
int Cards_ModelStatsIndex(int model) { assert(model == 1); return model; }
static s32 first(s32 data, s32 mode) { assert(data == 0x801b0000 && mode == -1); called=1; return 0; }
static s32 middle(s32 data, s32 mode) { abort(); }
static s32 last(s32 data, s32 mode) { assert(data == 0x801b0000 && mode == -1); called=4; return 0; }
int main(void) {
 MemoriesMemory *m=calloc(1,sizeof(*m)); assert(m && !GuestRuntime_Bind(m));
 u32 *table=Memories_Resolve(m,0x800114e8,20,4);
 for(unsigned i=0;i<4;i++) {table[i]=0x8006a000+4*i; assert(!GuestRuntime_RegisterFunction(table[i],(void (*)(void))(i==0 ? first : i==3 ? last : middle)));}
 table[4]=0xdeadbeef;
 ModelSlot *slots=Memories_Resolve(m,0x800f2c40,3*sizeof(ModelSlot),4);
 slots[1].field_DF8=1; slots[0].field_DEC=(s32)0x801b0000;
 s8 *state=Memories_Resolve(m,0x8009af9a,1,1);
 *state=5; func_8004EB00(); assert(called==1 && *state==5);
 slots[0].field_E0F=6; *state=22; called=0; func_8004EB00(); assert(called==4 && *state==22);
 assert(table[4]==0xdeadbeef && updates==2); GuestRuntime_Reset();free(m);
 puts("Actual model effect controller copies four guest callbacks and dispatches first/last entries");
}
