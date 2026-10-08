/* The 3D Monsters mod (mod.json beside this file; notes/modding.md): the
 * face-up monsters on the duel field stand on their
 * cards as the models the battle presentation uses, animating on the spot,
 * floating just above them, the player's turned to face the opponent and the
 * opponent's to face the player, whichever way the camera is round. When one
 * monster attacks another, the two also stand on the big cards of the battle
 * presentation, turned towards each other (draw_battle).
 *
 * The console could not do this: one monster's MODEL.MRG record is a quarter
 * of its RAM and a quarter of its VRAM, and the duel already has both. The
 * port has neither limit, so each monster gets a private arena (mapped
 * outside guest RAM, at a negative address so the pointer-sign idioms in the
 * model code still hold) and a private texture bank in the software GPU
 * (src/pc/render/soft_gpu.h), which is VRAM-shaped memory a primitive selects
 * with bits the hardware leaves unused in its texture-page word.
 *
 * The monsters are sorted into the game's own model ordering table, the one
 * the battle presentation draws its two duellists into, so they are layered
 * where the game would layer them: over the field, under the hand and the
 * rest of the interface, and against each other by depth.
 *
 * Everything the game owns is borrowed and returned inside the pass: model
 * slot 0 (the duel field itself only uses slot 2, the arena), the packet work
 * base and, while a monster is being loaded, the loader's staging buffer and
 * the model area of VRAM. With the mod off nothing here runs at all.
 *
 * How a monster is loaded, all of it synchronous and beside the game's
 * streaming (Memories_DiscReadSectors), so the duel's own transfers are not
 * disturbed:
 *
 * - `Model_LoadMonsterMerge`'s id arithmetic picks the MODEL.MRG record;
 * - the record's seventeen phases are what `func_80056D7C` programs, so its
 *   copies are replayed here against this monster's arena instead of the two
 *   fixed duel arenas. The phases that a slot flagged `0x80` skips (the
 *   sequence bank and the 50-sector block) are skipped here too;
 * - the phases that upload to VRAM upload for real, because the palette phase
 *   of the setup below reads them back through the GPU. The duel's pixels are
 *   put back afterwards and the monster's block is kept in its bank;
 * - `func_80056828` then runs the same eleven setup phases the game runs,
 *   including `func_8004CB0C`'s parse of the HMD data in the arena. With the
 *   three command words at -1 (which the `0x80` flag leaves behind) no
 *   per-monster control module is ever called, so none is loaded.
 *
 * Drawing is `func_800540B4` and `func_800556E8`, the pair the Library's card
 * model view already drives, with the slot placed by `func_8005A4C4` at the
 * card's own field coordinates (`D_800908A0`, the table the duel projects its
 * card sprites from).
 */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "psyq/libhmd.h"
#include "ygo_types.h"
#include "game/model.h"
#include "game/duel_card.h"
#include "game/duel_card_layout.h"
#define DUEL_SCREEN_TABLES_TYPED_POSITIONS
#include "game/duel_screen_tables.h"
#include "game/view_state.h"
#include "game/graphics_frame.h"
#include "game/graphics_frame_buffer.h"
#define MODEL_SLOT_SETUP_EXPLICIT_TRANSFER_ARGS
#include "game/model_slot_setup.h"
#include "game/model_slot_updates.h"
#include "game/model_load_step.h"
#include "game/func_800540B4.h"
#include "game/func_800556E8.h"
#include "game/func_8005922C.h"
#include "game/func_80058DD8.h"
#include "game/file_transfer.h"
#include "game/file_transfer_steps.h"
#include "game/duel_display.h"
#include "game/duel_scene_state.h"
#include "game/high_memory_addresses.h"
#include "game/display_object.h"
#include "game/func_80015EF4.h"
#include "game/display_object_layout.h"
#include "game/display_object_work_slots.h"
#include "game/duel_effect_resource_record.h"
#include "pc/compat/gte.h"
#include "pc/render/packets.h"
#include "pc/render/soft_gpu.h"
#include "pc/mods/modapi.h"
#include "pc/cards/cards.h"
#include "field_art.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern u8 D_8009B1D5;          /* the side the view belongs to */
extern void *G32 D_800E9D98[]; /* D_800E9D90[2]: func_800540B4's table */
extern GsOT *G32 D_800E9D90[4]; /* the four ordering tables of the frame */
extern void (*G32 D_800E9DB0[4])(void); /* the frame service callbacks */
extern u32 D_800FE240;         /* GsSetWorkBase */

/* One monster's private RAM. 96 sectors of model data, then the module
 * destinations the record's phases write and the two data words
 * func_8004CB0C stores in the slot, which is everything the two duel arenas
 * hold for a slot. Mapped where a guest pointer is negative, as PS1 code
 * expects of a real address. */
#define ARENA_VARIANT 0x30000u
#define ARENA_PRIMARY 0x35000u
#define ARENA_DATA_A 0x36000u
#define ARENA_DATA_B 0x37000u
#define ARENA_SIZE 0x40000u
#define ARENA_BASE 0x90000000u

/* The model area of VRAM for slot 0: the texture block below the two display
 * buffers, and the palette rows beside them. The record also puts one
 * 256-pixel row aside at (0x200, 0xF2). */
#define BLOCK_X 0
#define BLOCK_Y 0xF0
#define BLOCK_W 0x100
#define BLOCK_H (0x200 - 0xF0)
#define ROW_X 0x200
#define ROW_Y 0xF2
#define ROW_W 0x100

/* The size a monster is drawn at: the middle one stands TALL_PIXELS high in
 * the middle of the field, on a 240-line picture, and the rest spread around
 * that (fit() explains how). MEMORIES_MODS_SCALE scales all of them. */
#define TALL_PIXELS 32
/* How tall a middling monster is at 1:1 in the middle of the field, measured
 * across the models this way; only the spread around it depends on this. */
#define MIDDLING_PIXELS 110
/* How many ordering-table entries a monster is moved towards the camera so
 * that it is drawn over the card it stands on (sort_monster). */
#define DEPTH_STEPS 3
#define SCALE_SMALLEST 0x100
#define SCALE_LARGEST 0x1800

/* fit() sizes a monster by its height alone: one whose arms or wings reach
 * far out to the sides comes out too wide for its card. These are drawn at
 * a share of the fitted size (MODEL_FIXED_ONE is the whole). */
static const struct {
    int card;
    int share;
} WIDE_MONSTERS[] = {
    {458, MODEL_FIXED_ONE * 3 / 5}, /* Kaminari Attack */
};

#define SECTOR 2048
#define RECORD_SECTORS MODEL_MRG_SECTOR_COUNT
/* Models kept loaded, least recently drawn replaced first. The field has
 * ten monster zones, and with fewer entries than that a field of nine or ten
 * different monsters missed on every draw: all of them were loaded again from
 * the disc every frame (about 40 ms at ten), and each came back at the first
 * frame of its animation, standing still. Each entry has its own texture bank
 * (bank = index + 1, below SOFT_GPU_BANKS) and arena. acquire() marks an
 * entry used the moment a zone takes it, so every model a frame asks for (ten
 * at most) stays put until that frame is drawn. */
#define CACHE 12

typedef struct {
    int card;      /* one-based card id; 0 when the entry is free */
    int position;  /* 0 face-up attack, 1 face-up defence */
    unsigned used; /* frame number of the last draw, for replacement */
    int bank;      /* its texture bank in the software GPU */
    u8 *arena;
    ModelSlot slot;
    int stepped;   /* the animation is advanced once a frame, not once a draw */
    /* Where the body sits around the model's own origin, and the scale it is
     * drawn at; both from measure(), once, when it is loaded. */
    int raw_x, raw_y, raw_z;    /* where its body sits around its origin */
    int body_x, body_y, body_z; /* the same, at the scale it is drawn */
    int scale;
    int natural;   /* its height in pixels at 1:1 in the middle of the field */
    /* How it stands on a card of the battle presentation (battle_pose). */
    int battle_scale, battle_yaw, battle_pixels;
    int battle_x, battle_y, battle_z;
    int battle_ox, battle_oy; /* its outline's middle and foot on the screen */
    int fade;      /* 0 opaque to 255 gone, as it is drawn now */
    u32 packet_bytes; /* the most its packets have taken in one sort */
} Monster;

