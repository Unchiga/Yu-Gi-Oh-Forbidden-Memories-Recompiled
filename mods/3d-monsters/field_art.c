/* The 3D Monsters mod's "Card art" style (mod.json beside this file, the
 * "style" setting; notes/modding.md): where the "3D models" style stands a
 * battle model on a face-up field card, this stands an enlarged cutout of
 * the card's own art instead, floating just above it, always facing the
 * camera. No model, no arena, no VRAM budget to borrow: a card's art record
 * is seven sectors of WA_MRG.MRG, the same disc data the card-detail panel
 * and the Library already draw large, held here one bank per cached card in
 * the software GPU (src/pc/render/soft_gpu.h), exactly as field_models.c
 * holds a model's textures. field_models.c's own draw_frame calls
 * FieldArt_DrawFrame (below) instead of drawing a model, when the style
 * setting says to; this file has no MemoriesModInit of its own.
 *
 * Unlike a model, a cutout has no size of its own to measure: every card's
 * art is the same 102x96 record, so one world-space height serves every
 * zone, and perspective alone makes a farther card's cutout smaller -- the
 * fitting loop below (fit_height) finds that height once a frame, the same
 * way the 3D Monsters mod's fit() finds a model's scale, but by projecting
 * two points instead of measuring drawn packets.
 *
 * Drawing is a single POLY_FT4 per face-up monster, its four corners placed
 * in screen space from two world points (the card's own field position, and
 * that position lifted by the fitted height) run through RotTransPers under
 * the frame's own world-screen matrix (D_800FE148, GsSetRefView2's own),
 * exactly as func_80015EF4 projects a field card's ground sprite. The same
 * call's returned depth is at RotTransPers' own native resolution, a quarter
 * as fine as the card scale func_80015EF4 sorts a field card's own upright
 * quad at (its own comment on a model's primitives); draw_one() divides it
 * down the same single step the 3D Monsters mod's sort_monster does
 * (`nearest / 4`) before it sorts the cutout into D_800E9D90[2] -- the same
 * table func_80015EF4 sorts that upright quad into, and the one the 3D
 * Monsters mod's own field-standing draw_monster() uses too (as
 * D_800E9D98[0], its own comment's name for the same table): shared with the
 * field's own card geometry, not borrowed for being unused, so a cutout
 * sorts correctly against the card it stands on and against other cutouts. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "ygo_types.h"
#include "game/duel_card.h"
#include "game/duel_card_layout.h"
#define DUEL_SCREEN_TABLES_TYPED_POSITIONS
#include "game/duel_screen_tables.h"
#include "game/view_state.h"
#include "game/main_services.h"
#include "game/ordering_tables.h"
#include "game/duel_display.h"
#include "game/model.h"
#include "game/card_constants.h"
#include "pc/render/soft_gpu.h"
#include "pc/mods/modapi.h"
#include "pc/cards/cards.h"
#include "pc/cards/art.h"
#include "field_art.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern MATRIX D_800FE148;      /* GsWSMATRIX: GsSetRefView2's world-screen matrix */

static const MemoriesModHost *host;
static unsigned frame;

/* Cards kept loaded, least recently drawn replaced first (3D Monsters' own
 * CACHE and acquire() explain the reasoning; a bank is bank = index + 1,
 * below SOFT_GPU_BANKS). */
#define CACHE 12

typedef struct {
    int card;      /* one-based card id; 0 when the entry is free */
    unsigned used; /* frame number of the last draw, for replacement */
    int bank;      /* its texture bank in the software GPU */
} Art;

static Art cache[CACHE];
static u8 *record;             /* one card's WA_MRG.MRG art record, read whole */
static int mrg_start = -2;

