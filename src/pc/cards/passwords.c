/* View > Card passwords (passwords.h). */
#include "passwords.h"
#include "cards.h"
#include "stars.h"
#include "pc/platform/settings.h"
#include "pc/debug/log.h"
#include "pc/text/glyphs.h"
#include "types.h"
#include "ygo_types.h"
#include "game/card_constants.h"
/* The viewer's description box links as D_8009B250 (duel_card_viewer.h). */
#define DUEL_CARD_VIEWER_ADDRESS_ALIASES
#include "game/duel_card_viewer.h"
#include "game/duel_effect.h"
#include "game/duel_effect_card_viewer_state.h"
#include "game/duel_effect_entry_control.h"
#include "game/duel_card.h"
#include "game/library_runtime.h"
#include "game/main_modes.h"
#include "game/text_box_lifecycle.h"
#include "game/text_box_runtime.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern unsigned Memories_PresentedFrames(void);

/* --- the line --------------------------------------------------------- */

/* The viewer's text box is laid out by string 3 (a monster) or 4 (the
 * rest): the card's kind, then for a monster GUARDIAN STAR 24 pixels down
 * and the two stars 16 and 32 below that, and in both the card's text from
 * 80 down ({f8 00 40}). The password goes on the second star's row, 24
 * above the card's text, flush right: the box is as wide as the panel
 * (0xA8) and the longest star name (Mercury) ends at about 80. In the
 * other layout that row is the bottom of the empty middle panel. */
#define ROW_ABOVE_TEXT 24
#define DIGITS_X 96
#define LAYOUT_MONSTER 3
#define LAYOUT_OTHER 4

u32 Text_LookupString(s32 bank, s32 id);   /* src/game/text_lookup_string.c */

static unsigned char text[256];
static int text_card = -1;
static unsigned text_password;