static const MemoriesModHost *host;
static Monster cache[CACHE];
static u8 *record;             /* one MODEL.MRG record, read whole */
static int mrg_start = -2;
static unsigned frame;
static int inside;
static DisplayObject *dimmed[DUEL_SIDE_COUNT]; /* the big cards draw_battle dimmed */

/* The summon (summon_on), per monster zone, side * 5 + zone: the card id it
 * last saw stand there, whether its card has come down onto the zone (a card
 * put down drops from above for about 25 frames; it has landed once its
 * height has held for SETTLE_FRAMES), and how many frames the monster has
 * been drawn since. A new id starts it again; an emptied, face-down or magic
 * zone forgets it. */
#define SUMMON_ZONES 10
typedef struct {
    int card;
    int landed;
    int y;            /* the card's height last frame, while it may be falling */
    unsigned settled; /* frames that height has held */
    unsigned elapsed;
} Summon;
static Summon summons[SUMMON_ZONES];

static void reset(void)
{
    int i;
    for (i = 0; i < CACHE; i++) {
        cache[i].card = 0;
    }
    dimmed[0] = dimmed[1] = NULL; /* a state was loaded over them */
    memset(summons, 0, sizeof(summons));
    FieldArt_Reset();
}

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!host->log_enabled(host)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    host->log(host, "%s", message);
}

/* Knobs for tuning the look, kept as the mod's settings (mod.3d-monsters.<key>
 * in the settings file) and tried for one run from the environment:
 * MEMORIES_MOD_3D_MONSTERS_DEPTH=... (modapi.h). */
static int tunable(const char *key, int fallback)
{
    return host->setting(host, key, fallback);
}

/* Model_LoadMonsterMerge's own arithmetic: the three id ranges with no record
 * are rejected and every id above one is biased down. */
static int mrg_record(int model)
{
    if (model < 0 || model >= MODEL_MRG_ID_END ||
        (model >= MODEL_MRG_FIRST_GAP_START && model < MODEL_MRG_FIRST_GAP_END) ||
        (model >= MODEL_MRG_SECOND_GAP_START && model < MODEL_MRG_SECOND_GAP_END) ||
        model == MODEL_MRG_SINGLE_GAP_ID) {
        return -1;
    }
    if (model >= MODEL_MRG_LAST_ID) {
        model--;
    }
    if (model >= MODEL_MRG_SECOND_GAP_END) {
        model -= MODEL_MRG_GAP_SIZE;
    }
    if (model >= MODEL_MRG_FIRST_GAP_END) {
        model -= MODEL_MRG_GAP_SIZE;
    }
    return model;
}

static u16 scratch_pixels[BLOCK_W * BLOCK_H];
static u16 saved_block[BLOCK_W * BLOCK_H], saved_row[ROW_W];

static void bank_put(u16 *bank, int x, int y, int w, int h, const u16 *pixels)
{
    int row;
    for (row = 0; row < h; row++) {
        memcpy(bank + (size_t)(y + row) * SOFT_GPU_WIDTH + x, pixels + (size_t)row * w, (size_t)w * 2);
    }
}

static void load_rect(int x, int y, int w, int h, const void *pixels)
{
    RECT rect;
    rect.x = (short)x;
    rect.y = (short)y;
    rect.w = (short)w;
    rect.h = (short)h;
    LoadImage(&rect, (u32 *)pixels);
}

static void store_rect(int x, int y, int w, int h, void *pixels)
{
    RECT rect;
    rect.x = (short)x;
    rect.y = (short)y;
    rect.w = (short)w;
    rect.h = (short)h;
    StoreImage(&rect, (u32 *)pixels);
}

/* The record's image phases: one 64x16 block a sector, filling the texture
 * band from the top and stepping a column to the right at its end. This is
 * the file-transfer runtime's phase-2 advance with the x-advance bit clear,
 * which is how the MODEL phases program it. */
static const u8 *upload_blocks(const u8 *at, int sectors, int x)
{
    int y = 0x100, i;
    for (i = 0; i < sectors; i++, at += SECTOR) {
        load_rect(x, y, 0x40, 0x10, at);
        y += 0x10;
        if ((y & 0xFF) == 0) {
            y = (y ^ 0x100) & 0x100;
            x += 0x40;
        }
    }
    return at;
}

/* Replay func_80056D7C's seventeen phases for slot 0 into this monster's
 * arena. `position` is the record's alternate selector, which chooses between
 * the two stance variants. */
/* `slot_index` chooses the VRAM column (slot 1's block sits 0x100 to the
 * right) and, when `full`, the sequence bank goes to its slot block too. */
static void run_record_slot(Monster *monster, const u8 *at, int position, int slot_index, int full)
{
    FileTransferDescriptor descriptor;
    u8 *arena = monster->arena;
    int xbase = slot_index << 8;

    memcpy(arena, at, 96 * SECTOR);                       /* 0: model data */
    at += 96 * SECTOR;
    at = upload_blocks(at, 48, xbase);                    /* 1: textures */
    memcpy(D_801DD000, at, 2 * SECTOR);                   /* 2: palette stage */
    at += 2 * SECTOR;
    load_rect(xbase, 0xF8, 0x100, 8, D_801DD000);         /* 3: palettes */
    memcpy(D_801DE000, at, SECTOR);
    at += SECTOR;
    if (position == 0) {                                  /* 4: stance 0 */
        load_rect(ROW_X, ROW_Y, ROW_W, 1, D_801DE000);
        at = upload_blocks(at, 16, xbase + 0xC0);
    } else {
        at += 16 * SECTOR;
    }
    memcpy(D_801DD000, at, SECTOR);                       /* 5 */
    at += SECTOR;
    if (position == 1) {                                  /* 6: stance 1 */
        load_rect(ROW_X, ROW_Y, ROW_W, 1, D_801DD000);
        at = upload_blocks(at, 16, xbase + 0xC0);
    } else {
        at += 16 * SECTOR;
    }
    if (position == 0) {                                  /* 7: stance-0 module */
        memcpy(arena + ARENA_VARIANT, at, 10 * SECTOR);
    }
    at += 10 * SECTOR;
    at += 10 * SECTOR;                                    /* 8: the other slot */
    if (position == 1) {                                  /* 9: stance-1 module */
        memcpy(arena + ARENA_VARIANT, at, 10 * SECTOR);
    }
    at += 10 * SECTOR;
    at += 10 * SECTOR;                                    /* 10 */
    memcpy(arena + ARENA_PRIMARY, at, 2 * SECTOR);        /* 11: primary module */
    at += 2 * SECTOR;
    at += 2 * SECTOR;                                     /* 12 */
    if (full) {                                           /* 13: sequence bank */
        memcpy((void *)(uintptr_t)(0x801A8000u + (unsigned)slot_index * 0x800u), at, SECTOR);
    }
    at += SECTOR;
    at += 50 * SECTOR;                                    /* 14: voices */
    memcpy(D_801DD000, at, SECTOR);                       /* 15: metadata */

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.position = (u32)position;
    descriptor.callback_data = (void *)(uintptr_t)slot_index;
    func_80056D7C(&descriptor, 16);                       /* 16: into the slot */
}

static void run_record(Monster *monster, const u8 *at, int position)
{
    run_record_slot(monster, at, position, 0, 0);
}

/* The model runtime's own setup phases, which end with the slot ready. */
static int run_setup(void)
{
    int guard;
    for (guard = 0; guard < 64; guard++) {
        if (func_80058DD8(0) == 1) {
            return 1;
        }
        func_80056828(0);
    }
    return 0;
}

/* Take the block of VRAM the record filled into this monster's bank, so that
 * every monster on the field keeps its textures at once. Their coordinates
 * are the ones the model's primitives already carry: a bank is VRAM-shaped. */
