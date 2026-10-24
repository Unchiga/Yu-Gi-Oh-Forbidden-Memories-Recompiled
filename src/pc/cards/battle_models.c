/* A mod's model bigger than a MODEL.MRG record in the battle: battle_models.h. */
#include "battle_models.h"
#include "pc/mods/mods.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/render/packets.h"
#include "pc/render/soft_gpu.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "game/model.h"
#define MODEL_SLOT_SETUP_EXPLICIT_TRANSFER_ARGS
#include "game/model_slot_setup.h"
#include <stdint.h>
#include <string.h>

extern s32 D_800FE240;   /* GsSetWorkBase's cursor */
extern u8 D_8009AFA2;    /* the frame's half of the packet buffers */

#define ARENAS 3                          /* slot 0's, slot 1's, the check's */
#define PACKETS(slot) (0x80100000u + (unsigned)(slot) * 0x40000u)
#define PACKET_AREA 0x35000u              /* the record's model data and what follows it */
#define HALF (PACKET_AREA / 2)
#define WORK_PARTS (MODEL_SLOT_PART_COUNT * 0x14)   /* func_80056828's phase 4, at most */
/* A model's own texture pages: its slot samples texture bank 17 or 18
 * (soft_gpu.h; not 16, which a stray bit 10 alone would name), which holds
 * the slot's VRAM block (256x256 halfwords at slot * 256, 256), its palette
 * rows and the pages. A page is 64x256 halfwords where func_8004D134 puts
 * the page its polygons were authored on: column - 10 + slot * 4. The disc's
 * models use authored columns 10-13 of the lower row; a model's pages are,
 * in order, 14 and 15 of the lower row, then 10 to 15 of the upper. */
#define TEXTURE_BANK(slot) (17 + (slot))
#define PAGE_W 64
#define PAGE_H 256
static const unsigned char page_places[][2] = {{14, 1}, {15, 1}, {10, 0}, {11, 0}, {12, 0}, {13, 0}, {14, 0}, {15, 0}};

/* What func_8004CB0C counted of the model each arena holds, for the cursor
 * of the half each slot draws into. */
static struct {
    s32 packets[ARENAS], normals[ARENAS];
} parsed;
static s32 cursor[2];
static int begun[2], too_many_parts, refused_draw[2], drawn[2];
/* The check's parse (BattleModels_Page, BattleModels_Image): its slot, or
 * -1; its model's pages; the first page or image it refused. */
static int checking = -1, checking_pages, refused_page = -1, refused_image;
/* Which window's pages are in each slot's bank (Mods_ModelWindows), -2 for none. */
static int placed[2] = {-2, -2};

static int arena_of(const void *at)
{
    for (int k = 0; k < ARENAS; k++) {
        unsigned size;
        const unsigned char *arena = Mods_ModelArena(k, &size);
        /* Strictly inside: the arenas are contiguous, and a model's data
         * ends before its arena does (BattleModels_Check). */
        if (arena && (uintptr_t)at >= (uintptr_t)arena && (uintptr_t)at - (uintptr_t)arena < size) return k;
    }
    return -1;
}

/* The battle slot of index `index` if it holds a bigger model: its arena. */
static int slot_arena(int index)
{
    int k;
    if (index < 0 || index >= 2) return -1;
    k = arena_of((const void *)(uintptr_t)D_800F2C40[index].field_DE0);
    return k < 2 ? k : -1;
}

/* The half of slot k's packet area this frame draws into, from its start. */
static void begin_frame(int k)
{
    if (!begun[k]) {
        begun[k] = 1;
        cursor[k] = (s32)(PACKETS(k) + D_8009AFA2 * HALF);
        refused_draw[k] = 0;
    }
}

void BattleModels_BeginFrame(void) { begun[0] = begun[1] = 0; }

static s32 room(int k)
{
    begin_frame(k);
    return (s32)(PACKETS(k) + D_8009AFA2 * HALF + HALF) - cursor[k];
}