void FieldArt_Reset(void)
{
    int i;
    for (i = 0; i < CACHE; i++) {
        cache[i].card = 0;
    }
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

static int tunable(const char *key, int fallback)
{
    return host->setting(host, key, fallback);
}

#define SECTOR 2048
#define ART_SECTORS 7

/* Where the art's pixels and its CLUT sit inside the card's bank: any two
 * places that do not overlap each other, nor the model's textures
 * (field_models.c) or, in bank 1, the glow (GLOW_PIXELS_X below). */
#define PIXELS_X 0
#define PIXELS_Y 0
#define CLUT_X 0
#define CLUT_Y CARD_ART_HEIGHT

static void bank_put(u16 *bank, int x, int y, int w, int h, const u16 *pixels)
{
    int row;
    for (row = 0; row < h; row++) {
        memcpy(bank + (size_t)(y + row) * SOFT_GPU_WIDTH + x, pixels + (size_t)row * w, (size_t)w * 2);
    }
}

static int load_art(Art *art, int card)
{
    int base, sectors;
    u16 *bank;
    const unsigned char *field_art;
    const u16 *pixels, *clut;

    /* A mod's own picture for the cutout alone ("field_art",
     * notes/more-cards.md): never patched into the card's own record, so
     * nothing else the card's art shows is touched by it, and none of the
     * disc reading below is needed. */
    field_art = Cards_FieldArtRecord(card);
    if (!field_art) {
        if (mrg_start == -2) {
            mrg_start = host->disc_file_start(host, "\\DATA\\WA_MRG.MRG;1");
            say("WA_MRG.MRG starts at sector %d\n", mrg_start);
        }
        if (mrg_start < 0) {
            return 0;
        }
        if (!record && !(record = malloc(ART_SECTORS * SECTOR))) {
            return 0;
        }
        /* A card past the disc's 722 has no record of its own; it stands as
         * the retail card it is a copy of, exactly as func_80029164 reads it
         * for the card-detail panel. */
        base = Cards_BaseId(card);
        sectors = host->disc_read(host, mrg_start + (base - 1) * ART_SECTORS + CARD_COUNT, ART_SECTORS, record);
        if (sectors != ART_SECTORS) {
            say("card %d: read %d of %d sectors\n", card, sectors, ART_SECTORS);
            return 0;
        }
        /* A mod's own "art" artwork over the base's, exactly as the
         * card-detail panel gets it (func_800289BC). */
        Cards_PatchArtRecord(card, record);
        pixels = (const u16 *)(record + CARD_ART_PIXELS);
        clut = (const u16 *)(record + CARD_ART_CLUT);
    } else {
        pixels = (const u16 *)(field_art + CARD_ART_PIXELS);
        clut = (const u16 *)(field_art + CARD_ART_CLUT);
    }

    bank = SoftGpu_Bank(art->bank);
    if (!bank) {
        say("no bank %d\n", art->bank);
        return 0;
    }
    bank_put(bank, PIXELS_X, PIXELS_Y, CARD_ART_WIDTH / 2, CARD_ART_HEIGHT, pixels);
    bank_put(bank, CLUT_X, CLUT_Y, 256, 1, clut);
    art->card = card;
    return 1;
}

static Art *acquire(int card)
{
    Art *art = NULL;
    int i;
    for (i = 0; i < CACHE; i++) {
        if (cache[i].card == card) {
            cache[i].used = frame;
            return &cache[i];
        }
    }
    for (i = 0; i < CACHE; i++) {
        if (!cache[i].card) {
            art = &cache[i];
            break;
        }
        if (!art || cache[i].used < art->used) {
            art = &cache[i];
        }
    }
    art->bank = (int)(art - cache) + 1;
    art->card = 0;
    if (!load_art(art, card)) {
        return NULL;
    }
    art->used = frame;
    return art;
}

/* A world point to screen, under the frame's own world-screen matrix, as
 * func_80015EF4 projects a field card's flattened ground sprite. Returns
 * RotTransPers' own depth: SZ already shifted the way a model's is (its own
 * comment, "a quarter of its distance"), ready for GsSortPoly. */
static int project(int x, int y, int z, int *sx, int *sy)
{
    SVECTOR v;
    PSXLONG sxy, p, flag, depth;   /* the Psy-Q long: 32 bits on every target */
    v.vx = (short)x;
    v.vy = (short)y;
    v.vz = (short)z;
    v.pad = 0;
    GsSetLsMatrix(&D_800FE148);
    depth = RotTransPers(&v, &sxy, &p, &flag);
    *sx = (short)((u32)sxy & 0xFFFF);
    *sy = (short)(((u32)sxy >> 16) & 0xFFFF);
    return (int)depth;
}

#define LIFT_PIXELS 8
#define LIFT_UNITS_PER_PIXEL 2
static int lift(void)
{
    return tunable("lift", LIFT_PIXELS * LIFT_UNITS_PER_PIXEL);
}

/* The world-space height that projects to `pixels` game pixels tall at the
 * middle of the field: found the way the 3D Monsters mod's fit() finds a
 * model's scale, by projecting instead of measuring drawn packets, because
 * every card's cutout is the same 102x96 record and needs the same height.
 * DEFAULT_PIXELS matches the 3D Monsters mod's own TALL_PIXELS: the same
 * "about this tall in the middle of the field" target its models are fit to,
 * so a cutout should read at the same scale a battle model would. */
#define MIDDLE_X 0
#define MIDDLE_Z 0
#define HEIGHT_DEFAULT 700
/* A floor only against a degenerate `got` (a near-zero or negative pixel
 * delta), not a realistic lower bound on the answer: live logging under the
 * field's actual camera (target 40, DEFAULT_PIXELS's old value) found 128
 * world units already project to 66 px, well past the target, with the loop
 * wanting to settle around 77. A HEIGHT_SMALLEST of 128 -- copied by analogy
 * from the 3D Monsters mod's SCALE_SMALLEST, a fixed-point scale *fraction*
 * (of MODEL_FIXED_ONE), not a raw world-unit height, so the same number
 * means something else entirely here -- clamped every attempt back up to
 * itself, so the loop could never reach the smaller height it kept computing
 * and every cutout was stuck oversized. */
#define HEIGHT_SMALLEST 16
#define HEIGHT_LARGEST 8192
#define DEFAULT_PIXELS 32

static int fit_height(void)
{
    int target = tunable("pixels", DEFAULT_PIXELS), height = HEIGHT_DEFAULT, attempt, got = 0;
    for (attempt = 0; attempt < 5; attempt++) {
        int sx, base_sy, top_sy, wanted;
        project(MIDDLE_X, -lift(), MIDDLE_Z, &sx, &base_sy);
        project(MIDDLE_X, -lift() - height, MIDDLE_Z, &sx, &top_sy);
        got = base_sy - top_sy;
        if (got <= 0) {
            break;
        }
        wanted = height * target / got;
        if (wanted > height * 15 / 16 && wanted < height * 17 / 16) {
            break;
        }
        height = wanted < HEIGHT_SMALLEST ? HEIGHT_SMALLEST : wanted > HEIGHT_LARGEST ? HEIGHT_LARGEST : wanted;
    }
    /* Left in deliberately, gated on the same mods log the rest of this file
     * uses: cheap to check against next time the size looks off. */
    say("fit_height: target %d px, got %d px, height %d after %d attempt(s)\n", target, got, height, attempt + 1);
    return height;
}

/* The duel field seen from above is the one place this draws (field_models.c's
 * own duel_field_up() explains why: the overview camera). */
#define FIELD_PITCH 512

static int duel_field_up(void)
{
    return D_800E9DB0[3] == Duel_DrawFieldCards && D_800F2C40[2].field_E1F != 0 &&
           D_800F2848.field_04 < tunable("pitch", FIELD_PITCH);
}

#define MONSTER_ZONES 5
#define SIDE_ZONE(side, zone) ((side) ? 20 + (zone) : 5 + (zone))
#define DEPTH_STEPS 3

/* A pulsing glow diffusing outward from the cutout's own rectangle ("glow"
 * setting; on by default): one continuous, soft gradient -- bright right at
 * the card's edge, fading smoothly out with distance, rounded at the
 * corners rather than four strips meeting at a seam (which is what the
 * first attempt at this did: visible double-bright overlaps at the
 * corners, and banding read as "on/off" rather than a smooth breathing
 * glow). A place in the first cached card's bank that no cached card uses
 * (its art is at x 0, its model's textures from y 0xF0 and x 0x200 on, in
 * field_models.c; bank 13, past the cache, is the card frames', which wrote
 * over a glow kept there) holds one square texture, GLOW_SIZE across,
 * generated once: at each texel, in coordinates normalised to [-1, 1]
 * across the texture, INNER_HALF is the half-width of a middle square left
 * untouched (it sits under the cutout itself, which draws over it, so its
 * exact brightness there does not matter); outside it, the texel's index is
 * the Euclidean distance to the middle square's nearest edge or corner,
 * scaled so it reaches 0 -- fully transparent, the same hardware rule as
 * the earlier attempts relied on -- at the texture's own edge along x or y,
 * and clamps there before the actual corners, which is what rounds them off
 * rather than squaring them like the first attempt's strips did. A
 * 256-entry CLUT turns every other index into that much white, STP set
 * (BGR555's bit 15, the semi-transparency flag the renderer reads alongside
 * the primitive's own setSemiTrans -- both must be on, or it draws opaque,
 * gl_picture.c). The glow's own colour is entirely the primitive's
 * r0/g0/b0 modulation (settings glow_r/g/b), scaled each frame by a sine
 * wave (glow_period, frames per full pulse) between a floor and full
 * brightness -- never down to fully off, which is what read as "on/off"
 * rather than a breathing pulse -- for the "shining" look; abr 1 (GetTPage's
 * second argument) is the additive blend equation, so the glow brightens
 * whatever is under it instead of covering it. One quad, its middle square
 * scaled to match the cutout's own rectangle exactly (so the diffusing part
 * sits outside it) and its overall size then scaled again by glow_reach, at
 * the cutout's own depth so it sorts with it. */
#define GLOW_BANK 1
#define GLOW_SIZE 48
#define GLOW_PIXELS_X 0x100   /* a page of its own: x 0x100 to 0x117, y 0 to 47 */
#define GLOW_PIXELS_Y 0
#define GLOW_CLUT_X 0x100
#define GLOW_CLUT_Y GLOW_SIZE
#define GLOW_INNER_HALF 0.30 /* the middle square's half-width, normalised */
#define GLOW_REACH_DEFAULT 50
#define GLOW_PERIOD_DEFAULT 90
#define GLOW_FLOOR 0.35 /* the pulse's dimmest point: never fully off */
#define GLOW_R_DEFAULT 255
#define GLOW_G_DEFAULT 255
#define GLOW_B_DEFAULT 90

static int glow_ready;

static void glow_init(void)
{
    u16 *bank;
    u16 pixels[GLOW_SIZE * GLOW_SIZE / 2], clut[256];
    double middle = (GLOW_SIZE - 1) / 2.0, max_distance = 1.0 - GLOW_INNER_HALF;
    int x, y, i;

    if (glow_ready) return;
    bank = SoftGpu_Bank(GLOW_BANK);
    if (!bank) return;

    for (y = 0; y < GLOW_SIZE; y++) {
        double ny = (y - middle) / middle, dy = fabs(ny) > GLOW_INNER_HALF ? fabs(ny) - GLOW_INNER_HALF : 0.0;
        for (x = 0; x < GLOW_SIZE; x++) {
            double nx = (x - middle) / middle, dx = fabs(nx) > GLOW_INNER_HALF ? fabs(nx) - GLOW_INNER_HALF : 0.0;
            double distance = sqrt(dx * dx + dy * dy) / max_distance;
            /* Squared falloff, softer near the card and steeper further
             * out, than a linear ramp would read. */
            double level = distance >= 1.0 ? 0.0 : (1.0 - distance) * (1.0 - distance);
            int index = (int)(level * 255.0 + 0.5), word = x / 2;
            u16 texel = (u16)(index & 0xff) << ((x & 1) * 8);
            if (x & 1) pixels[y * (GLOW_SIZE / 2) + word] |= texel;
            else pixels[y * (GLOW_SIZE / 2) + word] = texel;
        }
    }
    clut[0] = 0; /* never sampled: index 0 is always transparent */
    for (i = 1; i < 256; i++) {
        int component = i * 31 / 255;
        clut[i] = (u16)(0x8000 | (component << 10) | (component << 5) | component); /* white, STP set */
    }
    bank_put(bank, GLOW_PIXELS_X, GLOW_PIXELS_Y, GLOW_SIZE / 2, GLOW_SIZE, pixels);
    bank_put(bank, GLOW_CLUT_X, GLOW_CLUT_Y, 256, 1, clut);
    glow_ready = 1;
}

static void draw_glow(GsOT *table, unsigned short depth, int cx, int cy, int width_px, int height_px)
{
    POLY_FT4 prim;
    int reach, period, phase, half_w, half_h, r, g, b;
    double level;

    if (!tunable("glow", 1)) return;
    reach = tunable("glow_reach", GLOW_REACH_DEFAULT);
    period = tunable("glow_period", GLOW_PERIOD_DEFAULT);
    if (reach < 1 || period < 1) return;
    glow_init();
    if (!glow_ready) return;

    /* The quad's half-size such that its middle GLOW_INNER_HALF fraction
     * matches the cutout's own half-size exactly, then scaled again by the
     * user's reach. */
    half_w = (int)(width_px / (2.0 * GLOW_INNER_HALF) * reach / 100.0);
    half_h = (int)(height_px / (2.0 * GLOW_INNER_HALF) * reach / 100.0);
    if (half_w < 1 || half_h < 1) return;

    phase = (int)(frame % (unsigned)period);
    level = GLOW_FLOOR + (1.0 - GLOW_FLOOR) * (sin(2.0 * 3.14159265358979323846 * phase / period) * 0.5 + 0.5);
    r = (int)(tunable("glow_r", GLOW_R_DEFAULT) * level);
    g = (int)(tunable("glow_g", GLOW_G_DEFAULT) * level);
    b = (int)(tunable("glow_b", GLOW_B_DEFAULT) * level);

    /* Zeroed first: the port reads pad2 as the bank fade (SOFT_GPU_FADE). */
    memset(&prim, 0, sizeof(prim));
    setPolyFT4(&prim);
    setSemiTrans(&prim, 1);
    prim.r0 = (u8)r;
    prim.g0 = (u8)g;
    prim.b0 = (u8)b;
    prim.tpage = GetTPage(1, 1, GLOW_PIXELS_X, GLOW_PIXELS_Y) | (u16)(GLOW_BANK << 11);
    prim.clut = GetClut(GLOW_CLUT_X, GLOW_CLUT_Y);
    prim.x0 = (short)(cx - half_w);
    prim.y0 = (short)(cy - half_h);
    prim.u0 = 0;
    prim.v0 = 0;
    prim.x1 = (short)(cx + half_w);
    prim.y1 = (short)(cy - half_h);
    prim.u1 = GLOW_SIZE - 1;
    prim.v1 = 0;
    prim.x2 = (short)(cx - half_w);
    prim.y2 = (short)(cy + half_h);
    prim.u2 = 0;
    prim.v2 = GLOW_SIZE - 1;
    prim.x3 = (short)(cx + half_w);
    prim.y3 = (short)(cy + half_h);
    prim.u3 = GLOW_SIZE - 1;
    prim.v3 = GLOW_SIZE - 1;
    GsSortPoly(&prim, table, depth);
}

static void draw_one(int index, int world_height)
{
    DuelCardRecord *card = &D_801A7AD8[index];
    Art *art;
    int id = card->card_id;
    int wx, wz, base_sx, base_sy, top_sx, top_sy, depth;
    int height_px, width_px, cx;
    POLY_FT4 prim;
    GsOT *table;

    if (!(card->flags & DUEL_CARD_FLAG_OCCUPIED) || (card->flags & DUEL_CARD_FLAG_FACE_DOWN) || id <= 0 ||
        ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
        return; /* empty, face down, or a magic or trap card */
    }
    art = acquire(id);
    if (!art) {
        return;
    }

    wx = D_800908A0[index].x;
    wz = D_800908A0[index].y;
    depth = project(wx, -lift(), wz, &base_sx, &base_sy);
    project(wx, -lift() - world_height, wz, &top_sx, &top_sy);

    height_px = base_sy - top_sy;
    if (height_px <= 0) {
        return; /* behind the camera, or degenerate */
    }
    width_px = height_px * CARD_ART_WIDTH / CARD_ART_HEIGHT;
    cx = (base_sx + top_sx) / 2;

    /* Zeroed first: the port reads pad2 as the bank fade (SOFT_GPU_FADE). */
    memset(&prim, 0, sizeof(prim));
    setPolyFT4(&prim);
    prim.r0 = prim.g0 = prim.b0 = 0x80;
    prim.tpage = GetTPage(1, 0, PIXELS_X, PIXELS_Y) | (u16)(art->bank << 11);
    prim.clut = GetClut(CLUT_X, CLUT_Y);
    prim.x0 = (short)(cx - width_px / 2);
    prim.y0 = (short)top_sy;
    prim.u0 = 0;
    prim.v0 = 0;
    prim.x1 = (short)(cx + width_px / 2);
    prim.y1 = (short)top_sy;
    prim.u1 = CARD_ART_WIDTH - 1;
    prim.v1 = 0;
    prim.x2 = (short)(cx - width_px / 2);
    prim.y2 = (short)base_sy;
    prim.u2 = 0;
    prim.v2 = CARD_ART_HEIGHT - 1;
    prim.x3 = (short)(cx + width_px / 2);
    prim.y3 = (short)base_sy;
    prim.u3 = CARD_ART_WIDTH - 1;
    prim.v3 = CARD_ART_HEIGHT - 1;

    /* D_800E9D90[0] (what this used to target) is a real, but tiny, table in
     * this state -- 4 depth slots, confirmed live (table_length 2) -- meant
     * for something else entirely; every cutout's depth saturated at its
     * ceiling regardless of position, so draw order came down to insertion
     * order, not depth, against anything sharing that slot. D_800E9D90[2] is
     * what func_80015EF4 actually sorts a field card's own upright quad into,
     * and what the 3D Monsters mod's field-standing draw_monster() uses too
     * (its own D_800E9D98[0]) -- a real-sized table shared with the field's
     * own geometry, so a cutout sorts correctly against the card under it. */
    table = D_800E9D90[2];
    /* RotTransPers's raw depth is at a model's own native resolution, a
     * quarter as fine as this table's card scale (func_80015EF4's own
     * comment on a model's primitives); the 3D Monsters mod's sort_monster
     * divides its own raw "nearest" the same single step, `nearest / 4`,
     * before using it here. */
    depth = depth / 4 - tunable("depth", DEPTH_STEPS);
    depth = depth < 0 ? 0 : depth >= (1 << table->length) ? (1 << table->length) - 1 : depth;
    GsSortPoly(&prim, table, (unsigned short)depth);
    draw_glow(table, (unsigned short)depth, cx, (top_sy + base_sy) / 2, width_px, height_px);
}

/* Called from field_models.c's own draw_frame when "style" (mod.json) is
 * Card art, instead of registering its own MemoriesModInit: a mod has one
 * entry point, and this file is compiled into the 3D Monsters mod's object
 * alongside field_models.c (build_mod.py merges every .c in a mod's
 * directory into one), not a mod of its own. */
void FieldArt_DrawFrame(void)
{
    int side, zone, world_height;

    if (!duel_field_up()) {
        return;
    }
    frame++;

    /* The projection the duel draws its own field with, exactly as
     * Duel_DrawFieldCards and the 3D Monsters mod's draw_frame set it up.
     * fit_height()'s own project() calls need this in place first: it
     * calibrates world_height by measuring screen pixels under this same
     * matrix/scale, so it has to run under the field's camera, not whatever
     * was left over from the previous draw call this frame. */
    GsSetRefView2(&D_800F2848.view);
    SetGeomScreen(D_800F2848.projection);
    SetGeomOffset(0xA0, 0x6C);

    world_height = fit_height();

    for (side = 0; side < 2; side++) {
        for (zone = 0; zone < MONSTER_ZONES; zone++) {
            draw_one(SIDE_ZONE(side, zone), world_height);
        }
    }
    SetGeomOffset(0, 0);
}

void FieldArt_Init(const MemoriesModHost *from)
{
    host = from;
}