static void keep_textures(Monster *monster)
{
    u16 *bank = SoftGpu_Bank(monster->bank);
    if (!bank) {
        return;
    }
    store_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, scratch_pixels);
    bank_put(bank, BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, scratch_pixels);
    store_rect(ROW_X, ROW_Y, ROW_W, 1, scratch_pixels);
    bank_put(bank, ROW_X, ROW_Y, ROW_W, 1, scratch_pixels);
}

/* Put the duel's staging buffer and its pixels back. */
static void give_back(const u8 *staging, size_t length)
{
    memcpy(D_801DD000, staging, length);
    load_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, saved_block);
    load_rect(ROW_X, ROW_Y, ROW_W, 1, saved_row);
}

static int load_monster(Monster *monster, int card, int position)
{
    uint64_t started, finished;
    ModelSlot *slot = &D_800F2C40[0];
    static u8 staging[0x2000]; /* the loader's own scratch, put back after */
    u8 *payload_base;
    int model = mrg_record(card - 1), sectors;

    if (model < 0) {
        return 0;
    }
    if (mrg_start == -2) {
        mrg_start = host->disc_file_start(host, "\\DATA\\MODEL.MRG;1");
        say("MODEL.MRG starts at sector %d\n", mrg_start);
    }
    if (mrg_start < 0) {
        return 0;
    }
    started = host->now_us(host);
    if (!record && !(record = malloc(RECORD_SECTORS * SECTOR))) {
        return 0;
    }
    sectors = host->disc_read(host, mrg_start + model * RECORD_SECTORS, RECORD_SECTORS, record);
    if (sectors != RECORD_SECTORS) {
        say("card %d: read %d of %d sectors\n", card, sectors, RECORD_SECTORS);
        return 0;
    }

    /* The slot is reset exactly as Model_LoadMonsterMerge resets it, with the
     * flag that skips the sound bank and leaves the three control-module
     * command words at -1 so no module is ever called. */
    func_8004CB0C(0, 0, 0, 4);
    slot->field_DF8 = (u16)(card - 1);
    slot->field_E1D = 0x80;
    slot->field_DFA = 0;
    slot->field_DFC = 0;
    slot->field_DFE = (u8)position;
    slot->field_DFF = 0;
    slot->field_E14 = 0;

    /* The record's phases fill the loader's staging buffer and the model area
     * of VRAM, both of which belong to the duel. Keep what is in them. */
    memcpy(staging, D_801DD000, sizeof(staging));
    store_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, saved_block);
    store_rect(ROW_X, ROW_Y, ROW_W, 1, saved_row);
    run_record(monster, record, position);

    /* func_80056828's first phase takes the payload from the slot-0 arena
     * word; point it at this monster's. The call is synchronous, so nothing
     * else can see the substitution. */
    payload_base = D_80010000;
    D_80010000 = monster->arena;
    if (!run_setup()) {
        D_80010000 = payload_base;
        give_back(staging, sizeof(staging));
        say("card %d: setup did not finish (phase %d)\n", card, slot->field_E14);
        return 0;
    }
    D_80010000 = payload_base;

    slot->field_DE8 = (s32)(monster->arena + ARENA_DATA_A);
    slot->field_DEC = (s32)(monster->arena + ARENA_DATA_B);
    monster->slot = *slot;
    monster->card = card;
    monster->position = position;
    keep_textures(monster);
    give_back(staging, sizeof(staging));
    finished = host->now_us(host);
    say("card %d stance %d loaded in %d us: %d units, %d parts, animation %d\n", card, position,
        (int)(finished - started),
        slot->field_E1A, slot->field_E1B, slot->field_BF5);
    return 1;
}

static void fit(Monster *monster);

static Monster *acquire(int card, int position)
{
    Monster *monster = NULL;
    int i;
    for (i = 0; i < CACHE; i++) {
        if (cache[i].card == card && cache[i].position == position) {
            cache[i].used = frame;
            return &cache[i];
        }
    }
    for (i = 0; i < CACHE; i++) {
        if (!cache[i].card) {
            monster = &cache[i];
            break;
        }
        if (!monster || cache[i].used < monster->used) {
            monster = &cache[i];
        }
    }
    if (!monster->arena) {
        uintptr_t wanted = ARENA_BASE + (unsigned)(monster - cache) * ARENA_SIZE;
        u8 *got = host->map_fixed(host, wanted, ARENA_SIZE);
        if (!got) {
            say("no arena at %p\n", (void *)wanted);
            return NULL;
        }
        monster->arena = got;
    }
    monster->bank = (int)(monster - cache) + 1;
    monster->card = 0;
    monster->battle_scale = 0;
    monster->packet_bytes = 0;
    if (!load_monster(monster, card, position)) {
        return NULL;
    }
    fit(monster);
    /* Taken for this frame: another zone missing before the draws must not
     * pick this entry again and leave the first zone the later monster. */
    monster->used = frame;
    return monster;
}

/* Every unit's world matrix is cached against the frame counter, so the same
 * monster standing on two cards would otherwise be drawn twice in one place. */
static void forget_coordinates(ModelSlot *slot)
{
    int i;
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        while (unit && unit->flg) {
            unit->flg = 0;
            unit = unit->super;
        }
    }
}

static void place(ModelSlot *slot, int x, int y, int z, int yaw, int scale)
{
    VECTOR size;
    forget_coordinates(slot);
    func_8005A4C4(slot, x, y, z, yaw);
    size.vx = size.vy = size.vz = scale;
    size.pad = 0;
    func_8005922C(slot->field_D18, &size);
}

/* Point a run of freshly sorted packets at a monster's texture bank: bits
 * 11-14 of a primitive's texture-page word, which the hardware ignores and
 * retail always leaves zero. The page word is the second corner's texture
 * word; the palette word beside it needs nothing, because a bank holds the
 * palettes at their own coordinates too. */
static void stamp_bank(const u32 *from, const u32 *to, int bank, int fade)
{
    while (from < to) {
        unsigned words = *from >> 24, at = 1;
        while (at <= words) {
            u32 command = from[at];
            unsigned op = command >> 24, length = Memories_GpuCommandWords(command), corner;
            if (!length || at + length > words + 1) {
                break;
            }
            if (op >= 0x24 && op <= 0x3f && (op & 4)) {
                unsigned corners = (op & 8) ? 4 : 3, index = at + 1;
                for (corner = 0; corner < corners; corner++) {
                    if ((op & 0x10) && corner) {
                        index++;
                    }
                    index++;
                    if (corner == 1) {
                        ((u32 *)from)[index] |= (u32)bank << 27;
                    } else if (corner == 2) {
                        /* The spare half beside the third corner's texels:
                         * how far the polygon fades (soft_gpu.h). */
                        ((u32 *)from)[index] = (from[index] & 0xffffu) |
                                               ((fade ? SOFT_GPU_FADE | (u32)fade : 0u) << 16);
                    }
                    index++;
                }
            }
            at += length;
        }
        from += words + 1;
    }
}

/* The measuring table: the packet area of the frame buffer that is about to
 * be cleared for the next frame, which is free until then. Its packets are
 * read for their screen coordinates and never drawn. */
#define TAGS_BYTES 0x10000
#define TABLE_AT TAGS_BYTES
#define PACKETS_AT (TAGS_BYTES + 0x20)
static u8 *scratch;

/* Sorting a monster into the game's own model ordering table is all the
 * drawing there is: the frame it belongs to has not been sent to the GPU yet.
 * D_800E9D98[0] is the table func_800540B4 uses for slots 0 and 1, which the
 * battle presentation draws its duellists into.
 *
 * The field's cards and a model measure depth differently in that table: a
 * card is sorted at a sixteenth of its distance (func_80015EF4), a model's
 * primitives at a quarter of theirs. So the monster is sorted at the model's
 * own depth scale into a table of its own (the scratch area; nothing else is
 * in it), which keeps its parts in the order the battle presentation draws
 * them, and that run of packets goes into the game's table whole, at a
 * quarter of its nearest entry -- the card's scale -- and DEPTH_STEPS
 * nearer, which draws it on the card rather than under it. Quartering the
 * GTE's Z factors instead, as the first version did, put a monster's parts
 * four to an entry, and parts sharing an entry draw in the order they were
 * sorted rather than by depth: limbs and cloth showed through the body and
 * flickered as the animation carried them across entries.
 *
 * The run goes into `into` at entry `at`, or, when `at` is negative, at the
 * depth just described; draw_battle puts it over a big card that way. */
