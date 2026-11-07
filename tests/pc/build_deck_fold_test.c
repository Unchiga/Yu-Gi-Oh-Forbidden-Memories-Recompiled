/* The Build Deck's folded chest (src/pc/cards/build_deck_fold.c): a card and its variants as one row, with
 * the variants stepped by the picks the player makes. The cards module and the settings are stubs: card 5 has
 * the variants 20 and 21, card 7 has 22, and card 6 none. */
#include <stdio.h>
#include <string.h>

#include "pc/cards/build_deck_fold.h"
#include "pc/cards/cards.h"
#include "pc/platform/settings.h"
#include "game/card_constants.h"

static int failures;
static int setting_on = 1;
static int variants_exist = 1;

#define CHECK(condition, ...)                                                                   \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                \
            fprintf(stderr, __VA_ARGS__);                                                       \
            fputc('\n', stderr);                                                                \
            failures++;                                                                         \
        }                                                                                       \
    } while (0)

int Cards_HaveVariants(void) { return variants_exist; }
int Cards_VariantOf(int id) { return id == 20 || id == 21 ? 5 : id == 22 ? 7 : 0; }
int Cards_VariantCount(int base) { return base == 5 ? 2 : base == 7 ? 1 : 0; }
int Cards_VariantMember(int base, int index)
{
    if (base == 5) return index == 0 ? 20 : index == 1 ? 21 : 0;
    if (base == 7) return index == 0 ? 22 : 0;
    return 0;
}
int Settings_Get(SettingId id)
{
    return id == SET_BUILD_DECK_FOLD_VARIANTS ? setting_on : 0;
}

int main(void)
{
    static unsigned char chest[CARD_TABLE_ID_END], deck[CARD_TABLE_ID_END];
    int ws_a, ws_b;
    const void *a = &ws_a, *b = &ws_b;

    /* Folded only while the setting is on and a mod added a variant. */
    CHECK(BuildDeckFold_Active(), "active");
    setting_on = 0;
    CHECK(!BuildDeckFold_Active(), "setting off");
    setting_on = 1;
    variants_exist = 0;
    CHECK(!BuildDeckFold_Active(), "no variants");
    variants_exist = 1;

    CHECK(BuildDeckFold_Head(20) == 5 && BuildDeckFold_Head(21) == 5 && BuildDeckFold_Head(5) == 5 &&
          BuildDeckFold_Head(6) == 6, "heads");

    /* A new screen shows the cards themselves. */
    BuildDeckFold_Begin(a);
    CHECK(BuildDeckFold_Shown(a, 5) == 5 && BuildDeckFold_Shown(a, 7) == 7, "head first");

    /* The row: shown for whatever the entry has, in the chest ("on") or only in the deck ("held"). */
    memset(chest, 0, sizeof(chest));
    memset(deck, 0, sizeof(deck));
    CHECK(BuildDeckFold_RowFlag(chest, deck, 5, 1, 0x80) == 0, "nothing: not shown");
    chest[21] = 2;
    CHECK(BuildDeckFold_RowFlag(chest, deck, 5, 1, 0x80) == 1, "a variant in the chest");
    chest[21] = 0;
    deck[20] = 1;
    CHECK(BuildDeckFold_RowFlag(chest, deck, 5, 1, 0x80) == 0x80, "only in the deck: held");
    deck[20] = 0;
    chest[5] = 3;
    chest[21] = 2;
    CHECK(BuildDeckFold_ChestTotal(chest, 5) == 5, "the entry's copies in the chest");
    CHECK(BuildDeckFold_ChestTotal(chest, 6) == 0 && BuildDeckFold_ChestTotal(chest, 7) == 0, "other entries");

    /* Stepping goes through the cards the player has, round again. */
    BuildDeckFold_Begin(a);
    CHECK(BuildDeckFold_Step(a, chest, deck, 5, 1) == 1 && BuildDeckFold_Shown(a, 5) == 21, "R3 skips the card the player lacks");
    CHECK(BuildDeckFold_Step(a, chest, deck, 5, 1) == 1 && BuildDeckFold_Shown(a, 5) == 5, "and wraps to the card itself");
    CHECK(BuildDeckFold_Step(a, chest, deck, 5, -1) == 1 && BuildDeckFold_Shown(a, 5) == 21, "L3 goes back");
    chest[20] = 1;
    CHECK(BuildDeckFold_Step(a, chest, deck, 5, -1) == 1 && BuildDeckFold_Shown(a, 5) == 20, "a variant the player has now");
    /* A card in the deck only still counts. */
    chest[21] = 0;
    deck[21] = 1;
    CHECK(BuildDeckFold_Step(a, chest, deck, 5, 1) == 1 && BuildDeckFold_Shown(a, 5) == 21, "held counts");
    /* Alone: nothing to step to. */
    CHECK(BuildDeckFold_Step(a, chest, deck, 6, 1) == 0, "no variants");
    memset(chest, 0, sizeof(chest));
    memset(deck, 0, sizeof(deck));
    chest[7] = 1;
    BuildDeckFold_Begin(a);
    CHECK(BuildDeckFold_Step(a, chest, deck, 7, 1) == 0 && BuildDeckFold_Shown(a, 7) == 7, "only the card");

    /* Another screen's workspace has its own picks. */
    chest[5] = 1;
    chest[20] = 1;
    BuildDeckFold_Begin(a);
    BuildDeckFold_Begin(b);
    BuildDeckFold_Pick(a, 20);
    CHECK(BuildDeckFold_Shown(a, 5) == 20 && BuildDeckFold_Shown(b, 5) == 5, "workspaces apart");
    BuildDeckFold_Pick(a, 5);
    CHECK(BuildDeckFold_Shown(a, 5) == 5, "back to the card itself");

    /* After a count changed: the card on show is one the player still has. */
    BuildDeckFold_Pick(a, 20);
    chest[20] = 0;
    BuildDeckFold_Settle(a, chest, deck, 5);
    CHECK(BuildDeckFold_Shown(a, 5) == 5, "the picked card is gone: the first the player has");
    memset(chest, 0, sizeof(chest));
    chest[21] = 1;
    BuildDeckFold_Settle(a, chest, deck, 5);
    CHECK(BuildDeckFold_Shown(a, 5) == 21, "only a variant is left");
    chest[21] = 0;
    BuildDeckFold_Settle(a, chest, deck, 5);
    CHECK(BuildDeckFold_Shown(a, 5) == 21, "none left: as it was");

    if (failures) return 1;
    puts("Build Deck fold: the row, the picks and the steps are as expected");
    return 0;
}
