#include "types.h"
#include "game/model.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void Model_QueueTintRequestForParts(s32,s32,ModelTintColor,ModelTintColor,s32,...);
static unsigned calls;
void Model_QueueTintRequest(s32 slot, s32 selection, ModelTintColor start,
                           ModelTintColor end, s32 duration, const u8 *mask) {
    assert(slot==1 && selection==128 && duration==17);
    assert(start.b0==1 && start.b1==2 && start.b2==3 && start.b3==5);
    assert(end.b0==6 && end.b1==7 && end.b2==8 && end.b3==9);
    const u8 wanted[8]={1,128,0,0,0,0,0,128};
    const u8 empty[8]={0};
    assert(!memcmp(mask,calls ? empty : wanted,8)); calls++;
}
int main(void) {
    MemoriesMemory *memory=calloc(1,sizeof(*memory)); assert(memory && !GuestRuntime_Bind(memory));
    ModelTintColor start={.b0=1,.b1=2,.b2=3,.b3=4},end={.b0=6,.b1=7,.b2=8,.b3=9};
    Model_QueueTintRequestForParts(1,133,start,end,17,0,15,63,-1);
    Model_QueueTintRequestForParts(1,133,start,end,17,-1);
    assert(calls==2); GuestRuntime_Reset(); free(memory);
    puts("Actual model tint variadics: three parts and empty list pass");
}