#define LINK_MASK 0xFFFFFFu
#define LINK_END LINK_MASK
#define GUEST_LINK(link) ((u32 *)(uintptr_t)(0x80000000u | (link)))

/* A monster's packets go at the end of the game's own packet buffer for the
 * frame, and nothing stops them at its end: the second buffer is followed by
 * the display environment, the ordering-table pointers and the frame service
 * callbacks, so writing past it crashes the next frame. Ten monsters on the
 * field under Raigeki's lightning came to 584 bytes too many. A monster is
 * left out of a frame that has no room for the most its packets have taken
 * yet (measured first by fit()), a quarter as much again, and PACKET_SLACK.
 * The models here take 4 to 11 KB each. */
#define PACKET_SLACK 0x400u
#define PACKET_FLOOR 0x1000u

static u32 packet_room(void)
{
    u32 base = (u32)(uintptr_t)D_800A5768, at = D_800FE240;
    u32 end = base + (at < base + GRAPHICS_PACKET_BUFFER_SIZE ? 1 : 2) * GRAPHICS_PACKET_BUFFER_SIZE;
    return at < base || at >= end ? 0 : end - at;
}

static int packets_fit(const Monster *monster)
{
    static unsigned said = ~0u;
    u32 need = monster->packet_bytes > PACKET_FLOOR ? monster->packet_bytes : PACKET_FLOOR;
    need += need / 4 + PACKET_SLACK;
    if (packet_room() >= need) {
        return 1;
    }
    if (said != frame) {
        said = frame;
        say("card %d left out: %u bytes of packets left, it needs %u\n", monster->card, packet_room(), need);
    }
    return 0;
}

static void sort_monster(Monster *monster, GsOT *into, int at)
{
    const u32 *from = (const u32 *)(uintptr_t)D_800FE240;
    GsOT *live = (GsOT *)D_800E9D98[0];
    GsOT *table = (GsOT *)(scratch + TABLE_AT);
    u32 *tags = (u32 *)scratch, *entry, *last = NULL, first = LINK_END, end;
    int entries, nearest = 0, i;

    if (!packets_fit(monster)) {
        return;
    }
    table->length = 14;
    table->org = (GsOT_TAG *)scratch;
    table->offset = 0;
    table->point = 0;
    GsClearOt(0, 0, table);
    end = tags[0] & LINK_MASK; /* what entry 0 leads to: the table's tail */
    D_800E9D98[0] = table;
    func_800540B4(0);
    D_800E9D98[0] = live;
    if (D_800FE240 - (u32)(uintptr_t)from > monster->packet_bytes) {
        monster->packet_bytes = D_800FE240 - (u32)(uintptr_t)from;
    }
    entries = TAGS_BYTES / 4; /* func_800540B4 leaves the length at 12 */

    /* The table's entries run from the far end down to entry 0; chain the
     * packets through them, leaving the empty entries out. */
    for (i = entries - 1; i >= 0; i--) {
        u32 link = tags[i] & LINK_MASK;
        u32 stop = i ? (u32)(uintptr_t)&tags[i - 1] & LINK_MASK : end;
        while (link != stop) {
            u32 *packet = GUEST_LINK(link);
            if (last) {
                *last = (*last & ~LINK_MASK) | link;
            } else {
                first = link;
            }
            last = packet;
            nearest = i;
            link = *packet & LINK_MASK;
        }
    }
    if (!last) {
        return;
    }
    if (at < 0) {
        at = nearest / 4 - tunable("depth", DEPTH_STEPS);
    }
    at = at < 0 ? 0 : at >= (1 << into->length) ? (1 << into->length) - 1 : at;
    entry = (u32 *)into->org + at;
    *last = (*last & ~LINK_MASK) | (*entry & LINK_MASK);
    *entry = (*entry & ~LINK_MASK) | first;
    stamp_bank(from, (const u32 *)(uintptr_t)D_800FE240, monster->bank, monster->fade);
}


static int packet_height(const u32 *from, const u32 *to);

static int sort_aside(Monster *monster)
{
    GsOT *table = (GsOT *)(scratch + TABLE_AT);
    void *live = D_800E9D98[0];
    u32 base = D_800FE240;
    int height;
    table->length = 14;
    table->org = (GsOT_TAG *)scratch;
    table->offset = 0;
    table->point = 0;
    GsClearOt(0, 0, table);
    GsSetWorkBase((PACKET *)(scratch + PACKETS_AT));
    D_800E9D98[0] = table;
    func_800540B4(0);
    height = packet_height((const u32 *)(scratch + PACKETS_AT), (const u32 *)(uintptr_t)D_800FE240);
    if (D_800FE240 - (u32)(uintptr_t)(scratch + PACKETS_AT) > monster->packet_bytes) {
        monster->packet_bytes = D_800FE240 - (u32)(uintptr_t)(scratch + PACKETS_AT);
    }
    D_800E9D98[0] = live;
    D_800FE240 = base;
    return height;
}

/* How many scan lines the packets just sorted aside cover. This is the only
 * thing that answers "how big is this monster": a model's parts say where its
 * joints are, not how far its skin reaches, and Korogashi is one big ball
 * around a single joint. */
static struct {
    int left, top, right, bottom;
} bounds; /* what packet_height last covered, in screen pixels */

static int packet_height(const u32 *from, const u32 *to)
{
    int top = 0x7fff, bottom = -0x7fff, left = 0x7fff, right = -0x7fff;
    while (from < to) {
        unsigned words = *from >> 24, at = 1;
        while (at <= words) {
            u32 command = from[at];
            unsigned op = command >> 24, length = Memories_GpuCommandWords(command), corner;
            if (!length || at + length > words + 1) {
                break;
            }
            if (op >= 0x20 && op <= 0x3f) {
                unsigned corners = (op & 8) ? 4 : 3, index = at + 1;
                for (corner = 0; corner < corners; corner++) {
                    int x, y;
                    if ((op & 0x10) && corner) {
                        index++; /* every further corner of a shaded polygon */
                    }
                    x = (short)(from[index] & 0xffff);
                    y = (short)(from[index] >> 16);
                    left = x < left ? x : left;
                    right = x > right ? x : right;
                    top = y < top ? y : top;
                    bottom = y > bottom ? y : bottom;
                    index++;
                    if (op & 4) {
                        index++; /* texture coordinates */
                    }
                }
            }
            at += length;
        }
        from += words + 1;
    }
    bounds.left = left;
    bounds.top = top;
    bounds.right = right;
    bounds.bottom = bottom;
    return bottom > top && right > left ? bottom - top : 0;
}

/* Where the body sits around the model's own origin. A duel model is built
 * around the point between the two duellists, so slot 0's body stands some
 * 250 units up the field from the origin func_8005A4C4 places, and nothing in
 * the record says by how much -- the parts do, once their world matrices are
 * built. y is the feet, which is y at its largest because it grows downwards. */
static void measure_body(Monster *monster)
{
    ModelSlot *slot = &D_800F2C40[0];
    s32 feet = -0x7fffffff, sum_x = 0, sum_z = 0;
    int parts = 0, i;

    *slot = monster->slot;
    place(slot, 0, 0, 0, 0, MODEL_FIXED_ONE);
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        MATRIX world;
        if (!unit) {
            continue;
        }
        GsGetLwUnit(unit, &world);
        feet = world.t[1] > feet ? world.t[1] : feet;
        sum_x += world.t[0];
        sum_z += world.t[2];
        parts++;
    }
    if (parts) {
        monster->raw_x = sum_x / parts;
        monster->raw_y = feet;
        monster->raw_z = sum_z / parts;
    }
}