void BattleModels_Parsed(const void *hmd, int packet_bytes, int normals)
{
    int k = hmd ? arena_of(hmd) : -1;
    if (k >= 0) {
        parsed.packets[k] = packet_bytes;
        parsed.normals[k] = normals;
    }
}

void BattleModels_TooManyParts(void) { too_many_parts = 1; }

/* Whether authored page `column` of `row` is the record's or one of the
 * model's own pages. */
static int page_given(int column, int row)
{
    int i;
    if (column >= 10 && column <= 13 && row == 1) return 1;
    for (i = 0; i < checking_pages; i++)
        if (page_places[i][0] == column && page_places[i][1] == row) return 1;
    return 0;
}

void BattleModels_Page(unsigned authored)
{
    /* A polygon reads its page and, at 8 and 15 bits, the next one or three
     * (an 8-bit page is 128 halfwords across). */
    int column = (int)(authored & 15), row = (int)(authored >> 4 & 1), depth = (int)(authored >> 7 & 3), i;
    int columns = depth == 0 ? 1 : depth == 1 ? 2 : 4;
    if (checking < 0 || refused_page >= 0) return;
    if (authored & 0x7C00) { /* a texture bank of its own: the slot's is ORed in (stamp) */
        refused_page = 0x100 | (int)(authored & 0x1F);
        return;
    }
    for (i = 0; i < columns; i++)
        if (column + i > 15 || !page_given(column + i, row)) {
            refused_page = (column + i) | row << 4;
            return;
        }
}

int BattleModels_Image(int clut, int slot, int x, int y, int w, int h)
{
    int left = slot * 256;
    int inside = w > 0 && h > 0 && x >= left && x + w <= left + 256 &&
                 (clut ? y >= 0xF8 && y + h <= 0x100 : y >= 256 && y + h <= 512);
    if (checking < 0 && slot_arena(slot) < 0) return 1;   /* the disc's and record-sized models upload as ever */
    if (!inside && checking >= 0 && !refused_image) {
        refused_image = 1;
        LOG(LOG_MODS, "battle slot %d: a model's %s at %d,%d, %dx%d, is outside the slot's (x %d to %d, y %s); it fights as its base",
            slot, clut ? "palette" : "picture", x, y, w, h, left, left + 255, clut ? "248 to 255" : "256 to 511");
    }
    return inside;
}

static void bank_rect(uint16_t *bank, const uint16_t *from, int x, int y, int w, int h)
{
    for (int row = 0; row < h; row++)
        memcpy(bank + (size_t)(y + row) * SOFT_GPU_WIDTH + x, from + (size_t)(y + row) * SOFT_GPU_WIDTH + x,
               (size_t)w * sizeof(*bank));
}

/* Points a run of the slot's sorted packets at its texture bank: the page
 * word, the second corner's texture word (3D Monsters' stamp_bank too). */
static void stamp(s32 from_address, s32 to_address, int bank)
{
    u32 *from = (u32 *)(uintptr_t)(u32)from_address, *to = (u32 *)(uintptr_t)(u32)to_address;
    u32 bits = (u32)SOFT_GPU_BANK_TPAGE(bank) << 16;
    while (from < to) {
        unsigned words = *from >> 24, at = 1;
        while (at <= words) {
            u32 command = from[at];
            unsigned op = command >> 24, length = (unsigned)Memories_GpuCommandWords(command);
            if (!length || at + length > words + 1) break;
            if (op >= 0x24 && op <= 0x3f && (op & 4)) from[at + 4 + ((op & 0x10) ? 1 : 0)] |= bits;
            at += length;
        }
        from += words + 1;
    }
}

/* A slot whose model has texture pages: its bank has the pages, and its VRAM
 * block and palette rows as the frame left them (the game tints and flashes
 * the rows as it draws), and its packets sample it. */