static int layout_of(int card)
{
    /* As the viewer chose it: a monster a mod gave no star has the other
     * layout, with no GUARDIAN STAR heading (stars.h). */
    return ((gDuel_adwCardStats[card - 1] >> CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK) >= CARD_TYPE_MAGIC ||
                   Stars_NoStarCard(card)
               ? LAYOUT_OTHER
               : LAYOUT_MONSTER;
}

/* The card's layout (a translation's, if it has one) with the password
 * before the card's text. 0 when the card has no password, or the layout
 * has no card text or anything this does not copy (a jump, a choice). */
static int compose(int card)
{
    unsigned password = Cards_Password(card);
    const unsigned char *layout = (const unsigned char *)(uintptr_t)Text_LookupString(0, layout_of(card));
    unsigned char *out = text, *end = text + sizeof(text) - 24;
    int i, placed = 0;
    text_card = -1;
    if (password == CARD_PASSWORD_NONE || !layout) return 0;
    for (; *layout != 0xFF; layout++) {
        if (out >= end) return 0;
        /* A port glyph (glyphs.h: F1-F5 and a low byte, any value) or a
         * command's index is copied with its prefix, so a translation's
         * accented letter is not read as a code or the end of the text. */
        if ((*layout >= 0xF1 && *layout <= 0xF5) || (*layout == 0xF8 && layout[1] != 0x00)) {
            if (layout[1] == 0xFF) break;   /* cut short: end there */
            *out++ = *layout++;
            *out++ = *layout;
            continue;
        }
        if (*layout >= 0xF9 && *layout <= 0xFD) return 0;   /* {if}, {choice}, {call}, {jump} */
        if (!placed && layout[0] == 0xF8 && layout[1] == 0x00 && layout[2] == 0x40) {
            *out++ = 0xF8, *out++ = 0x01, *out++ = (unsigned char)-ROW_ABOVE_TEXT;   /* up to the row */
            *out++ = 0xF8, *out++ = 0x06, *out++ = DIGITS_X & 0xFF, *out++ = DIGITS_X >> 8;
            for (i = 7; i >= 0; i--) {
                int code = Glyphs_Code((uint32_t)('0' + ((password >> (i * 4)) & 0xF)));
                if (code < 0 || code >= 0xF0) return 0;             /* the retail digits are single bytes */
                *out++ = (unsigned char)code;
            }
            *out++ = 0xF8, *out++ = 0x01, *out++ = ROW_ABOVE_TEXT;  /* and back */
            placed = 1;
        }
        *out++ = *layout;
    }
    *out = 0xFF;
    if (!placed) return 0;
    text_card = card;
    text_password = password;
    return 1;
}

const unsigned char *CardPassword_Text(int id)
{
    return id == CARD_PASSWORD_TEXT_ID && text_card >= 0 ? text : NULL;
}

/* --- the box ------------------------------------------------------------ */

/* The viewer's description box, as the game made it (3 or 4) or as this
 * made it again. */
static int description(const DuelEffectChannel *box)
{
    return box && (box->flags_34 & DUEL_EFFECT_CHANNEL_FLAG_ACTIVE) &&
           (box->field_36 == LAYOUT_MONSTER || box->field_36 == LAYOUT_OTHER || box->field_36 == CARD_PASSWORD_TEXT_ID);
}

/* The box made again in place, on its channel, where it is and as it was
 * set up, from string `id`, built at once as the viewer builds it. Its
 * fields read the card from gDuel_wSelectedCardID. Returns 0 if the text
 * filled the channel's slice of the entries (DuelEffect_AppendEntry leaves
 * the rest out), which would cut the end of the card's text. */
static int remake(DuelEffectChannel *box, int id, int card)
{
    s16 x = box->field_3C, y = box->field_40, w = box->field_3E, h = box->field_42, selected = gDuel_wSelectedCardID;
    u8 delay = box->field_53, step = box->field_54, depth = box->field_59, index = box->index_57;
    DuelEffectChannel *made;
    int used;
    TextBox_Destroy(box);
    gDuel_wSelectedCardID = (s16)card;
    made = TextBox_Create(index, id, x, y, w, h);
    made->field_53 = delay;
    made->field_54 = step;
    made->field_59 = depth;
    func_80039A14(made);
    gDuel_wSelectedCardID = selected;
    used = (int)(made->entry_end_20 - &D_800EB288[made->range_start_5C]);
    LOG(LOG_DUEL_EFFECTS, "card password: card %d's box made from string %04x on channel %d (%d of %d entries) at frame %u",
        card, id, index, used, made->range_count_5E, Memories_PresentedFrames());
    return used < made->range_count_5E - 1;
}

/* Once a frame on either screen, for the description box of the card on
 * view: with the password while `show`, else as the game has it. Nothing
 * is kept here that a loaded state could contradict: which it is, is the
 * box's string id, and the retail layout comes from the card. */
static void sync(DuelEffectChannel *box, int card, int show)
{
    static int unfit = -1;   /* the card whose text and password did not fit together */
    if (!description(box) || !Cards_Valid(card)) return;
    if (box->field_36 == CARD_PASSWORD_TEXT_ID && show && card == text_card &&
        Cards_Password(card) == text_password) return;
    /* The game's box once all its text is in and still: the Library's
     * types it in, each letter settling over a few frames. */
    if (show &&
        (box->field_36 == CARD_PASSWORD_TEXT_ID ||
         ((box->flags_34 & TEXT_BOX_FLAG_DONE) && !DuelEffect_HasActiveEntry(box))) &&
        card != unfit && compose(card)) {
        if (remake(box, CARD_PASSWORD_TEXT_ID, card)) return;
        unfit = card;
        remake(box, layout_of(card), card);
    } else if (box->field_36 == CARD_PASSWORD_TEXT_ID) {
        remake(box, layout_of(card), card);
    }
}

void CardPassword_UpdateViewer(void)
{
    u8 flags;
    DuelEffect_UpdateCardViewerState();
    flags = gDuel_bEffectHandlerFlags;
    /* Past the opening (the description box is there, D_8009B250), with
     * 0x20 once the face has turned up; 0x40 the slides, 0x10 the closing
     * (DuelEffect_UpdateCardViewerState). */
    if (D_8009B250 && (Settings_Get(SET_CARD_PASSWORDS) || D_8009B250->field_36 == CARD_PASSWORD_TEXT_ID)) {
        sync(D_8009B250, (s16)gDuel_wViewerCardID,
             Settings_Get(SET_CARD_PASSWORDS) && (flags & 0x20) && !(flags & 0x50));
    }
}

void CardPassword_RunLibraryMenu(void)
{
    const u8 *state = D_800EA1E8;
    DuelEffectChannel *box = &D_800EB0F8[0];
    Main_RunLibraryMenu();
    /* The card page (func_8002ACA4), with its description box on channel
     * 0, at rest at step 5: after the card has turned and the grid has
     * gone, until Circle (6, the way back) or the 3D model (4). */
    if ((state[0] & 0xF) == 2 && (Settings_Get(SET_CARD_PASSWORDS) || box->field_36 == CARD_PASSWORD_TEXT_ID)) {
        sync(box, *(const u16 *)(state + 6), Settings_Get(SET_CARD_PASSWORDS) && (state[1] & 0x1F) == 5);
    }
}