static void scale_body(Monster *monster)
{
    monster->body_x = monster->raw_x * monster->scale / MODEL_FIXED_ONE;
    monster->body_y = monster->raw_y * monster->scale / MODEL_FIXED_ONE;
    monster->body_z = monster->raw_z * monster->scale / MODEL_FIXED_ONE;
}

/* Pick the scale this monster is drawn at, once, when it is loaded: one
 * monster is several times another end to end, and at a single scale either
 * the small ones are specks or the big ones cover the field. Each is sized
 * until it is about TALL_PIXELS high in the middle of the field, then the
 * square root of that answer is taken against the middle size, which keeps
 * the order -- a dragon still towers over Sangan -- while bringing the range
 * in. The monster is sorted but never drawn: the packets are read and thrown
 * away. */
static void fit(Monster *monster)
{
    ModelSlot *slot = &D_800F2C40[0];
    int target = tunable("pixels", TALL_PIXELS), attempt, wide, height = 0;

    monster->scale = MODEL_FIXED_ONE / 2;
    monster->natural = MIDDLING_PIXELS;
    measure_body(monster);
    for (attempt = 0; attempt < 5; attempt++) {
        int wanted;
        scale_body(monster);
        *slot = monster->slot;
        place(slot, -monster->body_x, -monster->body_y, -monster->body_z, 0, monster->scale);
        height = sort_aside(monster);
        if (height <= 0) {
            break;
        }
        wanted = monster->scale * target / height;
        if (wanted > monster->scale * 15 / 16 && wanted < monster->scale * 17 / 16) {
            break;
        }
        monster->scale = wanted < SCALE_SMALLEST ? SCALE_SMALLEST
                       : wanted > SCALE_LARGEST ? SCALE_LARGEST : wanted;
    }
    if (height > 0 && monster->scale > 0) {
        /* The size that would make it TALL_PIXELS high says how big the
         * monster is in itself; drawing it at the square root of that against
         * a middling monster keeps the order -- a dragon still towers over
         * Sangan -- while bringing a sevenfold range down to about two and a
         * half. */
        double natural = (double)target * MODEL_FIXED_ONE / monster->scale;
        monster->natural = (int)natural;
        monster->scale = (int)(MODEL_FIXED_ONE * target / sqrt(natural * MIDDLING_PIXELS));
        monster->scale = monster->scale < SCALE_SMALLEST ? SCALE_SMALLEST
                       : monster->scale > SCALE_LARGEST ? SCALE_LARGEST : monster->scale;
    }
    for (wide = 0; wide < (int)(sizeof(WIDE_MONSTERS) / sizeof(WIDE_MONSTERS[0])); wide++) {
        if (WIDE_MONSTERS[wide].card == monster->card) {
            monster->scale = monster->scale * WIDE_MONSTERS[wide].share / MODEL_FIXED_ONE;
        }
    }
    monster->scale = monster->scale * tunable("scale", MODEL_FIXED_ONE) / MODEL_FIXED_ONE;
    scale_body(monster);
    say("card %d fits %d pixels at %d/4096, body at %d,%d,%d, %u bytes of packets\n", monster->card, height,
        monster->scale, monster->body_x, monster->body_y, monster->body_z, (unsigned)monster->packet_bytes);
}

/* How far above its card a monster floats, in field units (y is down).
 * LIFT_PIXELS is what that comes to on the 240-line picture from the view the
 * duel is played from; MEMORIES_MODS_LIFT overrides the units directly. */
#define LIFT_PIXELS 8
#define LIFT_UNITS_PER_PIXEL 2
static int lift(void)
{
    return tunable("lift", LIFT_PIXELS * LIFT_UNITS_PER_PIXEL);
}

/* `share` of its fitted size (MODEL_FIXED_ONE is the whole), faded by `fade`
 * (0 opaque to 255 gone) and `lifted` field units above the card: the summon
 * grows it from its card; whole, opaque and at lift() it is drawn as ever. */
static void draw_monster(Monster *monster, int x, int z, int yaw, int share, int fade, int lifted)
{
    ModelSlot *slot = &D_800F2C40[0];
    int turned = yaw == MODEL_ANGLE_HALF_TURN;
    int body_x = monster->body_x * share / MODEL_FIXED_ONE;
    int body_y = monster->body_y * share / MODEL_FIXED_ONE;
    int body_z = monster->body_z * share / MODEL_FIXED_ONE;

    *slot = monster->slot;
    /* The body offset was measured facing up the field, so turning the
     * monster turns it too. */
    place(slot, turned ? x + body_x : x - body_x, -body_y - lifted,
          turned ? z + body_z : z - body_z, yaw, monster->scale * share / MODEL_FIXED_ONE);
    monster->fade = fade;
    sort_monster(monster, (GsOT *)D_800E9D98[0], -1);
    monster->fade = 0;
    if (!monster->stepped) {
        func_800556E8(0);
        monster->stepped = 1;
    }
    monster->slot = *slot;
    monster->used = frame;
}

/* The duel field seen from above is the one place this draws. Its own
 * card-drawing service is installed only while the field is up and the arena
 * model has to be loaded; the rest is the camera. Choosing a zone, using a
 * card and the fusion presentations all fly it down to eye level with the
 * mat (pitch 1022 of a 4096-unit turn, against 256 for the view the duel is
 * played from) and put their own panels on the screen, and a monster
 * standing on a card has nothing to stand on there. */
#define FIELD_PITCH 512

static int duel_field_up(void)
{
    static int phase = -1, distance = -1, pitch = -1, angle = -1;
    if (host->log_enabled(host) && (phase != (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) ||
                      distance != D_800F2848.field_00 || pitch != D_800F2848.field_04 ||
                      angle != D_800F2848.angle)) {
        phase = gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK;
        distance = D_800F2848.field_00;
        pitch = D_800F2848.field_04;
        angle = D_800F2848.angle;
        say("duel scene phase %d, camera %d away, pitch %d, angle %d, side %d, projection %d\n",
            phase, distance, pitch, angle, D_8009B1D5, D_800F2848.projection);
    }
    return D_800E9DB0[3] == Duel_DrawFieldCards && D_800F2C40[2].field_E1F != 0 &&
           D_800F2848.field_04 < tunable("pitch", FIELD_PITCH);
}

typedef struct {
    Monster *monster;
    int x, z, yaw;
    int summon; /* its zone in summons[] */
} Standing;

/* The five monster zones of a side, in card-record order. */
#define MONSTER_ZONES 5
#define SIDE_ZONE(side, zone) ((side) ? 20 + (zone) : 5 + (zone))
/* Records 0-14 are the player's, whose view is the quarter-turn camera. */
#define DUEL_SIDE_PLAYER 0

/* The battle presentation (DuelScene_UpdateBattle, scene state 9) lays the
 * attacker's card and the defender's side by side, big, over the faded field:
 * D_800E9EF0[2] on the left and [3] on the right, made from the field cards
 * D_800E9EF0[0] and [1]. Each monster stands on its big card here, the two
 * turned towards each other, and the cards are dimmed under them.
 *
 * The big cards are sprites in ordering table 1, the interface's, sorted
 * DisplayObject_SetDepthOffset(-10) behind its usual depth, with the picture
 * and the print one entry nearer (func_80028B08). The model table is not
 * drawn at all once the field has faded out behind them
 * (Fade_StartOutKeepOverlayAndHideSecondaryTables), so a monster goes into
 * table 1 whole, BATTLE_DEPTH_STEPS nearer than its card: over the picture,
 * under the damage numbers and the flashes.
 *
 * The dimming is the card's own colour word, the one the battle's last step
 * turns down to fade the cards out: the card and everything printed on it go
 * darker together, and that fade simply starts from where the dimming left
 * it. The colour goes back to retail's whenever this lets go of a card that is
 * still up. */
