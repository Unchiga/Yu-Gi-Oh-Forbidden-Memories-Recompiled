/* Card Frame Colors - API 4 mod
 *
 * The card frame is one texture drawn through one of six palette rows the
 * disc uploads with every package that shows cards (duel terrains, Library,
 * Password, Build Deck; notes/modding-tutorial-evidence.md):
 *
 *   slot  colour   retail use
 *   0     yellow   every monster
 *   1     green    Magic and Equip
 *   2     pink     Trap
 *   3     blue     Ritual (the spell)
 *   4     purple   none
 *   5     orange   none
 *
 * Small card (hand, field): row 241 + slot. Large card: row 248 + slot.
 * Rows 245/246 and 252/253 are byte-identical in every package that draws
 * the frame, so nothing is uploaded: this only chooses a different row for
 * monsters, as the TCG colours them (card_classes.h).
 *
 * Three places draw a frame, and each is wrapped:
 *   func_800291E0  the large card; its frame object's CLUT row is set by
 *                  DisplayObject_ConfigureSpriteResource to 0xF8 + slot
 *   func_80016784  the 2D small card; the face goes through
 *                  DisplayObject_SubmitPacket with cy 0xF1, so that call is
 *                  retargeted while the card is drawn
 *   func_80015EF4  the 3D field card; row 0xF1 + object->field_42
 *
 * A face-down card is never tinted: on the field it keeps the face's
 * texture and is only turned over (Duel_ApplyCardObjectFlags), the back
 * showing because the quad faces away, so its back would be drawn through
 * the tinted row and tell what the card is. The card record's
 * DUEL_CARD_FLAG_FACE_DOWN is what decides.
 *
 * Only the player's cards are tinted: against the computer its side (card
 * records 15-29, notes/duel-card-record.md) keeps the retail frames. The
 * small and field cards come with their record. The large card only gets
 * a card ID, so its record is the one the duel handed over just before:
 * Duel_CalcCardStats's, which battle and placement call for it at once;
 * Duel_GetCardViewerRequestId's for the viewer (a hand card opens it
 * without one, and is the player's); the active side for the rest (a
 * ritual's result). Outside a duel (Library, Password) nothing is held and
 * every card is tinted.
 *
 * The Library's grid thumbnails have only the four retail palettes and keep
 * them. */
#include "types.h"
#include "ygo_types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "game/display_object.h"
#include "game/duel_card.h"
#include "game/duel_card_layout.h"
#include "game/duel_effect_resource_record.h"
#include "game/func_800291E0.h"
#include "game/func_80016784.h"
#include "game/func_80015EF4.h"
#include "game/duel_get_card_viewer_request_id.h"
#include "pc/cards/tables.h"
#include "pc/mods/modapi.h"

#include "card_classes.h"

#define CARD_COUNT 722
#define TYPE_MAGIC 20
#define SMALL_ROW 0xF1
#define LARGE_ROW 0xF8
#define SMALL_SPRITE ((SpritePrim *)0x1F800320)
#define SMALL_FACE_TPAGE 0x1E
#define VIEWER_INDEX 3   /* the resource slot func_800283F4 opens the viewer's card in */

enum { SLOT_YELLOW, SLOT_GREEN, SLOT_PINK, SLOT_BLUE, SLOT_PURPLE, SLOT_ORANGE };
enum { MODE_RETAIL, MODE_TCG, MODE_FM };

static const MemoriesModHost *host;
static void *original_large, *original_small, *original_field, *original_submit;
static void *original_stats, *original_viewer;
static int mode = MODE_TCG;
static int small_row;   /* the face row func_80016784 is drawing, 0 when none */
static const u8 *active_side;               /* D_8009B1D5 */
static DuelCardRecord *stats_card, *viewer_card;
static int drew_duel, in_duel;              /* a duel card drawn this frame, and last frame */

/* Whether a card record is on the computer's side. */
static int computer_side(int side)
{
    return side == 1 && Tables_OpponentId() >= 0;
}

static int computer_card(const DuelCardRecord *card)
{
    return computer_side((int)(card - D_801A7AD8) / DUEL_CARD_SIDE_RECORD_COUNT);
}

