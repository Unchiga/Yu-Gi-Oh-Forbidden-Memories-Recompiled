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
#include "game/view_state_orbit.h"
#include "game/graphics_frame.h"
#include "game/graphics_frame_buffer.h"
#define MODEL_SLOT_SETUP_EXPLICIT_TRANSFER_ARGS
#include "game/model_slot_setup.h"
#include "game/model_slot_updates.h"
#include "game/model_load_step.h"
#include "game/func_800540B4.h"
#include "game/func_800556E8.h"
#include "game/model_control_slot_animation.h"
#include "game/model_slot_state_updates.h"
#include "game/duel_scene_battle.h"
#include "game/duel_battle_stats.h"
#include "game/func_8005922C.h"
#include "game/func_80058DD8.h"
#include "game/file_transfer.h"
#include "game/file_transfer_steps.h"
#include "game/duel_display.h"
#include "game/duel_scene_state.h"
#include "game/duel_side_state.h"
#include "game/high_memory_addresses.h"
#include "game/display_object.h"
#include "game/func_80015EF4.h"
#include "game/display_object_layout.h"
#include "game/display_object_work_slots.h"
#include "game/duel_effect_resource_record.h"
#include "game/duel_effect_request.h"
#include "game/duel_apply_card_object_flags.h"
#include "game/duel_card_record_lifecycle.h"
#include "game/display_object_motion.h"
#include "game/model_control.h"
#include "game/model_transfer_flags.h"
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
#define META_SECTOR 275     /* the record's last sector: sounds, then the */
#define META_COMMANDS 0x110 /* command words, the stance variants' and the primary's */
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
    int tag;       /* 0 on the field; side + 1 on a big card the attack plays on */
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
    s32 commands[3]; /* its control modules' command words (effect_begin) */
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
static void attack_finish(void);
static void module_dropped(void);
/* The field fight has the duel camera, which was as in fight_view
 * (draw_fighters). */
static int fight_moved;
static ViewState fight_view;