#define DUEL_SCENE_BATTLE 9
#define BATTLE_STEP_TO_ARENA 5    /* fading out to the 3D battle in the arena */
#define BATTLE_STEP_DESTROY 10    /* the losing card burns */
#define BATTLE_STEP_END 11        /* the cards fade out */
#define BATTLE_OUTRO 16           /* frames the monsters fade out and the cards light up over */
#define BATTLE_CARD_WIDTH 0x8C
#define BATTLE_CARD_FEET 0xAC     /* the feet, down from the card's top edge */
#define BATTLE_BOX_WIDTH 0x96     /* the widest a monster stands on its card */
#define BATTLE_BACK 8             /* each stands this far back from the middle of its card */
#define BATTLE_SMALLEST 0.7       /* the least share of that box it gets */
#define BATTLE_LARGEST 1.6        /* and the most */
#define BATTLE_TALLEST 188        /* from the card's feet line to the top of the screen */
#define BATTLE_DEPTH_STEPS 2
#define BATTLE_PIXELS 160         /* a middling monster's height on its card */
#define BATTLE_DIM 50             /* percent */
#define CARD_COLOUR 0x808080u
/* The screen the pass projects through: a camera looking straight at the
 * cards, BATTLE_DISTANCE units in front of them. */
#define BATTLE_PROJECTION 0x200
#define BATTLE_DISTANCE 0x800
#define BATTLE_SCALE_LARGEST 0x6000
/* How far each monster is turned from facing the camera towards the other
 * card: a little short of a quarter turn, so the two face each other and the
 * camera still sees their faces. */
#define BATTLE_TURN (MODEL_ANGLE_QUARTER_TURN * 2 / 3)

extern u8 D_8009B174;          /* the battle's step, in the low nibble */
extern u16 D_8009B178[2];      /* the two cards' saved flags */
extern s8 D_8009B1B9;          /* the side whose card is destroyed */
extern u8 D_8009B229;          /* the battle goes on to the 3D arena */
extern MATRIX D_800FE128, D_800FE148; /* GsLIGHTWSMATRIX, GsWSMATRIX */

/* The battle sets up its projection once and draws its damage numbers and
 * glows through it for the rest of the presentation, so this pass puts the
 * GTE and the world-screen matrices back as it found them. */
static u8 saved_gte[512];
static MATRIX saved_world_screen, saved_light_world;

static void keep_geometry(int restore)
{
    unsigned size;
    void *registers = Gte_StateData(&size);
    size = size < sizeof(saved_gte) ? size : sizeof(saved_gte);
    if (restore) {
        memcpy(registers, saved_gte, size);
        D_800FE148 = saved_world_screen;
        D_800FE128 = saved_light_world;
    } else {
        memcpy(saved_gte, registers, size);
        saved_world_screen = D_800FE148;
        saved_light_world = D_800FE128;
    }
}

static u32 dim_colour(void)
{
    int dim = tunable("battle_dim", BATTLE_DIM), level;
    dim = dim < 0 ? 0 : dim > 100 ? 100 : dim;
    level = 0x80 * (100 - dim) / 100;
    return (u32)level * 0x010101u;
}

static void undim(int side)
{
    DisplayObject *card = dimmed[side];
    dimmed[side] = NULL;
    if (card && D_800E9EF0[side + 2] == card && (card->field_0C & 0xFFFFFFu) == dim_colour()) {
        card->field_0C = (card->field_0C & ~0xFFFFFFu) | CARD_COLOUR;
    }
}

static void dim(int side, DisplayObject *card)
{
    if (dimmed[side] != card) {
        undim(side);
    }
    dimmed[side] = card;
    card->field_0C = (card->field_0C & ~0xFFFFFFu) | dim_colour();
}

static void screen_to_world(int x, int y, int *wx, int *wy)
{
    *wx = (x - 0xA0) * BATTLE_DISTANCE / BATTLE_PROJECTION;
    *wy = (y - 0x78) * BATTLE_DISTANCE / BATTLE_PROJECTION;
}

static void battle_camera(void)
{
    GsRVIEW2 view;
    memset(&view, 0, sizeof(view));
    view.vpz = -BATTLE_DISTANCE;
    view.super = WORLD;
    GsSetRefView2(&view);
    SetGeomScreen(BATTLE_PROJECTION);
    SetGeomOffset(0xA0, 0x78);
    SetFarColor(0, 0, 0);
    SetFogNearFar(BATTLE_DISTANCE * 4, BATTLE_DISTANCE * 5, BATTLE_PROJECTION);
}

/* Where the body sits around the origin turned by `yaw`, at 1:1: the x and z
 * of its parts on average, and its feet. */
static void measure_turned(Monster *monster, int yaw, int *x, int *y, int *z)
{
    ModelSlot *slot = &D_800F2C40[0];
    s32 feet = -0x7fffffff, sum_x = 0, sum_z = 0;
    int parts = 0, i;

    *slot = monster->slot;
    place(slot, 0, 0, 0, yaw, MODEL_FIXED_ONE);
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        MATRIX world;
        if (!unit) {
            continue;
        }
        GsGetLwUnit(unit, &world);
        feet = world.t[1] > feet ? world.t[1] : feet;
        sum_x += world.t[0];
        sum_z += world.t[2];
        parts++;
    }
    *x = parts ? sum_x / parts : 0;
    *y = parts ? feet : 0;
    *z = parts ? sum_z / parts : 0;
}

/* The scale a monster stands on its big card at, once per facing, and where
 * its outline sits: it is fitted into a box the size of the card's picture
 * and print, BATTLE_BOX_WIDTH wide and `battle_pixels` high, whichever of the
 * two it meets first -- a dragon's wings or a vine's reach count as much as
 * its height, which is what kept them on the card. The box follows the
 * monster's size on the field (monster->natural), from BATTLE_SMALLEST to
 * BATTLE_LARGEST of it and never taller than BATTLE_TALLEST, so a dragon
 * still towers over an elf. Measured as fit() measures, from the packets,
 * and then placed by the outline rather than by the body: its middle over
 * the card's middle, its lowest point on the card's feet line. */
static void battle_pose(Monster *monster, int yaw)
{
    ModelSlot *slot = &D_800F2C40[0];
    int pixels = tunable("battle_pixels", BATTLE_PIXELS), raw_x, raw_y, raw_z, scale = monster->scale,
        attempt, wx, wy, box_w, box_h;
    double share = sqrt((double)monster->natural / MIDDLING_PIXELS);

    if (monster->battle_scale && monster->battle_yaw == yaw && monster->battle_pixels == pixels) {
        return;
    }
    share = share < BATTLE_SMALLEST ? BATTLE_SMALLEST : share > BATTLE_LARGEST ? BATTLE_LARGEST : share;
    box_w = (int)(BATTLE_BOX_WIDTH * share);
    box_h = (int)(pixels * share);
    box_h = box_h > BATTLE_TALLEST ? BATTLE_TALLEST : box_h;
    measure_turned(monster, yaw, &raw_x, &raw_y, &raw_z);
    screen_to_world(0xA0, 0x78, &wx, &wy);
    for (attempt = 0; attempt < 6; attempt++) {
        int height, width, wanted;
        *slot = monster->slot;
        place(slot, wx - raw_x * scale / MODEL_FIXED_ONE, wy - raw_y * scale / MODEL_FIXED_ONE,
              -raw_z * scale / MODEL_FIXED_ONE, yaw, scale);
        height = sort_aside(monster);
        width = bounds.right - bounds.left;
        if (height <= 0) {
            break;
        }
        wanted = scale * box_h / height;
        if (scale * box_w / width < wanted) {
            wanted = scale * box_w / width;
        }
        if (wanted > scale * 31 / 32 && wanted < scale * 33 / 32) {
            break;
        }
        scale = wanted < SCALE_SMALLEST ? SCALE_SMALLEST
              : wanted > BATTLE_SCALE_LARGEST ? BATTLE_SCALE_LARGEST : wanted;
    }
    monster->battle_scale = scale;
    monster->battle_yaw = yaw;
    monster->battle_pixels = pixels;
    monster->battle_x = raw_x * scale / MODEL_FIXED_ONE;
    monster->battle_y = raw_y * scale / MODEL_FIXED_ONE;
    monster->battle_z = raw_z * scale / MODEL_FIXED_ONE;
    /* Where the outline's foot and middle came out, from the point placed. */
    monster->battle_ox = (bounds.left + bounds.right) / 2 - 0xA0;
    monster->battle_oy = bounds.bottom - 0x78;
    say("card %d (natural %d) fits %dx%d on its battle card at %d/4096\n", monster->card, monster->natural,
        bounds.right - bounds.left, bounds.bottom - bounds.top, scale);
}

