#include "game/duel_effect_request.h"
#include "game/duel_side_state.h"
#include "pc/guest/retail_image.h"
#include "pc/debug/log.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
unsigned char D_8009B261;
DuelFieldEffectObject *G32 gDuel_apSwordsEffectObjects[DUEL_SIDE_COUNT];
static int calls;
static DuelEffectRequest *wanted;
unsigned Memories_PresentedFrames(void){return 0;}
int Log_Wanted(LogChannel c){(void)c;return 0;}
void Log_Printf(LogChannel c,const char*f,...){(void)c;(void)f;}
int RetailImage_Verified(RetailImageId id){assert(id==RETAIL_IMAGE_DUEL_EFFECTS);return 1;}
int RetailImage_CodeVerified(RetailImageId id){(void)id;return 1;}
const char *Mods_DuelEffectsOwnData(void){return 0;}
int Stars_EffectCell(int s,int*t,int*u,int*v,int*c){(void)s;(void)t;(void)u;(void)v;(void)c;return 0;}
void duel_effects__func_80146258(int id,int phase,void*buffer,DuelEffectRequest*request)
{
 assert(id==-7 && phase==-123 && (uintptr_t)buffer==0x80018000u && request==wanted);calls++;
}
extern void func_801462B0(short,short,int,DuelEffectRequest*);
int main(void){DuelEffectRequest request={0};wanted=&request;func_801462B0(-7,-123,(int)0x80018000u,&request);assert(calls==1);puts("Exact effect import ABI delegates to real retail-verification gate");}
