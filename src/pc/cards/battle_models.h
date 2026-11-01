#ifndef MEMORIES_PC_CARDS_BATTLE_MODELS_H
#define MEMORIES_PC_CARDS_BATTLE_MODELS_H
/* A mod's model bigger than a MODEL.MRG record (notes/modding.md, "Bigger
 * models") in the battle. Its model data is parsed in an arena of its slot's
 * past guest RAM (Mods_ModelArena), and the slot draws into the RAM its
 * record's model data takes otherwise, 0x35000 bytes at 0x80100000 (slot 0)
 * or 0x80140000 (slot 1), one half a frame, instead of the frame's shared
 * packet buffer: GPU packets link by 24-bit addresses, so they cannot leave
 * RAM, and that RAM is free while the slot holds such a model. Every hook
 * is a no-op for any other model: a slot is taken for one by its data's
 * place (field_DE0 in an arena), never by its index, as 3D Monsters sorts
 * its own models in slot 0's place.
 *
 * BattleModels_Check: whether the bigger model in slot `slot`'s arena fits,
 * parsed once in the third arena first: its parts, its packets in a half, its
 * work area in the arena. Model_LoadMonsterMerge loads the base otherwise.
 * BattleModels_Parsed: func_8004CB0C's counts, which the slot keeps as u16.
 * BattleModels_TooManyParts: GsLinkAnim would pass MODEL_SLOT_PART_COUNT.
 * BattleModels_Page: func_8004D134 relocates a polygon's page word, as the
 * model authored it; the check refuses a page that is neither the record's
 * nor one of the model's own texture pages (Mods_ModelPages), which its slot
 * then samples from a texture bank of its own.
 * BattleModels_Image: Model_ProcessType2Unit uploads a picture or palette a
 * model carries, relocated: whether it is inside slot `slot`'s VRAM block
 * (the rest is not uploaded, and the check refuses the model).
 * BattleModels_BeginFrame: Graphics_BeginFrame set the frame's work base;
 * the slots' halves start over too.
 * BattleModels_BeginSort / EndSort: around func_800540B4; 0 for a slot of
 * another model, -1 for one whose packets would not fit (it is not drawn).
 * BattleModels_Insufficient: Model_HasInsufficientBufferSpace for such a
 * slot, or -1 for another. BattleModels_State: the arenas in a save state. */
#include "types.h"

struct MemoriesState;
int BattleModels_Check(int slot);
void BattleModels_Parsed(const void *hmd, int packet_bytes, int normals);
void BattleModels_TooManyParts(void);
void BattleModels_Page(unsigned authored);
int BattleModels_Image(int clut, int slot, int x, int y, int w, int h);
void BattleModels_BeginFrame(void);
int BattleModels_BeginSort(int index, s32 *saved);
void BattleModels_EndSort(int begun, s32 saved);
int BattleModels_Insufficient(int index, int type);
void BattleModels_State(struct MemoriesState *state);

#endif