/* The monster a big card shows, or NULL: the card the presentation loaded
 * for it, in the stance its field card was in. */
static Monster *battle_monster(int side)
{
    int id = (s16)D_800EA0E8[side].field_30;
    /* For measuring, as `test` is on the field: any two monsters. */
    if (tunable("battle_test", 0)) {
        return acquire(tunable("battle_test", 0) + side * tunable("battle_test_step", 1), 0);
    }
    if (id <= 0 || ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
        return NULL;
    }
    return acquire(Cards_ModelId(id), (D_8009B178[side] & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0);
}

/* Whether a side's big card is up, settled and showing a monster. */
static DisplayObject *battle_card(int side)
{
    DisplayObject *card = D_800E9EF0[side + 2];
    int step = D_8009B174 & 0xF;
    /* The last step releases the field cards first thing; the big ones stay
     * until they have faded out. */
    if ((gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) != DUEL_SCENE_BATTLE ||
        (!D_800E9EF0[side] && step != BATTLE_STEP_END) ||
        !card || (card->flags & DISPLAY_OBJECT_RENDERABLE_MASK) != DISPLAY_OBJECT_RENDERABLE_MASK ||
        (card->flags & 4)) {
        return NULL; /* not the battle, no such card, or still fading in */
    }
    /* When the battle goes on to the arena the cards are up only a few
     * frames before the fade, and the arena has the monsters anyway. */
    if (D_8009B229 || step < 3 || step == BATTLE_STEP_TO_ARENA || step > BATTLE_STEP_END) {
        return NULL;
    }
    return card;
}

/* Whether the monster on a side's big card has gone: its card is burning.
 * The card stays dimmed until it is released, or it would light up for the
 * frames before the flames cover it. */
static int battle_lost(int side)
{
    return (D_8009B174 & 0xF) == BATTLE_STEP_DESTROY && D_8009B1B9 == side;
}

/* The pass for the battle presentation; 0 when it is not up. */
static int draw_battle(void)
{
    static ModelSlot borrowed;
    DisplayObject *cards[DUEL_SIDE_COUNT];
    Monster *monsters[DUEL_SIDE_COUNT];
    static int outro = -1; /* frames into the last step */
    u32 work_base;
    int side, count = 0, fade = 0;

    /* The battle's last step fades the cards out, each from its colour, by 8
     * a frame (DuelScene_UpdateBattle, case 11). Before that the monsters
     * fade out over BATTLE_OUTRO frames and the cards light up again: the
     * pass sets the colour the step will take 8 off next frame, which holds
     * the cards up that long, then lets the step fade them from full. */
    if ((D_8009B174 & 0xF) != BATTLE_STEP_END || !tunable("battle", 0)) {
        outro = -1;
    } else if (++outro >= BATTLE_OUTRO) {
        return 0;
    } else {
        int from = (int)(dim_colour() & 0xFF), level;
        dimmed[0] = dimmed[1] = NULL; /* the colour is the outro's now */
        level = from + (0x80 - from) * (outro + 1) / BATTLE_OUTRO;
        fade = 255 * (outro + 1) / BATTLE_OUTRO;
        for (side = 0; side < DUEL_SIDE_COUNT; side++) {
            DisplayObject *card = battle_card(side);
            if (card) {
                card->field_0C = (card->field_0C & ~0xFFFFFFu) | (u32)(level + 8) * 0x010101u;
            }
        }
    }

    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        cards[side] = tunable("battle", 0) ? battle_card(side) : NULL;
        monsters[side] = NULL;
        if (!cards[side]) {
            undim(side);
        } else {
            count++;
        }
    }
    if (!count) {
        return 0;
    }
    frame++;
    inside = 1;
    borrowed = D_800F2C40[0];
    work_base = D_800FE240;
    scratch = &D_800A5768[GsGetActiveBuff() * GRAPHICS_PACKET_BUFFER_SIZE];
    keep_geometry(0);
    battle_camera();

    count = 0;
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        if (cards[side] && battle_lost(side)) {
            dim(side, cards[side]);
        } else if (cards[side] && (monsters[side] = battle_monster(side)) != NULL) {
            monsters[side]->stepped = 0;
            if (outro < 0) {
                dim(side, cards[side]);
            }
        } else if (outro < 0) {
            undim(side);
        }
    }
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        Monster *monster = monsters[side];
        ModelSlot *slot = &D_800F2C40[0];
        /* The attacker is on the left and turns right; the defender turns left. */
        int yaw = side == 0 ? MODEL_ANGLE_FULL_TURN - BATTLE_TURN : BATTLE_TURN, wx, wy, at;
        GsOT *table = D_800E9D90[cards[side]->ot_index];
        if (!monster) {
            continue;
        }
        battle_pose(monster, yaw);
        screen_to_world(cards[side]->field_30.h.field_30 + BATTLE_CARD_WIDTH / 2 - monster->battle_ox +
                            (side == 0 ? -BATTLE_BACK : BATTLE_BACK),
                        cards[side]->field_30.h.field_32 + BATTLE_CARD_FEET - monster->battle_oy, &wx, &wy);
        *slot = monster->slot;
        place(slot, wx - monster->battle_x, wy - monster->battle_y, -monster->battle_z, yaw,
              monster->battle_scale);
        at = cards[side]->field_14 - (int)table->offset - BATTLE_DEPTH_STEPS;
        monster->fade = fade;
        sort_monster(monster, table, at < 0 ? 0 : at);
        monster->fade = 0;
        if (!monster->stepped) {
            func_800556E8(0);
            monster->stepped = 1;
        }
        monster->slot = *slot;
        monster->used = frame;
        count++;
    }

    D_800F2C40[0] = borrowed;
    if (!count) {
        D_800FE240 = work_base;
    }
    keep_geometry(1);
    inside = 0;
    return 1;
}

static void applied(int on)
{
    if (!on) {
        undim(0);
        undim(1);
        memset(summons, 0, sizeof(summons));
    }
}

/* The summon: a monster put face up grows out of its card, from SUMMON_FROM
 * of its size, fading in and rising to lift() over summon_frames frames,
 * eased out; meanwhile draw_card flashes its card for FLASH_FRAMES frames and
 * then leaves it out. The frames are those it is drawn in, so one put down
 * while the camera is away (choosing its zone) is summoned when the field
 * comes back. Off, or in the Card art style, nothing here runs. */
#define SUMMON_FROM (MODEL_FIXED_ONE / 5)
#define SUMMON_FRAMES 30
#define FLASH_FRAMES 8
#define SETTLE_FRAMES 3

static int summon_on(void)
{
    return tunable("summon", 1) && !tunable("style", 0);
}

static int summon_frames(void)
{
    int frames = tunable("summon_frames", SUMMON_FRAMES);
    return frames < 1 ? 1 : frames > 240 ? 240 : frames;
}

/* 1 - (1 - p)^2, all in MODEL_FIXED_ONE units: fast at first, settling. */
static int eased(int progress)
{
    int rest = MODEL_FIXED_ONE - progress;
    return MODEL_FIXED_ONE - rest * rest / MODEL_FIXED_ONE;
}

