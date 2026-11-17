#include "pc/guest/image.h"
#include "pc/guest/translated_runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
extern void *resolve_function(uint32_t);
extern void func_8016AA6C(void);
extern void func_8016866C(int32_t);
extern void func_80168FB4(void);
static int name_calls, alternate_calls, campaign_index, free_calls;
static void campaign_entry(int32_t index) { campaign_index=index; }
static void free_entry(void) { free_calls++; }
static void name_entry(void) { name_calls++; }
static void alternate_entry(void) { alternate_calls++; }
const MemoriesGuestFunction Memories_FunctionMap[]={
 /* Generated maps are sorted by guest address; equal keys keep their order. */
 {0x8016866C,(void (*)(void))campaign_entry,0x80168000,0x14},
 {0x80168FB4,free_entry,0x80168000,0x13},
 {0x8016AA6C,name_entry,0x80168000,0x15},
 {0x8016AA6C,alternate_entry,0x80168000,0x14}};
const unsigned Memories_FunctionMapCount=4;
/* No interpreter in the translated game: an unknown bank must resolve NULL, never a wrong native. */
int main(void) {
 MemoriesMemory *memory=calloc(1,sizeof(*memory));assert(memory && !GuestRuntime_Bind(memory));
 uint32_t *bank=Memories_Resolve(memory,0x80168000,4,4);assert(bank);
 GuestRuntime_SetFunctionResolver(resolve_function);
 *bank=0x15;func_8016AA6C();assert(name_calls==1 && alternate_calls==0);
 *bank=0x14;func_8016AA6C();assert(name_calls==1 && alternate_calls==1);
 func_8016866C(-1234567);assert(campaign_index==-1234567);
 *bank=0x13;func_80168FB4();assert(free_calls==1);assert(resolve_function(0x8016866C)==NULL);
 *bank=0x14;assert(resolve_function(0x80168FB4)==NULL);
 *bank=0x99;assert(resolve_function(0x8016AA6C)==NULL);
 GuestRuntime_Reset();free(memory);puts("Real shared-bank resolver and direct entry bridge select active module and reject unknown bank");return 0;
}