static void textures(int k, s32 from, s32 to)
{
    int count, i, row;
    unsigned size;
    const unsigned char *pages = Mods_ModelPages(k, &count);
    const int *windows = Mods_ModelWindows(&size);
    const uint16_t *vram = SoftGpu_Vram();
    uint16_t *bank;
    if (!pages || !(bank = SoftGpu_Bank(TEXTURE_BANK(k)))) return;
    if (placed[k] != windows[k]) {
        for (i = 0; i < count; i++) {
            int x = (page_places[i][0] - 10 + k * 4) * PAGE_W, y = page_places[i][1] * PAGE_H;
            for (row = 0; row < PAGE_H; row++)
                memcpy(bank + (size_t)(y + row) * SOFT_GPU_WIDTH + x, pages + ((size_t)i * PAGE_H + row) * PAGE_W * 2,
                       PAGE_W * 2);
        }
        placed[k] = windows[k];
        LOG(LOG_MODS, "battle slot %d: %d texture pages of its own, in texture bank %d", k, count, TEXTURE_BANK(k));
    }
    bank_rect(bank, vram, k * 256, 256, 256, 256);
    bank_rect(bank, vram, k * 256, 0xF0, 256, 16);
    bank_rect(bank, vram, 0x200, 0xF0, 256, 4);
    stamp(from, to, TEXTURE_BANK(k));
}

int BattleModels_Check(int slot)
{
    static ModelSlot keep;
    unsigned size;
    unsigned char *arena = Mods_ModelArena(slot, &size), *check = Mods_ModelArena(2, NULL);
    ModelSlot *p;
    s32 hmd_size, work;
    u32 units;
    int parts, inside, ok;
    if (slot < 0 || slot >= 2 || !arena || !check || !Mods_ModelPending(slot)) return 0;
    /* Parsed in a copy, which the parse rewrites, with the slot as it is. */
    memcpy(check, arena, size);
    hmd_size = *(s32 *)check ? *(s32 *)check : 0xC000;
    if (hmd_size <= 0 || (unsigned)hmd_size >= size) {
        LOG(LOG_MODS, "battle slot %d: a bigger model's data says it is %d bytes, more than its arena; "
                      "it fights as its base", slot, hmd_size);
        return 0;
    }
    /* func_8004CB0C takes 60 units and drops the rest, as a byte. */
    units = *(u32 *)(check + 0xC);
    if (units > 0x3C) {
        LOG(LOG_MODS, "battle slot %d: a bigger model has %u units (60 at most); it fights as its base", slot,
            units);
        return 0;
    }
    p = &D_800F2C40[slot];
    keep = *p;
    too_many_parts = 0;
    parsed.packets[2] = parsed.normals[2] = 0;
    Mods_ModelPages(slot, &checking_pages);
    refused_page = -1;
    refused_image = 0;
    checking = slot;
    func_8004CB0C(slot, check, hmd_size, -1);
    checking = -1;
    parts = p->field_E1B;
    /* Its work area, from its type-2 unit on, must be in the arena: the
       slot's draws are told from others' by it (slot_arena). */
    inside = (uintptr_t)p->field_DE0 >= (uintptr_t)check && (uintptr_t)p->field_DE0 - (uintptr_t)check < size;
    work = inside ? (s32)((uintptr_t)p->field_DE0 - (uintptr_t)check) + WORK_PARTS + parsed.normals[2] * 4 : -1;
    *p = keep;
    if (refused_page >= 0x100)
        LOG(LOG_MODS, "battle slot %d: a bigger model's polygons name a texture bank of their own in their page "
                      "words (bits 10-14); it fights as its base", slot);
    else if (refused_page >= 0)
        LOG(LOG_MODS, "battle slot %d: a bigger model's polygons use texture page %d of row %d, which is neither "
                      "the record's (10 to 13 of row 1) nor one of its %d pages; it fights as its base", slot,
            refused_page & 15, refused_page >> 4, checking_pages);
    if (refused_page < 0 && checking_pages && !SoftGpu_Bank(TEXTURE_BANK(slot))) {
        refused_page = 0x200;
        LOG(LOG_MODS, "battle slot %d: no memory for texture bank %d, for a model's own pages; it fights as its "
                      "base", slot, TEXTURE_BANK(slot));
    }
    ok = refused_page < 0 && !refused_image && !too_many_parts && inside && parsed.normals[2] <= 0xFFFF &&
         parsed.packets[2] <= (s32)HALF - MODEL_DATA_MIN_FREE_BYTES && work <= (s32)size;
    if (ok) {
        LOG(LOG_MODS, "battle slot %d: a bigger model, %d bytes of model data, %d parts, %d bytes of packets, "
                      "drawn from its own RAM", slot, hmd_size, parts, parsed.packets[2]);
        drawn[slot] = 0;
    } else if (refused_page < 0 && !refused_image)   /* those are said above */
        LOG(LOG_MODS, "battle slot %d: a bigger model does not fit: %s%d parts (%d at most), %d bytes of "
                      "packets (%d at most), %d normals (65535 at most), %s%d bytes with its work area (%u at "
                      "most); it fights as its base", slot, too_many_parts ? "over " : "", parts,
            MODEL_SLOT_PART_COUNT, parsed.packets[2], (int)HALF - MODEL_DATA_MIN_FREE_BYTES, parsed.normals[2],
            inside ? "" : "no type-2 unit in its data, ", work, size);
    too_many_parts = 0;
    return ok;
}

