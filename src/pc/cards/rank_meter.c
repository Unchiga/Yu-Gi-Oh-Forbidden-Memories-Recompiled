/* View > Duel rank: the rank the duel is heading for, drawn by the host over
 * the picture as the fusion helper is, with the result screen's own
 * pictures (rank_art.c). The numbers come from rank.c. */
#define D_8009B360_AS_SIDE_ARRAY
#include "rank_meter.h"
#include "rank.h"
#include "fusion_helper.h"
#include "pc/platform/settings.h"
#include "pc/platform/platform.h"
#include "rank_art.h"
#include "game/duel_scene_state.h"
#include "game/duel_effect.h"
#include "game/duel_check_quit_input.h"
#include "game/duel_action_lock.h"
#include "game/duel_result_display.h"
#include "game/ai_opponent_data.h"
#include "game/duel_side_state.h"
#include "game/duel_init_scene.h"
#include "game/duel_effect_resource_record.h"
#include "game/display_object.h"
#include "duel_ui.h"
#include "pc/platform/ui_config.h"
#include <stdio.h>

extern unsigned char D_8009B26C, D_8009B26E;

/* Scene phases (gDuel_apfnSceneStateHandler) from the first draw to the turn
 * switch; 12 on are the outro and the result screens, which show the rank.
 * DuelScene_UpdateCardUse shows the card being used across the screen. */
enum { PHASE_FIRST_DRAW = 2, PHASE_CARD_USE = 6, PHASE_LAST_PLAY = 11, PHASE_RESULTS = 13 };
/* An LP win (DuelScene_UpdateFieldActions writes 2 to the winner's record);
 * until the duel ends, the rank is shown as if it ended that way. */
enum { ADJUST_LP_WIN = 2 };

static struct { int visible, level, score, tec, tier, box_x, box_y, x, y, w, h; } view;
static int checked;

static int cpu_duel(void)
{
    return (D_8009B26C & 31) == 3 && D_8009B26E == 0x81 && D_8009B360[0] < 0 && gDuel_bOpponentID >= 0;
}

/* A duel preview uses a linked pair of display objects. Check the existing
 * resources rather than tracking a separate flag for each duel action. */
static int large_card_preview_visible(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        const DuelEffectResourceRecord *entry = &D_800EA0E8[i];
        const DisplayObject *card = (const DisplayObject *)entry->object_00;
        const DisplayObject *details = (const DisplayObject *)entry->object_04;

        if (!card || !details || details->field_54 != card) continue;
        if ((card->flags & DISPLAY_OBJECT_RENDERABLE_MASK) != DISPLAY_OBJECT_RENDERABLE_MASK) continue;
        if ((details->flags & DISPLAY_OBJECT_RENDERABLE_MASK) != DISPLAY_OBJECT_RENDERABLE_MASK) continue;
        if (details->attribute & 0x80000000u) continue; /* GsDOFF */
        return 1;
    }
    return 0;
}

/* Once a duel, as the result screen opens: the same sum over the player's
 * record with the adjustment the duel ended with against the game's own. */
static void check_result(void)
{
    const DuelResultDisplayState *result = D_8009B1E8;
    int ours, tec, tier;
    if (checked || !result) return;
    checked = 1;
    ours = Rank_Score(&D_800E9FF0[0], D_800E9FF0[0].rank.result_adjustment);
    Rank_Grade(ours, &tec, &tier);
    fprintf(stderr, "memories-pc: duel rank: ours %d, the game's %d (winner side %d, %s)\n", ours,
            (int)result->side_scores[0], gDuel_bWinnerSide,
            gDuel_bWinnerSide ? "no rank" :
            tec == result->is_tec_rank && tier == result->rank_tier ? "same rank" : "RANK DIFFERS");
}

static void update(void)
{
    int level = Settings_Get(SET_RANK_METER), phase, adjustment, score;
    view.visible = 0;
    if (!level) return;
    phase = gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK;
    if (!cpu_duel()) return;
    if (phase == PHASE_RESULTS && (gDuel_wSceneStateFlags & DUEL_SCENE_FLAG_INITIALIZED)) check_result();
    if (phase < PHASE_FIRST_DRAW || phase > PHASE_LAST_PLAY) return;
    checked = 0;
    if (phase == PHASE_CARD_USE) return;
    if (large_card_preview_visible()) return;
    /* The card viewer, a card's effect being shown, the quit dialog. */
    if (gDuel_bEffectState || gDuel_wCardEffectFlags || gDuel_bQuitDialogState || !D_8009B214) return;
    /* The plate goes with the FIELD box (Duel_InitScene's sprite), which
     * slides off the left edge for battles, the opponent's turn and the
     * field views; while it is not all on screen the plate is not shown. */
    view.box_x = (s16)D_8009B214->field_30.h.field_30;
    view.box_y = (s16)D_8009B214->field_30.h.field_32;
    if (view.box_x < 0 || view.box_y < 0) return;
    /* A mod's "ui" moving the box (duel_ui.h): the plate goes with it. */
    {
        int dx, dy;
        DuelUi_Offset(UI_FIELD, &dx, &dy);
        view.box_x += dx;
        view.box_y += dy;
    }
    adjustment = D_800E9FF0[0].rank.result_adjustment;
    score = Rank_Score(&D_800E9FF0[0], adjustment ? adjustment : ADJUST_LP_WIN);
    if (score == RANK_SCORE_UNKNOWN) return;
    view.visible = 1;
    view.level = level;
    view.score = score;
    Rank_Grade(score, &view.tec, &view.tier);
}

unsigned RankMeter_Signature(void)
{
    int x, y, w, h;
    update();
    if (!view.visible) return 0;
    FusionHelper_GetViewport(&x, &y, &w, &h);
    return (((((unsigned)view.score * 3u + (unsigned)view.level) * 331u + (unsigned)view.box_x) * 241u +
             (unsigned)view.box_y) * 31u + (unsigned)x * 17u + (unsigned)y) * 31u + (unsigned)w * 7u + (unsigned)h + 1u;
}

/* The FIELD box sprite from its position: its right edge. */
enum { FIELD_BOX_RIGHT = 55 };

/* The result screen's badge and rank letter on its stone plate, right of
 * the FIELD box and as tall as it, with the score in card digits at level
 * 2: the game's own pictures (rank_art.h), laid on the picture in its own
 * pixels, so they grow with the window and keep their place in
 * widescreen. */
void RankMeter_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    RankArtView picture;
    *x = *y = *w = *h = 0;
    update();
    FusionHelper_GetViewport(&view.x, &view.y, &view.w, &view.h);
    if (!view.visible || view.w <= 0 || view.h <= 0) return;
    picture.x = view.x;
    picture.y = view.y;
    picture.w = view.w;
    picture.h = view.h;
    picture.width_2d = Platform_Widescreen() ? 426 : 320;
    RankArt_Draw(canvas, &picture, view.box_x + FIELD_BOX_RIGHT, view.box_y, view.tec, view.tier,
                 view.level == 2 ? (view.score < 0 ? 0 : view.score) : -1, x, y, w, h);
}