/* The slot a card's frame is drawn with, or -1 to leave the game's own. */
static int monster_slot(int id)
{
    unsigned char c;

    if (id < 1 || id > CARD_COUNT) return -1;
    if (((gDuel_adwCardStats[id - 1] >> 26) & 0x1F) >= TYPE_MAGIC) return -1;
    c = card_classes[id];
    switch (mode) {
    case MODE_TCG:
        switch (c & 3) {
        case 1: return SLOT_ORANGE;
        case 2: return SLOT_PURPLE;
        case 3: return SLOT_BLUE;
        }
        return -1;
    case MODE_FM:
        if (c & 0x20) return SLOT_BLUE;
        if (c & 0x10) return SLOT_PURPLE;
        return -1;
    }
    return -1;
}

static u8 *large_card(s32 index, s32 x, s32 y)
{
    DisplayObject *frame = (DisplayObject *)((u8 *(*)(s32, s32, s32))original_large)(index, x, y);
    int slot = monster_slot((s16)D_800EA0E8[index].field_30), computer = 0;

    if (in_duel) {
        if (index == VIEWER_INDEX) {
            computer = viewer_card && computer_card(viewer_card);
            viewer_card = 0;
        } else if (x >= 0 && stats_card) {
            computer = computer_card(stats_card);
        } else {
            computer = active_side && computer_side(*active_side);
        }
    }
    if (computer) slot = -1;

    if (frame && slot > 0) {
        frame->field_40.h.field_42 = LARGE_ROW + slot;
        frame->flags &= 0xFFEF;   /* rebuild its command stream */
    }
    return (u8 *)frame;
}

static void small_card(DisplayObject *object, s32 ot, s32 x, s32 y)
{
    DuelCardRecord *card = &D_801A7AD8[object->field_6A];
    int slot = card->flags & DUEL_CARD_FLAG_FACE_DOWN || computer_card(card) ? -1 : monster_slot(card->card_id);

    drew_duel = 1;
    small_row = slot > 0 ? SMALL_ROW + slot : 0;
    ((void (*)(DisplayObject *, s32, s32, s32))original_small)(object, ot, x, y);
    small_row = 0;
}

static void submit(SpritePrim *sprite, u8 *packet, s32 ot, s32 mode_word, u8 *extra)
{
    /* Only the face: the back (u 0x38) and the digits draw on the same row. */
    if (small_row && sprite == SMALL_SPRITE && sprite->tpage == SMALL_FACE_TPAGE &&
        sprite->cxcy.h.cy == SMALL_ROW && sprite->uv.b.lo == 0 && sprite->uv.b.hi >= 0x80)
        sprite->cxcy.h.cy = (u16)small_row;
    ((void (*)(SpritePrim *, u8 *, s32, s32, u8 *))original_submit)(sprite, packet, ot, mode_word, extra);
}

static void field_card(void *record, POLY_GT4 *prim, POLY_FT4 *sprite, s32 *color)
{
    DuelCardRecord *card = record;
    DisplayObject *object = card->object;
    int slot = card->flags & DUEL_CARD_FLAG_FACE_DOWN || computer_card(card) ? -1 : monster_slot(card->card_id);
    s16 row = 0;

    drew_duel = 1;

    if (object && slot > 0) {
        row = object->field_40.h.field_42;
        object->field_40.h.field_42 = (s16)slot;
    } else {
        object = 0;
    }
    ((void (*)(void *, POLY_GT4 *, POLY_FT4 *, s32 *))original_field)(record, prim, sprite, color);
    if (object) object->field_40.h.field_42 = row;
}

static s32 card_stats(DuelCardRecord *card)
{
    stats_card = card;
    return ((s32 (*)(DuelCardRecord *))original_stats)(card);
}

static s32 viewer_request(DuelCardRecord *card)
{
    s32 id = ((s32 (*)(DuelCardRecord *))original_viewer)(card);
    if (id) viewer_card = card;
    return id;
}

static void frame(void)
{
    mode = host->setting(host, "mode", MODE_TCG);
    in_duel = drew_duel;
    drew_duel = 0;
    stats_card = 0;
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4) return 0;
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = frame;
    frame();
    active_side = host->symbol(host, "D_8009B1D5");
    return host->hook(host, (void *)func_800291E0, (void *)large_card, &original_large) &&
           host->hook(host, (void *)func_80016784, (void *)small_card, &original_small) &&
           host->hook(host, (void *)DisplayObject_SubmitPacket, (void *)submit, &original_submit) &&
           host->hook(host, (void *)func_80015EF4, (void *)field_card, &original_field) &&
           host->hook(host, (void *)Duel_CalcCardStats, (void *)card_stats, &original_stats) &&
           host->hook(host, (void *)Duel_GetCardViewerRequestId, (void *)viewer_request, &original_viewer);
}
