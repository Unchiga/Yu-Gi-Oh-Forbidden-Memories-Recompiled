/* DuelScene_UpdateDrawResolution (src/game/duel_draw_resolution.c, included
 * as it is): a turn that starts with a full hand has nothing to draw, so the
 * draw ends at once and touches no hand slot, hand[] entry or deck card. The
 * count went below zero there and the draw ran past the hand until the deck
 * was out. One empty slot still draws it. Compile as 32-bit for the game's
 * layouts. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include "../../src/game/duel_draw_resolution.c"

u8 D_8009B1EC, D_8009B1ED, D_8009B1D5, D_800907CC[2 * HAND_SIZE];
u8 D_8015C424[0x60000];
/* The deck view's asm label carries no leading underscore on Windows. */
#ifdef _WIN32
__asm__(".globl D_8015C424\n.set D_8015C424, _D_8015C424");
#endif
u16 gDuel_wSceneStateFlags;
u8 gDuel_bWinnerSide;
DuelSideState D_800E9FF0[DUEL_SIDE_COUNT];
DuelSideState *D_8009B1C8;
DuelCardRecord D_801A7AD8[8];
/* The game's D_800EA030 with one slot after it, so a draw past the hand can be
 * seen. The header declares it with HAND_SIZE entries, so the name is bound
 * to this storage instead of being defined. */
DuelHandSlot hand_slots[HAND_SIZE + 1] = {{0}};   /* initialised: no common symbol */
#ifdef _WIN32
__asm__(".globl _D_800EA030\n.set _D_800EA030, _hand_slots");
#else
__asm__(".globl D_800EA030\n.set D_800EA030, hand_slots");
#endif

int Cards_ExodiaPiece(int id) { (void)id; return 0; }

static DisplayObject card_object;
static DuelDeckCardRecord deck_card;
static int allocated, set_up, placed, sounds, last_k, last_deck;

void *DisplayObject_FindAllocatedByTag(s32 value) { (void)value; allocated++; return &card_object; }
u8 *Duel_SetupCardRecord(s32 k, s32 deck)
{
    set_up++;
    last_k = k;
    last_deck = deck;
    return 0;
}
void Duel_UpdateDrawCardSlide(DisplayObject *object) { (void)object; }
void SD_SEPlayFull(u32 id) { (void)id; sounds++; }
DuelCardDisplayObject *func_80018004(DuelCardRecord *card, s32 x, s32 y)
{
    (void)card; (void)x; (void)y;
    placed++;
    return (DuelCardDisplayObject *)&card_object;
}

static void reset(int empty_slots)
{
    memset(&card_object, 0, sizeof(card_object));
    memset(D_800E9FF0, 0x5A, sizeof(D_800E9FF0));
    memset(hand_slots, 0x5A, sizeof(hand_slots));
    D_8009B1C8 = &D_800E9FF0[0];
    D_8009B1C8->deck_draw_cursor = 6;
    D_8009B1D5 = 0;
    D_8009B1EC = (u8)empty_slots;
    D_8009B1ED = 0;
    gDuel_wSceneStateFlags = 0;
    D_800907CC[HAND_SIZE - 1] = 2;
    /* The drawn card's record, which points at its deck entry. */
    deck_card.deck_index = 3;
    ((DuelCardReplayRecordBlock *)(D_8015C424 + DUEL_CARD_STAGING_REPLAY_BASE_OFFSET))->record.data =
        (u8 *)&deck_card;
    allocated = set_up = placed = sounds = 0;
}

int main(void)
{
    DuelSideState side_before;
    DuelHandSlot slots_before[HAND_SIZE + 1];
    int i;

    /* A full hand: it is done at once, with everything left as it was. */
    reset(0);
    memcpy(&side_before, D_800E9FF0, sizeof(side_before));
    memcpy(slots_before, hand_slots, sizeof(slots_before));
    DuelScene_UpdateDrawResolution();
    assert(gDuel_wSceneStateFlags & 0x4000);
    assert(D_8009B1EC == 0);
    assert(D_8009B1C8->deck_draw_cursor == 6);
    assert(!memcmp(&side_before, D_800E9FF0, sizeof(side_before)));
    assert(!memcmp(slots_before, hand_slots, sizeof(slots_before)));
    assert(!set_up && !placed && !sounds);
    
    /* The next frame is the settled branch (the Exodia check runs there). */
    DuelScene_UpdateDrawResolution();
    assert(allocated == 1 && set_up == 0 && D_8009B1C8->deck_draw_cursor == 6);

    /* One empty slot: the last slot draws, the deck card is the next one,
     * and the draw ends. */
    reset(1);
    DuelScene_UpdateDrawResolution();
    assert(set_up == 1 && last_k == 2 && last_deck == 6);
    assert(placed == 1 && sounds == 1);
    assert(D_8009B1EC == 0);
    assert(D_8009B1C8->deck_draw_cursor == 7);
    assert(hand_slots[HAND_SIZE - 1].object == (u8 *)&card_object);
    for (i = 0; i < HAND_SIZE - 1; i++) assert(hand_slots[i].object == slots_before[i].object);
    assert(!memcmp(&hand_slots[HAND_SIZE], &slots_before[HAND_SIZE], sizeof(DuelHandSlot)));
    assert(D_8009B1C8->hand[HAND_SIZE - 1] == 3);
    assert(gDuel_wSceneStateFlags & 0x4000);
    for (i = 0; i < HAND_SIZE - 1; i++) assert(D_8009B1C8->hand[i] == 0x5A);
    return 0;
}