static void draw_frame(void)
{
    static ModelSlot borrowed;
    Standing standing[DUEL_SIDE_COUNT * MONSTER_ZONES];
    u32 work_base;
    int count = 0, i, side, zone, summoning, frames;

    if (inside) {
        return;
    }
    /* Card art has no battle-presentation equivalent (draw_battle, the
     * models standing on the attack cards): style gates it out here,
     * before it can run at all, the same way every other 3D-only setting
     * (scale, battle, battle_pixels, battle_dim) only ever gets read from
     * inside 3D-only code below -- none of it should have any effect while
     * Card art is showing, the same standard Card art's own settings are
     * held to the other way round. */
    if (tunable("style", 0)) {
        FieldArt_DrawFrame();
        return;
    }
    if (draw_battle()) {
        return;
    }
    if (!duel_field_up()) {
        return;
    }
    frame++;
    inside = 1;
    borrowed = D_800F2C40[0];
    work_base = D_800FE240;
    scratch = &D_800A5768[GsGetActiveBuff() * GRAPHICS_PACKET_BUFFER_SIZE];

    /* The projection the duel draws its own field with. */
    GsSetRefView2(&D_800F2848.view);
    SetGeomScreen(D_800F2848.projection);
    SetGeomOffset(0xA0, 0x6C);
    SetFarColor(0, 0, 0);
    SetFogNearFar(0x28A, 0x320, D_800F2848.projection);

    summoning = summon_on();
    frames = summon_frames();
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        for (zone = 0; zone < MONSTER_ZONES; zone++) {
            int index = SIDE_ZONE(side, zone);
            DuelCardRecord *card = &D_801A7AD8[index];
            Summon *summon = &summons[side * MONSTER_ZONES + zone];
            Monster *monster;
            int id = card->card_id;

            /* A field full of monsters, for measuring: every zone stands a
             * different one, which exercises the cache, the arenas and the
             * texture banks at once. */
            if (tunable("test", 0)) {
                id = tunable("test", 0) + zone * 2 + side;
            } else if (!(card->flags & DUEL_CARD_FLAG_OCCUPIED) ||
                (card->flags & DUEL_CARD_FLAG_FACE_DOWN) || id <= 0 ||
                ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
                summon->card = 0;
                continue; /* empty, face down, or a magic or trap card */
            }
            if (summon->card != card->card_id) {
                summon->card = card->card_id; /* put down, turned up or replaced */
                summon->landed = 0;
                summon->settled = 0;
                summon->y = card->object ? (s16)((DisplayObject *)card->object)->field_30.h.field_32 : 0;
                summon->elapsed = 0;
            }
            /* A card a card mod added stands as the retail card it is a
             * copy of: MODEL.MRG has the disc's monsters only. */
            if (!tunable("test", 0)) {
                id = Cards_ModelId(id);
            }
            /* The record carries a stance of its own for a monster in
             * defence, which is the one the battle presentation would use. */
            monster = acquire(id, (card->flags & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0);
            if (!monster) {
                continue;
            }
            /* Loaded while its card comes down, so landing costs nothing;
             * summoned once it has landed. */
            if (summoning && !summon->landed) {
                DisplayObject *object = card->object;
                int y = object ? (s16)object->field_30.h.field_32 : 0;
                summon->settled = y == summon->y ? summon->settled + 1 : 0;
                summon->y = y;
                if (object && summon->settled < SETTLE_FRAMES) {
                    continue;
                }
                summon->landed = 1;
                say("zone %d: card %d landed at height %d, summoned\n", side * MONSTER_ZONES + zone, summon->card, y);
            }
            standing[count].monster = monster;
            standing[count].x = D_800908A0[index].x;
            standing[count].z = D_800908A0[index].y;
            /* A duel model faces the player's edge of the mat at yaw 0 --
             * that is slot 0 of the battle presentation, which stands
             * up-field looking back -- so the player's own monsters turn
             * round to face the opponent. The facing is fixed to the side
             * that owns the zone, not to whose turn it is: the turn switch
             * swings the camera a half turn round the mat over 48 frames
             * and flips D_8009B1D5 a third of the way through, and choosing
             * by that snapped every monster round mid-swing and left the
             * player's showing their backs for the opponent's turn. */
            standing[count].yaw = side == DUEL_SIDE_PLAYER ? MODEL_ANGLE_HALF_TURN : 0;
            standing[count].summon = side * MONSTER_ZONES + zone;
            count++;
        }
    }

    for (i = 0; i < count; i++) {
        standing[i].monster->stepped = 0;
    }
    for (i = 0; i < count; i++) {
        Summon *summon = &summons[standing[i].summon];
        int share = MODEL_FIXED_ONE, fade = 0, lifted = lift();
        if (summoning && summon->elapsed < (unsigned)frames) {
            int progress = (int)(summon->elapsed * MODEL_FIXED_ONE / (unsigned)frames);
            int grown = eased(progress);
            share = SUMMON_FROM + (MODEL_FIXED_ONE - SUMMON_FROM) * grown / MODEL_FIXED_ONE;
            fade = 255 - 255 * progress / MODEL_FIXED_ONE;
            lifted = lifted * grown / MODEL_FIXED_ONE;
        }
        draw_monster(standing[i].monster, standing[i].x, standing[i].z, standing[i].yaw, share, fade, lifted);
        if (summon->elapsed < 0xFFFFu) {
            summon->elapsed++;
        }
    }

    D_800F2C40[0] = borrowed;
    if (!count) {
        D_800FE240 = work_base;
    }
    SetGeomOffset(0, 0);
    inside = 0;
}

/* The card under a summoned monster. Duel_DrawFieldCards draws each field
 * card through func_80015EF4, which takes its colour from the card's display
 * object: for FLASH_FRAMES frames after the card has landed that colour rises
 * to full, then the card is not drawn at all while the monster stands on it.
 * This runs in the game's frame, before draw_frame (GsDrawOt), so it reads
 * what the last frame left in summons[]. A card drawn the other way (flag
 * 0x400, func_80015DFC: the field seen from overhead, where no monster is
 * drawn) stays as it is. */
static void *original_card;

static int summon_zone(const void *record)
{
    u32 at = (u32)(uintptr_t)record;
    int side, zone;
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        for (zone = 0; zone < MONSTER_ZONES; zone++) {
            if (at == (u32)(uintptr_t)&D_801A7AD8[SIDE_ZONE(side, zone)]) {
                return side * MONSTER_ZONES + zone;
            }
        }
    }
    return -1;
}

static void draw_card(void *record, POLY_GT4 *prim, POLY_FT4 *sprite, s32 *color)
{
    void (*original)(void *, POLY_GT4 *, POLY_FT4 *, s32 *) =
        (void (*)(void *, POLY_GT4 *, POLY_FT4 *, s32 *))original_card;
    DuelCardRecord *card = record;
    int zone = summon_zone(record);
    const Summon *summon = zone >= 0 ? &summons[zone] : NULL;

    if (summon && summon->landed && summon->card && summon->card == card->card_id &&
        (card->flags & (DUEL_CARD_FLAG_OCCUPIED | DUEL_CARD_FLAG_FACE_DOWN)) == DUEL_CARD_FLAG_OCCUPIED &&
        summon_on() && duel_field_up()) {
        DisplayObject *object = card->object;
        if (summon->elapsed >= FLASH_FRAMES) {
            return;
        }
        if (object) {
            u32 kept = object->field_0C;
            u32 level = 0x80u + 0x7Fu * (summon->elapsed + 1) / FLASH_FRAMES;
            object->field_0C = (kept & ~0xFFFFFFu) | level * 0x010101u;
            original(record, prim, sprite, color);
            object->field_0C = kept;
            return;
        }
    }
    original(record, prim, sprite, color);
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 2) {
        return 0;   /* map_fixed and now_us arrived in mod API 2 */
    }
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = draw_frame;
    mod->reset = reset;
    mod->applied = applied;
    FieldArt_Init(from);
    /* Hooks arrived in mod API 4; without one the summon still grows the
     * monster, over its card. */
    if (from->api >= 4 && !original_card &&
        !host->hook(host, (void *)func_80015EF4, (void *)draw_card, &original_card)) {
        say("the summon cannot hide cards: func_80015EF4 could not be hooked\n");
    }
    return 1;
}