static void reset(void)
{
    int i;
    for (i = 0; i < CACHE; i++) {
        cache[i].card = 0;
    }
    dimmed[0] = dimmed[1] = NULL; /* a state was loaded over them */
    memset(summons, 0, sizeof(summons));
    fight_moved = 0; /* the state brought its own camera */
    module_dropped();  /* and its own RAM where a module ran */
    attack_finish();
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
    /* The quiet load leaves the slot's command words at -1, so no module
     * runs; the record's own are kept for the attack's effects. */
    memcpy(monster->commands, record + META_SECTOR * SECTOR + META_COMMANDS, sizeof(monster->commands));
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

static Monster *acquire(int card, int position, int tag)
{
    Monster *monster = NULL;
    int i;
    for (i = 0; i < CACHE; i++) {
        if (cache[i].card == card && cache[i].position == position && cache[i].tag == tag) {
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
    monster->tag = tag;
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

/* `height` of its scale upright (MODEL_FIXED_ONE is all of it). */
static void place_flat(ModelSlot *slot, int x, int y, int z, int yaw, int scale, int height)
{
    VECTOR size;
    forget_coordinates(slot);
    func_8005A4C4(slot, x, y, z, yaw);
    size.vx = size.vz = scale;
    size.vy = scale * height / MODEL_FIXED_ONE;
    size.pad = 0;
    func_8005922C(slot->field_D18, &size);
}

static void place(ModelSlot *slot, int x, int y, int z, int yaw, int scale)
{
    place_flat(slot, x, y, z, yaw, scale, MODEL_FIXED_ONE);
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

/* The attacker whose control module plays its effects (effect_run, with the
 * fight below), run right after its model is sorted, while the scratch table
 * is live, so the effects' packets go with the model's and draw from its
 * texture bank. */
static Monster *effect_monster;
static void effect_run(void);

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
    if (monster == effect_monster) {
        effect_run();
    }
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
 * grows it from its card; whole, opaque and at lift() it is drawn as ever.
 * Sorted at its own depth, or at entry `at` when that is not negative (over a
 * flat card on the overhead field). */
static void draw_monster(Monster *monster, int x, int z, int yaw, int share, int fade, int lifted, int at,
                         int height)
{
    ModelSlot *slot = &D_800F2C40[0];
    int turned = yaw == MODEL_ANGLE_HALF_TURN;
    int body_x = monster->body_x * share / MODEL_FIXED_ONE;
    int body_y = monster->body_y * share / MODEL_FIXED_ONE * height / MODEL_FIXED_ONE;
    int body_z = monster->body_z * share / MODEL_FIXED_ONE;

    *slot = monster->slot;
    /* The body offset was measured facing up the field, so turning the
     * monster turns it too. */
    place_flat(slot, turned ? x + body_x : x - body_x, -body_y - lifted,
               turned ? z + body_z : z - body_z, yaw, monster->scale * share / MODEL_FIXED_ONE, height);
    monster->fade = fade;
    sort_monster(monster, (GsOT *)D_800E9D98[0], at);
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

/* With `board` the monsters stay on past FIELD_PITCH, through the tilt up to
 * the field seen from overhead, where a zone or an attack is chosen and the
 * cards are drawn flat on the screen (record flag 0x400, func_80015DFC), and
 * there too: through the same camera, so each is seen from above facing the
 * way it faces on the field, and it stands on its flat card, which the duel
 * projects to that very spot. That camera comes much nearer, so from
 * FIELD_PITCH each monster shrinks with the pitch, to `board_size` percent
 * looking straight down (TILT_PITCH), which keeps it over its own card. */
#define TILT_PITCH 1024
#define BOARD_SIZE 50
/* Seen from above, the parts of a tall monster nearest the camera fall
 * outwards from the middle of the screen, off its card at the field's edges;
 * its height is flattened with the pitch, to BOARD_HEIGHT looking straight
 * down, where height does not show. */
#define BOARD_HEIGHT (MODEL_FIXED_ONE / 2)
#define DUEL_CARD_FLAG_SPRITE 0x400
/* When the overhead view slides from one side's cards to the other's, a card
 * goes under the panel across the foot of the screen (its top at
 * BOARD_PANEL_TOP), and its monster, bigger than the card, would stick out
 * above it: one whose flat card is that far down (its corner, kept in the
 * record at 0x08 and 0x0A, plus BOARD_CARD_FEET) is left out. */
#define BOARD_CARD_FEET 0x26
#define BOARD_PANEL_TOP 0xAC
/* A flat card goes into the model table at its display object's priority
 * (func_80016784, GsSortFastSprite); its monster goes BOARD_DEPTH_STEPS
 * nearer, over it, as draw_battle puts one over a big card. */
#define BOARD_DEPTH_STEPS 2

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
    int at;     /* its ordering-table entry, or -1 for its own depth */
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
extern s16 D_8009B22A;         /* the trap that springs, 0 none */
extern MATRIX D_800FE128, D_800FE148; /* GsLIGHTWSMATRIX, GsWSMATRIX */
/* What the field fight needs to end the battle itself (fight_conclude). */
extern u16 D_8009B170[2];      /* the two cards' saved stat modifiers */
extern s16 gDuel_awSavedDefenseModifier[2];
extern u8 D_800E9ECE[];        /* the screen fade: bit 7 while one runs */
extern DisplayObject *G32 D_8009B214; /* the two panels slid off for the battle */
extern DisplayObject *G32 D_8009B21C;
void DisplayObject_ReleaseIfPresent(void *object);
void SD_SEPlayFull(u32 sound);

/* `attack` is a choice: off, on the attack cards, on the field, or both, one
 * bit each; the 1 stored when it was a bool on or off is the cards. */
#define ATTACK_CARDS 1
#define ATTACK_FIELD 2

/* The attack on the big cards (attack_begin and the rest, before
 * draw_battle): with `battle`. */
static int attack_on(void)
{
    return tunable("attack", 0) & ATTACK_CARDS;
}

/* The attack on the field (fight_begin and the rest, before draw_frame). */
static int fight_on(void)
{
    return (tunable("attack", 0) & ATTACK_FIELD) && !tunable("style", 0);
}

/* On the field alone: the fight ends the battle itself, with no attack
 * cards after it (fight_conclude). */
static int fight_alone(void)
{
    return (tunable("attack", 0) & (ATTACK_CARDS | ATTACK_FIELD)) == ATTACK_FIELD;
}

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
 * for it, in the stance its field card was in. While the attack is on it is
 * an entry of its own (tag side + 1), so the rows the attack plays never
 * reach the monster standing on the field, nor the other big card when both
 * show the same one. */
static Monster *battle_monster(int side)
{
    int id = (s16)D_800EA0E8[side].field_30, tag = attack_on() ? side + 1 : 0;
    /* For measuring, as `test` is on the field: any two monsters. */
    if (tunable("battle_test", 0)) {
        return acquire(tunable("battle_test", 0) + side * tunable("battle_test_step", 1), 0, tag);
    }
    if (id <= 0 || ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
        return NULL;
    }
    return acquire(Cards_ModelId(id), (D_8009B178[side] & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0, tag);
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

/* The attack (attack_on, with `battle`): when the battle reaches step 7, before
 * any damage number, the monsters on the big cards play the rows the arena
 * plays (func_8004EB00): the attacker its attack row, field_DFE + 3, and the
 * defender a reaction at that row's midpoint, which is when the arena's path
 * for a monster with no control module starts one (func_800559D4). Meanwhile
 * update_battle holds the battle at step 7 until the last blow lands; then the
 * game shows its flash, numbers and flames as ever, over the reactions. The
 * case is the exchange step 3 resolved: D_8009B1B0[side] is -1 for a card
 * destroyed and 1 for one hit that stays. A weaker attacker is answered with
 * the defender's own attack, as in the arena. The monsters played on are
 * entries of their own (tag side + 1), dropped when the presentation ends.
 * Only the models' own rows play: the arena's particles and sounds come from
 * each monster's control module and sound bank, which are never loaded here. */
#define BATTLE_STEP_PRESENT 2     /* the big cards are made (held here for the field) */
#define BATTLE_STEP_EXCHANGE 7    /* the damage numbers start from here */
#define ROW_ATTACK (-1)           /* field_DFE + 3 of the slot it is played on */
#define ROW_IDLE 1
#define ROW_WITHSTAND 5
#define ROW_HIT 6
#define ROW_GUARD 8
#define MODEL_ROW_STOPPED 0x23    /* field_E16 of a row that has stopped */
#define ATTACK_HOLD_LIMIT 1200    /* the most frames the battle is held */
/* Percent of the arena's own speed (8 units of a row a VBlank there and
 * here alike). The arena's blows take 1.3 to 4.5 seconds to land and a
 * counter-attack up to 10; twice as fast reads as one exchange. */
#define ATTACK_SPEED 200
#define ATTACK_DRIFT 35           /* percent of the body's shift kept */

enum { CASE_DESTROYED = 1, CASE_BLOCKED, CASE_COUNTER, CASE_TIE, CASE_DIRECT };
enum { ATTACK_IDLE, ATTACK_FIRST, ATTACK_COUNTER, ATTACK_OVER };
/* Where it is played: the field at step 2, then the big cards at step 7. */
enum { STAGE_CARDS = 1, STAGE_FIELD };

typedef struct {
    int stage;
    int phase, outcome, frames;
    int holding;                    /* update_battle waits */
    int held;                       /* the calls it has waited through */
    int landed;                     /* the blow being struck has landed */
    int struck;                     /* the last blow has landed */
    int still[DUEL_SIDE_COUNT];     /* destroyed: held on its hit row's last frame */
    int measured[DUEL_SIDE_COUNT];  /* rest and speed taken */
    int rest[DUEL_SIDE_COUNT][3];   /* the body's mean at rest, 1:1 */
    int speed[DUEL_SIDE_COUNT];     /* the slot's own field_E0D */
    int row[DUEL_SIDE_COUNT];       /* the row it plays, 0 none */
    int want[DUEL_SIDE_COUNT];      /* the row to start next, 0 none */
    int last[DUEL_SIDE_COUNT];      /* its field_E06 last frame, -1 before the row has begun */
    /* On the field: */
    int record[DUEL_SIDE_COUNT];    /* the card's record, -1 none */
    int home[DUEL_SIDE_COUNT][2];   /* its zone's x and z */
    int yaw[DUEL_SIDE_COUNT];       /* turned to face the other */
    int strike[2];                  /* where the attacker strikes from */
    int back;                       /* frames into the attacker's way home */
    int closing;                    /* frames into the camera's way back */
    int at[2];                      /* where the attacker was drawn */
    int gap;                        /* field units from body to body when it strikes */
    int turn;                       /* the camera's swing, all the way in */
    /* On the field alone (fight_alone), with no attack cards after it: */
    int wins[DUEL_SIDE_COUNT];      /* D_8009B1B0 as step 3 will store it */
    int damage[DUEL_SIDE_COUNT];    /* D_8009B1A4 as step 3 will store it */
    int due[DUEL_SIDE_COUNT];       /* its blow has landed: its number shows */
    int shown[DUEL_SIDE_COUNT];     /* its number has been asked for */
    DuelEffectRequest *number[DUEL_SIDE_COUNT]; /* the number's effect, while it runs */
    int gone[DUEL_SIDE_COUNT];      /* frames a destroyed one has been going */
    int head[DUEL_SIDE_COUNT][2];   /* the top middle of its outline on the screen, last drawn */
    int headed[DUEL_SIDE_COUNT];    /* head has been measured */
    int conclude;                   /* update_battle ends the battle at its next call */
    /* The attacker's module (effect_begin, with `effects`): */
    int effect_started;             /* it has been set going */
    int effect_hit;                 /* it has said the blow lands */
    int effect_done;                /* it has said it is over */
    int effect_frames;              /* frames it has been called since it started */
} Attack;
static Attack attack;

static int attack_outcome(void)
{
    int forced = tunable("attack_test", 0);
    if (!D_800E9EF0[1]) {
        return CASE_DIRECT;
    }
    if (forced >= CASE_DESTROYED && forced <= CASE_TIE) {
        return forced;
    }
    if (D_8009B1B0[0] == -1) {
        return D_8009B1B0[1] == -1 ? CASE_TIE : CASE_COUNTER;
    }
    return D_8009B1B0[1] == -1 ? CASE_DESTROYED : CASE_BLOCKED;
}

/* The mean world translation of the slot's parts, placed at the origin at 1:1
 * turned by `yaw`: where its body is in the animation's current frame. */
static void body_mean(ModelSlot *slot, int yaw, int mean[3])
{
    s32 sum[3] = {0, 0, 0};
    int parts = 0, i, axis;
    place(slot, 0, 0, 0, yaw, MODEL_FIXED_ONE);
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        MATRIX world;
        if (!unit) {
            continue;
        }
        GsGetLwUnit(unit, &world);
        for (axis = 0; axis < 3; axis++) {
            sum[axis] += world.t[axis];
        }
        parts++;
    }
    for (axis = 0; axis < 3; axis++) {
        mean[axis] = parts ? sum[axis] / parts : 0;
    }
}

/* The last blow has landed, and no other is coming (a monster that was hit
 * may never finish the row it was striking with): the hold on the big cards
 * lets go at once, the one on the field once the attacker is home
 * (draw_fighters). */
static void attack_struck(void)
{
    attack.struck = 1;
    attack.phase = ATTACK_OVER;
    if (attack.stage == STAGE_CARDS) {
        attack.holding = 0;
    } else {
        attack.due[0] = attack.due[1] = 1; /* every number still to show */
    }
}

/* Start `row` on side `side`'s slot, which is in D_800F2C40[0]. */
static void attack_play(int side, ModelSlot *slot, int row)
{
    int speed = tunable("attack_speed", ATTACK_SPEED), step;
    speed = speed < 25 ? 25 : speed > 400 ? 400 : speed;
    if (row == ROW_ATTACK) {
        row = slot->field_DFE + 3;
    }
    if (row == ROW_IDLE) {
        Model_ControlSlotAnimation(0, 0, 0);
        func_800597C8(0, ROW_IDLE, 0);
        slot->field_E0D = (u8)attack.speed[side];
        attack.row[side] = 0;
        say("attack: side %d back to rest\n", side);
        return;
    }
    if (!slot->field_750[row].max) {
        attack.row[side] = 0;
        if (attack.phase == ATTACK_COUNTER && side == 1) {
            attack_struck(); /* no attack row to answer with */
            attack.phase = ATTACK_OVER;
        }
        return;
    }
    step = attack.speed[side] * speed / 100;
    slot->field_E0D = (u8)(step > 0 ? step : 1);
    Model_ControlSlotAnimation(0, row, 1);
    attack.row[side] = row;
    attack.last[side] = -1;
    say("attack: side %d plays row %d (%d frames)\n", side, row, slot->field_750[row].max);
}

/* How far the row a side plays has got: 0 not yet half way, 1 past its
 * midpoint, 2 over (it ran to its end, wrapped round, or the slot left it). */
static int attack_progress(int side, const ModelSlot *slot)
{
    int row = attack.row[side], length, at = slot->field_E06, progress;
    if (!row) {
        return 0;
    }
    if (slot->field_BF5 != row) {
        return attack.last[side] >= 0 ? 2 : 0;
    }
    length = slot->field_750[row].max << 4;
    /* The hit row stops itself a step short of its end (func_800556E8). */
    progress = at >= length || slot->field_E16 == MODEL_ROW_STOPPED ||
                       (attack.last[side] >= 0 && at < attack.last[side])
                   ? 2
               : at >= length / 2 ? 1
                                  : 0;
    attack.last[side] = at;
    return progress;
}

/* The blow struck by side `side` lands. */
static void attack_landed(int side)
{
    if (attack.stage == STAGE_FIELD) {
        attack.due[!side] = 1; /* the struck one's number */
    }
    switch (attack.outcome) {
    case CASE_DESTROYED:
        attack.want[1] = ROW_HIT;
        attack_struck();
        break;
    case CASE_BLOCKED:
        attack.want[1] = ROW_GUARD;
        attack_struck();
        break;
    case CASE_TIE:
        attack.want[0] = attack.want[1] = ROW_HIT;
        attack_struck();
        break;
    case CASE_COUNTER:
        if (side == 0) {
            attack.want[1] = ROW_WITHSTAND;
        } else {
            attack.want[0] = ROW_HIT;
            attack_struck();
        }
        break;
    default: /* direct */
        attack_struck();
        break;
    }
    say("attack: side %d's blow lands (case %d) after %d frames\n", side, attack.outcome, attack.frames);
}

/* The defender strikes back: as soon as it has withstood the blow, where the
 * arena waits for the attacker's whole row to end first. */
static void attack_counter(void)
{
    attack.phase = ATTACK_COUNTER;
    attack.landed = 0;
    attack.want[1] = ROW_ATTACK;
}

/* Once the big cards are up at step 7 with the attacker drawn. */
static void attack_begin(Monster *const monsters[DUEL_SIDE_COUNT])
{
    const ModelSlot *slot;
    if (attack.phase != ATTACK_IDLE || !attack_on() || (D_8009B174 & 0xF) != BATTLE_STEP_EXCHANGE ||
        !monsters[0] || (D_800E9EF0[1] && !monsters[1])) {
        return;
    }
    slot = &monsters[0]->slot;
    if (!slot->field_750[slot->field_DFE + 3].max) {
        return; /* no attack row: the battle goes on as ever */
    }
    memset(&attack, 0, sizeof(attack));
    attack.stage = STAGE_CARDS;
    attack.phase = ATTACK_FIRST;
    attack.outcome = attack_outcome();
    attack.holding = 1;
    attack.want[0] = ROW_ATTACK;
    say("attack: case %d\n", attack.outcome);
}

/* One side's turn in the pass, its slot in D_800F2C40[0]. */
static void attack_side(int side, ModelSlot *slot, int yaw)
{
    int progress, striking;
    if (attack.phase == ATTACK_IDLE) {
        return;
    }
    if (!attack.measured[side]) {
        body_mean(slot, yaw, attack.rest[side]);
        attack.speed[side] = slot->field_E0D;
        attack.measured[side] = 1;
    }
    if (attack.want[side]) {
        attack_play(side, slot, attack.want[side]);
        attack.want[side] = 0;
    }
    progress = attack_progress(side, slot);
    striking = ((attack.phase == ATTACK_FIRST && side == 0) || (attack.phase == ATTACK_COUNTER && side == 1)) &&
               attack.row[side] == slot->field_DFE + 3;
    if (striking) {
        /* With the attacker's module playing, its first blow lands when the
         * module says so, or at the end of the row if it never does. */
        int due = progress >= 1;
        if (side == 0 && attack.phase == ATTACK_FIRST && attack.stage == STAGE_FIELD && effect_monster) {
            due = attack.effect_hit || progress == 2;
        }
        if (due && !attack.landed) {
            attack.landed = 1;
            attack_landed(side);
        }
        if (progress == 2) {
            if (attack.phase == ATTACK_FIRST && attack.outcome == CASE_COUNTER) {
                attack_counter(); /* it had no row to withstand the blow with */
            } else {
                attack.phase = ATTACK_OVER;
            }
            if (!attack.want[side]) {
                attack.want[side] = ROW_IDLE;
            }
        }
    } else if (progress == 2 && attack.row[side] == ROW_HIT) {
        /* A monster that was hit is destroyed: it holds its last frame until
         * its card burns and it goes. */
        func_800597C8(0, ROW_HIT, slot->field_750[ROW_HIT].max - 1);
        slot->field_E0D = 0;
        attack.still[side] = 1;
    } else if (progress == 2 && side == 1 && attack.phase == ATTACK_FIRST && attack.outcome == CASE_COUNTER) {
        attack_counter();
    } else if (progress == 2 && !attack.want[side]) {
        attack.want[side] = ROW_IDLE;
    }
}

/* What to add to a side's placement so that only `attack_drift` percent of
 * its body's shift from the animation shows, and no more than half a card's
 * width of it: the arena's rows carry a monster across a wide floor, and a
 * small monster stands on its card several times enlarged, so a reaction
 * threw it off the screen. The attacker still lunges towards the other card
 * and the one hit still recoils, both near their own. */
static void attack_drift(int side, ModelSlot *slot, int yaw, int scale, int keep, int limit, int drift[3])
{
    int now[3], axis;
    drift[0] = drift[1] = drift[2] = 0;
    if (attack.phase == ATTACK_IDLE || !attack.measured[side]) {
        return;
    }
    keep = keep < 0 ? 0 : keep > 100 ? 100 : keep;
    body_mean(slot, yaw, now);
    for (axis = 0; axis < 3; axis++) {
        int full = (now[axis] - attack.rest[side][axis]) * scale / MODEL_FIXED_ONE;
        int kept = full * keep / 100;
        kept = kept < -limit ? -limit : kept > limit ? limit : kept;
        drift[axis] = kept - full;
    }
}

/* The duel camera back where the field fight found it. */
static void fight_restore(void)
{
    if (fight_moved) {
        D_800F2848.field_00 = fight_view.field_00;
        D_800F2848.angle = fight_view.angle;
        D_800F2848.view.vrx = fight_view.view.vrx;
        D_800F2848.view.vry = fight_view.view.vry;
        D_800F2848.view.vrz = fight_view.view.vrz;
        ViewState_ApplyOrbit();
        fight_moved = 0;
    }
}

/* When the presentation is over, or the big cards take over from the field:
 * the fighters' entries go, and the next fight starts afresh. */
static void effect_end(void);

static void attack_finish(void)
{
    int i;
    effect_end();
    fight_restore();
    for (i = 0; i < CACHE; i++) {
        if (cache[i].tag) {
            cache[i].card = 0;
        }
    }
    memset(&attack, 0, sizeof(attack));
}

/* DuelScene_UpdateBattle, held while a fight is on: at step 7 while a blow is
 * on its way to a big card, or at step 2, before the big cards are made, while
 * the two fight on the field. Each held call counts towards the limit, so the
 * battle goes on whatever the drawing does. */
static void *original_battle;

static int battle_held(void)
{
    if (!attack.holding || attack.held >= ATTACK_HOLD_LIMIT || tunable("style", 0)) {
        return 0;
    }
    if (attack.stage == STAGE_FIELD) {
        return D_8009B174 == BATTLE_STEP_PRESENT && fight_on();
    }
    return (D_8009B174 & 0xF) == BATTLE_STEP_EXCHANGE && attack_on() && tunable("battle", 0);
}

/* On the field alone, at the call step 2 would make the big cards in: the
 * battle ends here. Step 3 runs as the game's own, its fade-out marked begun
 * so the field stays up: it takes the life points and counts the ranks. Then
 * what step 11 does first: each card not destroyed goes back on its zone, as
 * it was, the attacker used for the turn; the two lifted cards are let go;
 * the two panels slide back in. A destroyed card's record stays empty, as it
 * does once its card has burnt. A card back on its zone stands its monster
 * at once (the summon is marked done), where the fighter stood. */
#define BATTLE_STEP_RESOLVE 3     /* the exchange is worked out and the life points taken */
#define BATTLE_STEP_STARTED 0x80  /* a step's first call has been */
#define DUEL_SCENE_AFTER_BATTLE 5 /* where step 11 leaves the scene */
#define PANEL_LEFT 0xC            /* where step 11 slides the panels back to */
#define PANEL_RIGHT 0x118

static void fight_panel(DisplayObject *panel, int x)
{
    panel->position.h.field_28 = (s16)x;
    panel->field_2C.h.field_2C = 0x10;
    panel->field_6C = 1;
    panel->update = (DisplayObjectCallback)func_8001ED20;
    panel->position.h.field_2A = panel->field_30.h.field_32;
}

static void fight_conclude(void)
{
    int side;

    attack.conclude = 0;
    D_8009B174 = BATTLE_STEP_RESOLVE | BATTLE_STEP_STARTED;
    ((void (*)(void))original_battle)();
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        DisplayObject *card = D_800E9EF0[side];
        DuelCardRecord *record;
        int index;
        if (!card || D_8009B1B0[side] < 0) {
            continue;
        }
        index = card->field_6A;
        func_80024D34(index, card->field_6B);
        record = &D_801A7AD8[index];
        record->flags |= (D_8009B178[side] & (DUEL_CARD_FLAG_DEFENSE_POSITION | DUEL_CARD_FLAG_USE_GUARDIAN_STAR_2)) |
                         (side ? 0 : DUEL_CARD_FLAG_USED_THIS_TURN);
        record->stat_modifier = (s16)D_8009B170[side];
        record->defense_modifier = gDuel_awSavedDefenseModifier[side];
        Duel_ApplyCardObjectFlags((DuelCardDisplayObject *)record->object);
        if ((index >= SIDE_ZONE(0, 0) && index < SIDE_ZONE(0, MONSTER_ZONES)) ||
            (index >= SIDE_ZONE(1, 0) && index < SIDE_ZONE(1, MONSTER_ZONES))) {
            Summon *summon = &summons[(index >= SIDE_ZONE(1, 0)) * MONSTER_ZONES +
                                      index - SIDE_ZONE(index >= SIDE_ZONE(1, 0), 0)];
            summon->card = record->card_id;
            summon->landed = 1;
            summon->y = record->object ? (s16)((DisplayObject *)record->object)->field_30.h.field_32 : 0;
            summon->elapsed = 0xFFFFu;
        }
    }
    DisplayObject_ReleaseIfPresent(D_800E9EF0[0]);
    DisplayObject_ReleaseIfPresent(D_800E9EF0[1]);
    fight_panel(D_8009B214, PANEL_LEFT);
    fight_panel(D_8009B21C, PANEL_RIGHT);
    gDuel_wSceneStateFlags = DUEL_SCENE_AFTER_BATTLE;
    say("fight: the battle ends on the field (exchange %d, %d; life %d, %d)\n", D_8009B1B0[0], D_8009B1B0[1],
        D_800E9FF0[0].life_points.unsigned_value, D_800E9FF0[1].life_points.unsigned_value);
}

static void update_battle(void)
{
    if (battle_held()) {
        attack.held++;
        return;
    }
    if (attack.stage == STAGE_FIELD && attack.holding) {
        fight_restore(); /* let go early: the limit, or the setting changed */
        attack.holding = 0;
    }
    if (attack.stage == STAGE_FIELD && attack.conclude && original_battle && fight_alone() &&
        D_8009B174 == BATTLE_STEP_PRESENT &&
        (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) == DUEL_SCENE_BATTLE) {
        /* Step 3 waits for any screen fade to end; so does this, or it would
         * take no life points. */
        if (!(D_800E9ECE[0] & 0x80)) {
            fight_conclude();
            return;
        }
        if (++attack.held < ATTACK_HOLD_LIMIT) {
            return;
        }
        attack.conclude = 0; /* the big cards after all */
    }
    if (original_battle) {
        ((void (*)(void))original_battle)();
    }
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
        attack_finish();
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
        if (attack.stage != STAGE_FIELD) {
            attack_finish();
        }
        return 0;
    }
    /* The big cards take over from the fight on the field. */
    if (attack.stage == STAGE_FIELD) {
        attack_finish();
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
    attack_begin(monsters);
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        Monster *monster = monsters[side];
        ModelSlot *slot = &D_800F2C40[0];
        /* The attacker is on the left and turns right; the defender turns left. */
        int yaw = side == 0 ? MODEL_ANGLE_FULL_TURN - BATTLE_TURN : BATTLE_TURN, wx, wy, at, drift[3];
        GsOT *table = D_800E9D90[cards[side]->ot_index];
        if (!monster) {
            continue;
        }
        battle_pose(monster, yaw);
        screen_to_world(cards[side]->field_30.h.field_30 + BATTLE_CARD_WIDTH / 2 - monster->battle_ox +
                            (side == 0 ? -BATTLE_BACK : BATTLE_BACK),
                        cards[side]->field_30.h.field_32 + BATTLE_CARD_FEET - monster->battle_oy, &wx, &wy);
        *slot = monster->slot;
        attack_side(side, slot, yaw);
        attack_drift(side, slot, yaw, monster->battle_scale, tunable("attack_drift", ATTACK_DRIFT),
                     BATTLE_CARD_WIDTH / 2 * BATTLE_DISTANCE / BATTLE_PROJECTION, drift);
        place(slot, wx - monster->battle_x + drift[0], wy - monster->battle_y + drift[1],
              -monster->battle_z + drift[2], yaw, monster->battle_scale);
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
    if (attack.phase != ATTACK_IDLE) {
        attack.frames++;
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
        attack_finish();
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
    return tunable("summon", 0) && !tunable("style", 0);
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

/* The attack on the field (`attack` 2 or 3, fight_on). The quick battle lifts
 * both field cards in its first call (their records lose
 * DUEL_CARD_FLAG_OCCUPIED, and each card's display object becomes
 * D_800E9EF0[side], field_6A its record) and flies them up to the screen while
 * its own camera comes back from overhead to the view the duel is played
 * from. Step 1 ends there, and the next call makes the big cards: the battle
 * is held at that call while the two fight on the field. The cards in the air
 * are hidden, as step 2 hides them itself a little later; both monsters grow
 * on their zones, face to face, a face-down one shown as it is about to be;
 * the attacker crosses to just short of the defender, strikes as on the big
 * cards (attack_side), and goes home again unless it was destroyed. Once both
 * have done the battle goes on, and the two stay where they are until the
 * field fades out behind the big cards. The outcome is not stored until step
 * 3, but it is func_8001EFD4's, which only reads (the CPU calls it for its
 * field scans). */
#define FIELD_TAG 3      /* the fighters' entries: side + FIELD_TAG */
#define FIGHT_APPEAR 12  /* frames the two take to grow on their zones */
#define FIGHT_CHARGE 20  /* frames the attacker takes to cross, and to go home */
#define FIGHT_DRIFT 35   /* field units of the body's shift at most: half a zone */
/* The arena stands both models on one point facing each other, each body
 * its own way from it (raw_z), and an attack row is made to reach across that
 * gap: the attacker strikes from as far from the defender as the two bodies
 * would be in the arena at its own scale (fight_reach percent of it), and
 * while it plays an attack row all of its body's shift shows, up to that
 * gap, so one that runs in reaches the other and one that stretches out does
 * not overshoot it. */
#define FIGHT_REACH 100
/* The duel's camera comes in on the two over FIGHT_CAMERA frames, to
 * FIGHT_ZOOM percent of its distance, aimed between the attacker, wherever it
 * is, and the defender, FIGHT_RISE field units over the mat, and swung
 * FIGHT_TURN percent of the way round to seeing the two side on; it goes back
 * the same way while the attacker goes home. Nothing of the game moves the
 * camera then: it only re-applies the orbit while one of its own camera
 * moves runs, and the last one ended with step 1. */
#define FIGHT_ZOOM 60
#define FIGHT_TURN 60
#define FIGHT_RISE 48
#define FIGHT_CAMERA 24
/* On the field alone the big cards never come up, so what they would show
 * shows on the field: the number step 8 puts on a card (step 9's for a direct
 * attack, there as the plain one rather than the burst), the game's own
 * effect with its sound, over the monster struck as the blow lands; and a
 * destroyed monster goes over FIGHT_VANISH frames, the summon run backwards,
 * with the sound of a card burning. */
#define FIGHT_VANISH 16
#define EFFECT_DAMAGE 2           /* the duel effect step 8 shows a number with */
#define DIRECT_X 0xA0             /* a direct attack's number: the screen's middle, */
#define DIRECT_Y 0x78             /* no lower than its middle */
#define NUMBER_HALF 16            /* each of the effect's characters is 32 pixels square */
#define NUMBER_ABOVE 8            /* pixels between a number and the head under it */
#define SCREEN_WIDTH 320
#define SOUND_DAMAGE 0x10         /* + the number's size; the attacker's 0xD + it */
#define SOUND_DAMAGE_ATTACKER 0xD
#define SOUND_DESTROY 0x1B

static int fight_outcome(void)
{
    int forced = tunable("attack_test", 0), result;
    if (!D_800E9EF0[1]) {
        return CASE_DIRECT;
    }
    if (forced >= CASE_DESTROYED && forced <= CASE_TIE) {
        return forced;
    }
    /* As step 3 reads it into D_8009B1B0. */
    result = func_8001EFD4(D_800E9EF0[0], D_800E9EF0[1]);
    if (result > 0) {
        return CASE_DESTROYED;
    }
    if (result == 0) {
        return CASE_BLOCKED;
    }
    if (result == -1) {
        return CASE_TIE;
    }
    return (D_8009B178[1] & DUEL_CARD_FLAG_DEFENSE_POSITION) ? CASE_BLOCKED : CASE_COUNTER;
}

/* What step 3 will store, D_8009B1B0 (-1 destroyed, 1 struck and stays, 0
 * untouched) and D_8009B1A4 (the number on its card), from the same
 * func_8001EFD4 result. */
static void fight_tally(void)
{
    int defence = D_800E9EF0[1] && (D_8009B178[1] & DUEL_CARD_FLAG_DEFENSE_POSITION);
    int result = func_8001EFD4(D_800E9EF0[0], D_800E9EF0[1]);

    attack.wins[0] = attack.wins[1] = 0;
    attack.damage[0] = attack.damage[1] = 0;
    if (result >= 0) {
        attack.wins[1] = 1;
        if (result) {
            attack.wins[1] = -1;
            attack.damage[1] = defence ? 0 : result;
        }
    } else {
        attack.wins[0] = attack.wins[1] = -1;
        if (result < -1) {
            attack.damage[0] = result;
            attack.wins[1] = 1;
            if (defence) {
                attack.wins[0] = 1;
            }
        }
    }
}

/* The yaw that faces a model along (dx, dz) on the mat: at 0 it faces the
 * player's edge, -z. The fight's angles use the game's own fixed-point
 * trigonometry (ratan2, rsin, rcos) rather than the C library's, whose last
 * bits differ between systems: a frame is the same on every one. */
static int yaw_towards(int dx, int dz)
{
    int yaw = (int)ratan2(-dx, -dz);
    return yaw < 0 ? yaw + MODEL_ANGLE_FULL_TURN : yaw;
}

/* The monster a fighter's card shows, in its stance, or NULL. */
static Monster *fighter(int side)
{
    int index = attack.record[side], id;
    const DuelCardRecord *card;
    if (index < 0) {
        return NULL;
    }
    if (tunable("battle_test", 0)) {
        return acquire(tunable("battle_test", 0) + side * tunable("battle_test_step", 1), 0, side + FIELD_TAG);
    }
    card = &D_801A7AD8[index];
    id = card->card_id;
    if (id <= 0 || ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
        return NULL;
    }
    return acquire(Cards_ModelId(id), (card->flags & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0, side + FIELD_TAG);
}

/* The camera's swing towards seeing a fight along (dx, dz) side on. It looks
 * along (cos angle, sin angle) on the mat (ViewState_ApplyOrbit), so side on
 * is the line's own angle a quarter turn either way: the nearer of the two. */
static int fight_turn(int dx, int dz)
{
    int line = (int)ratan2(dz, dx), turn, share;
    turn = (line + MODEL_ANGLE_QUARTER_TURN - D_800F2848.angle) & (MODEL_ANGLE_FULL_TURN - 1);
    /* Either side on is a half turn from the other: the nearer is within a
     * quarter turn of where the camera is. */
    turn = (turn + MODEL_ANGLE_QUARTER_TURN) % MODEL_ANGLE_HALF_TURN - MODEL_ANGLE_QUARTER_TURN;
    share = tunable("fight_turn", FIGHT_TURN);
    share = share < 0 ? 0 : share > 100 ? 100 : share;
    return turn * share / 100;
}

/* The attacker's own effects (`effects`, on the field fight). The arena's
 * beams, flashes and particles come from each monster's control module, a
 * retail MIPS overlay the port interprets (notes/pc-build.md, MIPS-only
 * effects). Its primary goes to 0x8013A000 and its stance's variant to
 * 0x8013B000, the addresses slot 0's copies are linked for; what was there
 * is kept and put back when the fight is over. Its contexts are in its
 * arena. It draws from the arena's effect sheet, which the duel's card
 * thumbnails cover in VRAM, so the sheet goes into the attacker's bank.
 *
 * It is called the arena's way (func_800559D4), but told the arena's world:
 * - time: on each frame its animation index moves it adds Model_GetFrameStep,
 *   the VBlanks a frame took: 2 in the 30 fps arena, 1 on the 60 fps field,
 *   where it ran at half speed and Blue-Eyes never fired. It is handed 2.
 * - space: it works at the arena's scale around slot 0's arena pose, aims at
 *   the other slot's body centre (field_DD0, through
 *   Model_CopySlotU16Values) and draws camera-facing billboards sized in
 *   view space. So for the call the attacker stands at that pose, both body
 *   centres are carried into it through the inverse of its field root matrix
 *   T (field_D18: rotation times its scale s, and its position), and the
 *   world-screen matrix (GsWSMATRIX, which GsGetLs reads too) is WS T scaled
 *   by 1/s: the field's screen positions at the arena's depth, so everything
 *   comes out s times its arena size, as the monster does.
 * - camera: its camera requests are refused (D_8009B07B and D_8009B07C).
 * Its answer (field_E0E) times the fight: 4, 3 or 1, the blow lands; 2, it
 * is over. The reactions it starts in slot 1 go with the copy put there for
 * the call: the fight plays the defender's own. */
#define MODULE_AREA 0x8013A000u
#define MODULE_SPAN 0x6000u
#define MODULE_PRIMARY_BYTES 0x1000u  /* to MODULE_AREA */
#define MODULE_VARIANT_BYTES 0x5000u  /* to MODULE_AREA + MODULE_PRIMARY_BYTES */
#define SU_STAGE_RECORD 0x88          /* stage 0's record in SU.MRG */
#define SHEET_PALETTE 83              /* its effect sheet: the palette sector, */
#define SHEET_SECTORS 33              /* then 32 of texels */
#define SHEET_X 0x380
#define SHEET_PALETTE_X 0x200
#define SHEET_PALETTE_Y 0xF4
#define ARENA_STEP 2
#define EFFECT_LIMIT 900              /* the most frames the fight waits for a module */
#define EFFECT_OVER 2                 /* field_E0E: the module is done, or not going */
#define EFFECT_SETUP 6                /* field_E0E before the attack row */

static u8 module_saved[MODULE_SPAN];
static int module_held;
static u8 effect_sheet[SHEET_SECTORS * SECTOR];
static int effect_sheet_state; /* 0 not read yet, 1 read, -1 not there */
static Monster *effect_defender;

extern u8 D_8009AFA3; /* the VBlanks the last frame took: Model_GetFrameStep */

/* A loaded state brought its own RAM: what was kept is not put back. */
static void module_dropped(void)
{
    module_held = 0;
}

static void effect_end(void)
{
    if (module_held) {
        memcpy((void *)(uintptr_t)MODULE_AREA, module_saved, MODULE_SPAN);
        module_held = 0;
    }
    effect_monster = effect_defender = NULL;
}

static int effect_sheet_ready(void)
{
    int su;
    if (effect_sheet_state) {
        return effect_sheet_state > 0;
    }
    su = host->disc_file_start(host, "\\DATA\\SU.MRG;1");
    effect_sheet_state = su >= 0 && host->disc_read(host, su + SU_STAGE_RECORD + SHEET_PALETTE, SHEET_SECTORS,
                                                    effect_sheet) == SHEET_SECTORS ? 1 : -1;
    say("effect sheet %s\n", effect_sheet_state > 0 ? "read" : "missing");
    return effect_sheet_state > 0;
}

/* At fight_begin: the attacker's modules into place, its commands on, its
 * contexts cleared and the effect sheet in its bank. */
static void effect_begin(Monster *attacker, Monster *defender)
{
    ModelControlCommandView *view = (ModelControlCommandView *)&attacker->slot;
    u16 *bank = SoftGpu_Bank(attacker->bank);
    int i;

    if (!tunable("effects", 1) || attacker->commands[attacker->position] < 0 || !bank || !effect_sheet_ready()) {
        return;
    }
    if (!module_held) {
        memcpy(module_saved, (const void *)(uintptr_t)MODULE_AREA, MODULE_SPAN);
        module_held = 1;
    }
    memcpy((void *)(uintptr_t)MODULE_AREA, attacker->arena + ARENA_PRIMARY, MODULE_PRIMARY_BYTES);
    memcpy((void *)(uintptr_t)(MODULE_AREA + MODULE_PRIMARY_BYTES), attacker->arena + ARENA_VARIANT,
           MODULE_VARIANT_BYTES);
    memset(attacker->arena + ARENA_DATA_A, 0, ARENA_SIZE - ARENA_DATA_A);
    for (i = 0; i < 3; i++) {
        view->commands[i] = attacker->commands[i];
    }
    bank_put(bank, SHEET_PALETTE_X, SHEET_PALETTE_Y, 0x100, 2, (const u16 *)effect_sheet);
    for (i = 0; i < SHEET_SECTORS - 1; i++) {
        bank_put(bank, SHEET_X + (i / 16) * 0x40, (i % 16) * 0x10, 0x40, 0x10,
                 (const u16 *)(effect_sheet + (i + 1) * SECTOR));
    }
    effect_monster = attacker;
    effect_defender = defender;
    say("effect: card %d, commands %d %d %d\n", attacker->card, attacker->commands[0], attacker->commands[1],
        attacker->commands[2]);
}

/* A field point into the attacker's arena space: T^-1, with T rotation times
 * uniform scale, then its position. */
static void effect_to_arena(const MATRIX *t, s16 *p)
{
    double d[3], s2 = 0, out;
    int i, k;
    for (i = 0; i < 3; i++) {
        d[i] = (double)p[i] - t->t[i];
        s2 += (double)t->m[i][0] * t->m[i][0];
    }
    s2 /= 4096.0 * 4096.0;
    for (i = 0; i < 3; i++) {
        out = 0;
        for (k = 0; k < 3; k++) {
            out += t->m[k][i] / 4096.0 * d[k];
        }
        p[i] = (s16)(out / (s2 > 0 ? s2 : 1));
    }
}

/* The world-screen matrix the module is handed: WS T, scaled by 1/s. */
static void effect_view(const MATRIX *ws, const MATRIX *t, MATRIX *m)
{
    double s2 = 0, k;
    int i, j, n;
    for (i = 0; i < 3; i++) {
        s2 += (double)t->m[i][0] * t->m[i][0];
    }
    k = s2 > 0 ? 4096.0 / sqrt(s2) : 1;
    for (i = 0; i < 3; i++) {
        double tt = 0;
        for (j = 0; j < 3; j++) {
            double v = 0;
            for (n = 0; n < 3; n++) {
                v += (double)ws->m[i][n] * t->m[n][j];
            }
            m->m[i][j] = (s16)(v / 4096.0 * k);
            tt += (double)ws->m[i][j] * t->t[j];
        }
        m->t[i] = (int)((tt / 4096.0 + ws->t[i]) * k);
    }
}

static void effect_run(void)
{
    static ModelSlot kept; /* slot 1, put back after the call */
    ModelSlot *slot = &D_800F2C40[0], *other = &D_800F2C40[1];
    GsCOORDUNIT *root = (GsCOORDUNIT *)(uintptr_t)slot->field_D18;
    MATRIX field_root, ws = D_800FE148;
    SVECTOR field_rot;
    s16 own[4];
    u8 camera = D_8009B07B, camera_too = D_8009B07C, step = D_8009AFA3;
    int i, state;

    if (!root || attack.stage != STAGE_FIELD || attack.phase == ATTACK_IDLE) {
        return;
    }
    kept = *other;
    *other = effect_defender ? effect_defender->slot : *slot;
    if (!effect_defender) {
        other->field_DD0[0] = (s16)attack.home[1][0]; /* the zone across, at its height */
        other->field_DD0[2] = (s16)attack.home[1][1];
    }
    field_root = root->matrix;
    field_rot = root->rot;
    for (i = 0; i < 4; i++) {
        own[i] = slot->field_DD0[i];
    }
    effect_view(&ws, &field_root, &D_800FE148);
    effect_to_arena(&field_root, slot->field_DD0);
    effect_to_arena(&field_root, other->field_DD0);
    forget_coordinates(slot);
    func_8005A4C4(slot, 0, 0, 0, 0);
    D_8009B07B = D_8009B07C = 1;
    D_8009AFA3 = ARENA_STEP;

    func_800559D4(0);

    D_8009AFA3 = step;
    D_8009B07B = camera;
    D_8009B07C = camera_too;
    D_800FE148 = ws;
    root->rot = field_rot;
    root->matrix = field_root;
    forget_coordinates(slot);
    for (i = 0; i < 4; i++) {
        slot->field_DD0[i] = own[i];
    }
    *other = kept;

    state = slot->field_E0E;
    if (state != EFFECT_OVER && state != EFFECT_SETUP && !attack.effect_started) {
        attack.effect_started = 1;
    }
    if (!attack.effect_started) {
        return;
    }
    attack.effect_frames++;
    if (!attack.effect_hit && (state == 4 || state == 3 || state == 1)) {
        attack.effect_hit = 1;
        say("effect: card %d lands after %d frames\n", effect_monster->card, attack.effect_frames);
    }
    if (!attack.effect_done && state == EFFECT_OVER) {
        attack.effect_done = 1;
        say("effect: card %d over after %d frames\n", effect_monster->card, attack.effect_frames);
    }
}

/* The fight waits for the attacker's module: until it is over, it has run
 * EFFECT_LIMIT frames, or the attack row ended without it ever starting. */
static int effect_busy(void)
{
    return effect_monster && attack.stage == STAGE_FIELD && !attack.effect_done &&
           attack.effect_frames < EFFECT_LIMIT && (attack.effect_started || attack.phase != ATTACK_OVER);
}

/* At the call step 2 makes the big cards in, with the field up. */
static void fight_begin(void)
{
    const ModelSlot *slot;
    Monster *attacker, *defender;
    int side, dx, dz, length, reach;

    if (attack.phase != ATTACK_IDLE || !fight_on() || D_8009B229 || D_8009B22A || !D_800E9EF0[0] ||
        (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) != DUEL_SCENE_BATTLE ||
        D_8009B174 != BATTLE_STEP_PRESENT) {
        return;
    }
    memset(&attack, 0, sizeof(attack));
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        DisplayObject *card = D_800E9EF0[side];
        attack.record[side] = card ? card->field_6A : -1;
        if (card) {
            attack.home[side][0] = D_800908A0[card->field_6A].x;
            attack.home[side][1] = D_800908A0[card->field_6A].y;
        }
    }
    attacker = fighter(0);
    defender = attack.record[1] >= 0 ? fighter(1) : NULL;
    slot = attacker ? &attacker->slot : NULL;
    if (!slot || !slot->field_750[slot->field_DFE + 3].max || (D_800E9EF0[1] && !defender)) {
        memset(&attack, 0, sizeof(attack));
        attack.stage = STAGE_FIELD; /* not this battle: the cards go on as ever */
        attack.phase = ATTACK_OVER;
        attack.record[0] = attack.record[1] = -1;
        return;
    }
    /* With no defender it goes for the zone straight across. */
    if (attack.record[1] < 0) {
        attack.home[1][0] = attack.home[0][0];
        attack.home[1][1] = -attack.home[0][1];
    }
    dx = attack.home[1][0] - attack.home[0][0];
    dz = attack.home[1][1] - attack.home[0][1];
    length = (int)sqrt((double)dx * dx + (double)dz * dz);
    reach = abs(attacker->raw_z) + abs(defender ? defender->raw_z : attacker->raw_z);
    reach = reach * attacker->scale / MODEL_FIXED_ONE * tunable("fight_reach", FIGHT_REACH) / 100;
    reach = reach < 0 ? 0 : reach > length ? length : reach;
    attack.gap = reach;
    attack.strike[0] = length ? attack.home[1][0] - dx * reach / length : attack.home[0][0];
    attack.strike[1] = length ? attack.home[1][1] - dz * reach / length : attack.home[0][1];
    attack.yaw[0] = yaw_towards(dx, dz);
    attack.yaw[1] = yaw_towards(-dx, -dz);
    attack.turn = fight_turn(dx, dz);
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        if (D_800E9EF0[side]) {
            D_800E9EF0[side]->flags &= ~DISPLAY_OBJECT_FLAG_RENDERABLE;
        }
    }
    fight_view = D_800F2848;
    attack.stage = STAGE_FIELD;
    attack.phase = ATTACK_FIRST;
    attack.outcome = fight_outcome();
    fight_tally();
    effect_begin(attacker, defender);
    attack.holding = 1;
    say("fight: case %d, record %d against %d, from %d,%d to %d,%d, %d short\n", attack.outcome,
        attack.record[0], attack.record[1], attack.home[0][0], attack.home[0][1], attack.strike[0], attack.strike[1],
        attack.gap);
}

/* p^2 (3 - 2p), in MODEL_FIXED_ONE units: slow, fast, slow. */
static int smooth(int progress)
{
    return progress * progress / MODEL_FIXED_ONE * (3 * MODEL_FIXED_ONE - 2 * progress) / MODEL_FIXED_ONE;
}

/* Where the attacker is along its way, from its zone (0) to its strike
 * (MODEL_FIXED_ONE). */
static int fight_along(void)
{
    int t = attack.frames - FIGHT_APPEAR;
    if (attack.back) {
        return MODEL_FIXED_ONE - smooth(attack.back * MODEL_FIXED_ONE / FIGHT_CHARGE);
    }
    return t <= 0 ? 0 : t >= FIGHT_CHARGE ? MODEL_FIXED_ONE : smooth(t * MODEL_FIXED_ONE / FIGHT_CHARGE);
}

/* The camera for the next frame: the mat is drawn before this pass, so the
 * monsters this frame have to be seen as the mat was. */
static void fight_camera(void)
{
    int zoom = tunable("fight_zoom", FIGHT_ZOOM), share;
    int mid_x = (attack.at[0] + attack.home[1][0]) / 2, mid_z = (attack.at[1] + attack.home[1][1]) / 2;
    zoom = zoom < 20 ? 20 : zoom > 100 ? 100 : zoom;
    if (attack.closing) {
        share = MODEL_FIXED_ONE - smooth((attack.closing < FIGHT_CAMERA ? attack.closing : FIGHT_CAMERA) *
                                         MODEL_FIXED_ONE / FIGHT_CAMERA);
    } else {
        share = smooth((attack.frames < FIGHT_CAMERA ? attack.frames : FIGHT_CAMERA) * MODEL_FIXED_ONE /
                       FIGHT_CAMERA);
    }
    D_800F2848.field_00 =
        (s16)(fight_view.field_00 - fight_view.field_00 * (100 - zoom) / 100 * share / MODEL_FIXED_ONE);
    D_800F2848.angle = (s16)((fight_view.angle + attack.turn * share / MODEL_FIXED_ONE) & (MODEL_ANGLE_FULL_TURN - 1));
    D_800F2848.view.vrx = fight_view.view.vrx + (mid_x - fight_view.view.vrx) * share / MODEL_FIXED_ONE;
    D_800F2848.view.vry = fight_view.view.vry - FIGHT_RISE * share / MODEL_FIXED_ONE;
    D_800F2848.view.vrz = fight_view.view.vrz + (mid_z - fight_view.view.vrz) * share / MODEL_FIXED_ONE;
    ViewState_ApplyOrbit();
    fight_moved = 1;
}

/* draw_monster, with the fight's rows and drift, turned to `facing`. The rows
 * and drift are measured at the yaw it fights at. On the field alone, where
 * its outline's top came out on the screen is kept, for its number. */
static void draw_fighter(int side, Monster *monster, int x, int z, int share, int fade, int lifted, int facing)
{
    ModelSlot *slot = &D_800F2C40[0];
    int yaw = attack.yaw[side], scale = monster->scale * share / MODEL_FIXED_ONE, drift[3];
    double c = rcos(facing) / (double)MODEL_FIXED_ONE, s = rsin(facing) / (double)MODEL_FIXED_ONE;
    int body_x = monster->body_x * share / MODEL_FIXED_ONE;
    int body_y = monster->body_y * share / MODEL_FIXED_ONE;
    int body_z = monster->body_z * share / MODEL_FIXED_ONE;

    *slot = monster->slot;
    attack_side(side, slot, yaw);
    if (attack.row[side] && attack.row[side] == slot->field_DFE + 3) {
        attack_drift(side, slot, yaw, scale, 100, attack.gap, drift);
    } else {
        attack_drift(side, slot, yaw, scale, tunable("attack_drift", ATTACK_DRIFT), FIGHT_DRIFT, drift);
    }
    /* The body offset was measured facing the player's edge; it turns with
     * the monster. */
    place(slot, x - (int)(c * body_x + s * body_z) + drift[0], -body_y - lifted + drift[1],
          z - (int)(c * body_z - s * body_x) + drift[2], facing, scale);
    monster->fade = fade;
    {
        const u32 *start = (const u32 *)(uintptr_t)D_800FE240;
        sort_monster(monster, (GsOT *)D_800E9D98[0], -1);
        if (fight_alone() && packet_height(start, (const u32 *)(uintptr_t)D_800FE240) > 0) {
            attack.head[side][0] = (bounds.left + bounds.right) / 2;
            attack.head[side][1] = bounds.top;
            attack.headed[side] = 1;
        }
    }
    monster->fade = 0;
    if (!monster->stepped) {
        func_800556E8(0);
        monster->stepped = 1;
    }
    monster->slot = *slot;
    monster->used = frame;
}

/* Where a fighter's body is on the screen, through the frame's own
 * world-screen matrix (the camera the mat was drawn with). */
static void fight_screen(int side, int *sx, int *sy)
{
    SVECTOR v;
    PSXLONG sxy, p, flag;
    v.vx = (short)(side ? attack.home[1][0] : attack.at[0]);
    v.vy = (short)-lift();
    v.vz = (short)(side ? attack.home[1][1] : attack.at[1]);
    v.pad = 0;
    GsSetLsMatrix(&D_800FE148);
    RotTransPers(&v, &sxy, &p, &flag);
    *sx = (short)((u32)sxy & 0xFFFF);
    *sy = (short)(((u32)sxy >> 16) & 0xFFFF);
}

/* Where a number goes: over the head of the monster it is for, all of it on
 * the screen. The effect adds its colour to what is under it, so over the
 * monster, a pale one above all, it all but went; the dark behind the fight
 * shows it whole. A direct attack's is over the middle of the screen, above
 * the attacker. */
static void fight_place(int side, int value, int *x, int *y)
{
    int owner = side == 1 && attack.record[1] < 0 ? 0 : side, glyphs = 2, rest, half;
    for (rest = abs(value); rest >= 10; rest /= 10) {
        glyphs++; /* the sign and each digit */
    }
    half = glyphs * NUMBER_HALF;
    if (attack.headed[owner]) {
        *x = attack.head[owner][0];
        *y = attack.head[owner][1] - NUMBER_ABOVE - NUMBER_HALF;
    } else {
        fight_screen(owner, x, y);
    }
    if (owner != side) {
        *x = DIRECT_X;
        *y = *y > DIRECT_Y ? DIRECT_Y : *y;
    }
    *x = *x < half ? half : *x > SCREEN_WIDTH - half ? SCREEN_WIDTH - half : *x;
    *y = *y < NUMBER_HALF ? NUMBER_HALF : *y;
}

/* The numbers whose blows have landed, as step 8 shows them on the cards: a
 * side step 3 leaves untouched shows none, one struck for nothing the flash
 * alone (the effect's own way with 0). */
static void fight_numbers(void)
{
    int side;
    if (!fight_alone()) {
        return;
    }
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        DuelEffectRequest *request;
        int size, x, y;
        if (!attack.due[side] || attack.shown[side]) {
            continue;
        }
        attack.shown[side] = 1;
        if (!attack.wins[side] ||
            !(request = (DuelEffectRequest *)DuelEffect_AllocateRequest(EFFECT_DAMAGE))) {
            continue;
        }
        size = abs(attack.damage[side]) / 1000;
        size = size > 2 ? 2 : size;
        fight_place(side, attack.damage[side], &x, &y);
        request->field_00 = (s16)x;
        request->field_02 = (s16)y;
        request->field_12 = (s16)attack.damage[side];
        request->field_1A = (s16)size;
        attack.number[side] = request;
        SD_SEPlayFull((side ? SOUND_DAMAGE : SOUND_DAMAGE_ATTACKER) + size);
        say("fight: side %d's number %d at %d,%d\n", side, attack.damage[side], x, y);
    }
}

/* A monster that was hit is still falling or going. */
static int fight_going(void)
{
    int side;
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        if (attack.row[side] == ROW_HIT && (!attack.still[side] || attack.gone[side] < FIGHT_VANISH)) {
            return 1;
        }
    }
    return 0;
}

/* On the field alone: every number has been shown and is over, and every
 * destroyed monster has gone. */
static int fight_settled(void)
{
    int side;
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        if (attack.number[side] && (attack.number[side]->flags & DUEL_EFFECT_REQUEST_FLAG_ACTIVE)) {
            return 0;
        }
        attack.number[side] = NULL;
        if ((attack.due[side] && !attack.shown[side]) || (attack.still[side] && attack.gone[side] < FIGHT_VANISH)) {
            return 0;
        }
    }
    return 1;
}

/* The way a fighter faces. On the field alone, while the camera goes back,
 * it turns to the way its zone faces (draw_frame), where it will stand once
 * the battle is over. */
static int fight_facing(int side)
{
    int yaw = attack.yaw[side], stand, turn, share;
    if (!fight_alone() || !attack.closing) {
        return yaw;
    }
    stand = attack.record[side] < SIDE_ZONE(1, 0) ? MODEL_ANGLE_HALF_TURN : 0;
    turn = ((stand - yaw + MODEL_ANGLE_HALF_TURN) & (MODEL_ANGLE_FULL_TURN - 1)) - MODEL_ANGLE_HALF_TURN;
    share = smooth((attack.closing < FIGHT_CAMERA ? attack.closing : FIGHT_CAMERA) * MODEL_FIXED_ONE / FIGHT_CAMERA);
    return (yaw + turn * share / MODEL_FIXED_ONE) & (MODEL_ANGLE_FULL_TURN - 1);
}

/* The fight's frame, in draw_frame's pass for the field; how many it drew. */
static int draw_fighters(void)
{
    Monster *monsters[DUEL_SIDE_COUNT];
    int side, count = 0, along, share = MODEL_FIXED_ONE, fade = 0, lifted = lift(), alone = fight_alone();

    if (attack.stage != STAGE_FIELD || attack.phase == ATTACK_IDLE) {
        return 0;
    }
    if (attack.frames < FIGHT_APPEAR) {
        int progress = attack.frames * MODEL_FIXED_ONE / FIGHT_APPEAR, grown = eased(progress);
        share = SUMMON_FROM + (MODEL_FIXED_ONE - SUMMON_FROM) * grown / MODEL_FIXED_ONE;
        fade = 255 - 255 * progress / MODEL_FIXED_ONE;
        lifted = lifted * grown / MODEL_FIXED_ONE;
    }
    if (attack.frames == FIGHT_APPEAR + FIGHT_CHARGE && attack.phase == ATTACK_FIRST) {
        attack.want[0] = ROW_ATTACK; /* arrived */
    }
    along = fight_along();
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        monsters[side] = attack.record[side] >= 0 ? fighter(side) : NULL;
        if (monsters[side]) {
            monsters[side]->stepped = 0;
        }
    }
    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        int x = attack.home[side][0], z = attack.home[side][1], its_share = share, its_fade = fade;
        if (!monsters[side]) {
            continue;
        }
        if (side == 0) {
            x += (attack.strike[0] - x) * along / MODEL_FIXED_ONE;
            z += (attack.strike[1] - z) * along / MODEL_FIXED_ONE;
            attack.at[0] = x;
            attack.at[1] = z;
        }
        if (alone && attack.still[side]) {
            int progress;
            if (attack.gone[side] >= FIGHT_VANISH) {
                continue; /* gone */
            }
            if (!attack.gone[side]) {
                SD_SEPlayFull(SOUND_DESTROY);
            }
            progress = attack.gone[side]++ * MODEL_FIXED_ONE / FIGHT_VANISH;
            its_share = MODEL_FIXED_ONE - (MODEL_FIXED_ONE - SUMMON_FROM) * eased(progress) / MODEL_FIXED_ONE;
            its_fade = 255 * progress / MODEL_FIXED_ONE;
        }
        draw_fighter(side, monsters[side], x, z, its_share, its_fade, lifted, fight_facing(side));
        count++;
    }
    fight_numbers();
    attack.frames++;
    if (!attack.holding) {
        return count;
    }
    /* Once the blows are over and its row has ended the attacker goes home,
     * unless it was destroyed, and the camera goes back; the battle goes on
     * when both have done. */
    if (attack.phase == ATTACK_OVER) {
        int fell = attack.outcome == CASE_COUNTER || attack.outcome == CASE_TIE;
        int home = fell || attack.back >= FIGHT_CHARGE;
        /* On the field alone the camera stays in, and the attacker where it
         * struck, until the destroyed have gone. */
        int waiting = (alone && fight_going()) || effect_busy();
        if (!home && !attack.row[0] && !waiting) {
            attack.back++;
        }
        if (attack.closing < FIGHT_CAMERA && (fell || !attack.row[0]) && !waiting) {
            attack.closing++;
        }
        if (home && attack.closing >= FIGHT_CAMERA && (!attack.row[0] || attack.still[0]) &&
            (!attack.row[1] || attack.still[1]) && (!alone || fight_settled()) && !effect_busy()) {
            fight_restore();
            attack.holding = 0;
            attack.conclude = alone;
            say("fight: over after %d frames\n", attack.frames);
            return count;
        }
    }
    fight_camera();
    return count;
}

static void draw_frame(void)
{
    static ModelSlot borrowed;
    Standing standing[DUEL_SIDE_COUNT * MONSTER_ZONES];
    u32 work_base;
    int count = 0, i, side, zone, summoning, frames, field, overhead, board_share = MODEL_FIXED_ONE;
    int height = MODEL_FIXED_ONE;

    if (inside) {
        return;
    }
    if (attack.stage == STAGE_FIELD &&
        (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) != DUEL_SCENE_BATTLE) {
        attack_finish(); /* the battle is over */
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
    field = duel_field_up();
    overhead = !field && tunable("board", 0) && D_800E9DB0[3] == Duel_DrawFieldCards &&
               D_800F2C40[2].field_E1F != 0;
    if (!field && !overhead) {
        return;
    }
    frame++;
    inside = 1;
    if (overhead) {
        int from = tunable("pitch", FIELD_PITCH), pitch = D_800F2848.field_04;
        int size = tunable("board_size", BOARD_SIZE), toward;
        size = size < 10 ? 10 : size > 100 ? 100 : size;
        toward = pitch <= from ? 0 : pitch >= TILT_PITCH ? MODEL_FIXED_ONE
                                                         : (pitch - from) * MODEL_FIXED_ONE / (TILT_PITCH - from);
        board_share = MODEL_FIXED_ONE - (MODEL_FIXED_ONE - size * MODEL_FIXED_ONE / 100) * toward / MODEL_FIXED_ONE;
        height = MODEL_FIXED_ONE - (MODEL_FIXED_ONE - BOARD_HEIGHT) * toward / MODEL_FIXED_ONE;
        keep_geometry(0); /* the pass never used to draw here: leave the GTE as found */
    }
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
            if (overhead && (card->flags & DUEL_CARD_FLAG_SPRITE) &&
                *(const s16 *)&card->pad_08[2] + BOARD_CARD_FEET > BOARD_PANEL_TOP) {
                continue; /* its card has gone under the panel */
            }
            /* A card a card mod added stands as the retail card it is a
             * copy of: MODEL.MRG has the disc's monsters only. */
            if (!tunable("test", 0)) {
                id = Cards_ModelId(id);
            }
            /* The record carries a stance of its own for a monster in
             * defence, which is the one the battle presentation would use. */
            monster = acquire(id, (card->flags & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0, 0);
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
            standing[count].at = -1;
            if (overhead && (card->flags & DUEL_CARD_FLAG_SPRITE) && card->object) {
                int at = (int)((DisplayObject *)card->object)->field_14 - (int)((GsOT *)D_800E9D98[0])->offset -
                         BOARD_DEPTH_STEPS;
                standing[count].at = at < 0 ? 0 : at;
            }
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
        share = share * board_share / MODEL_FIXED_ONE;
        lifted = lifted * board_share / MODEL_FIXED_ONE;
        draw_monster(standing[i].monster, standing[i].x, standing[i].z, standing[i].yaw, share, fade, lifted,
                     standing[i].at, height);
        if (summon->elapsed < 0xFFFFu) {
            summon->elapsed++;
        }
    }
    if (field) {
        fight_begin();
        count += draw_fighters();
    }

    D_800F2C40[0] = borrowed;
    if (!count) {
        D_800FE240 = work_base;
    }
    if (overhead) {
        keep_geometry(1);
    } else {
        SetGeomOffset(0, 0);
    }
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
    /* Without this hook the rows still play; the battle only does not wait
     * for the blow. */
    if (from->api >= 4 && !original_battle &&
        !host->hook(host, (void *)DuelScene_UpdateBattle, (void *)update_battle, &original_battle)) {
        say("the attack cannot hold the battle: DuelScene_UpdateBattle could not be hooked\n");
    }
    return 1;
}