int BattleModels_BeginSort(int index, s32 *saved)
{
    int k = slot_arena(index);
    if (k < 0) return 0;
    if (room(k) - parsed.packets[k] < MODEL_DATA_MIN_FREE_BYTES) {
        if (!refused_draw[k]) LOG(LOG_MODS, "battle slot %d: no room left this frame for its model's packets", k);
        refused_draw[k] = 1;
        return -1;
    }
    *saved = D_800FE240;
    D_800FE240 = cursor[k];
    return k + 1;
}

void BattleModels_EndSort(int begun, s32 saved)
{
    int k = begun - 1;
    if (begun <= 0) return;
    textures(k, cursor[k], D_800FE240);
    if (!drawn[k] && D_800FE240 > cursor[k]) {   /* once a load, when it is first seen */
        LOG(LOG_MODS, "battle slot %d: %d bytes of packets drawn in its own RAM", k, D_800FE240 - cursor[k]);
        drawn[k] = 1;
    }
    cursor[k] = D_800FE240;
    D_800FE240 = saved;
}

int BattleModels_Insufficient(int index, int type)
{
    int k = slot_arena(index);
    if (k < 0) return -1;
    return room(k) - parsed.packets[k] * (type == 5 ? 2 : 1) < MODEL_DATA_MIN_FREE_BYTES;
}

/* The arenas as the game left them (parsed in place, with absolute pointers
 * the slots in RAM keep), their counts, and which slot's load has not
 * parsed yet: after the windows' chunk, whose Mods_ModelRebind clears that. */
void BattleModels_State(struct MemoriesState *state)
{
    MemoriesStateField fields[4];
    unsigned size;
    fields[0].data = &parsed;
    fields[0].size = sizeof(parsed);
    fields[1].data = Mods_ModelPendings(&size);
    fields[1].size = size;
    fields[2].data = Mods_ModelArena(0, &size);
    fields[2].size = size;
    fields[3].data = Mods_ModelArena(1, &size);
    fields[3].size = size;
    if (!fields[2].data || !fields[3].data) return;
    Memories_StateChunk(state, "mod-arenas", fields, 4);
    if (Memories_StateLoading(state)) {
        BattleModels_BeginFrame();   /* the halves start over */
        placed[0] = placed[1] = -2;  /* and the banks take the pages again */
    }
}
