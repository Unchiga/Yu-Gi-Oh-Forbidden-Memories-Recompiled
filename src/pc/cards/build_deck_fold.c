#include "build_deck_fold.h"

#include <string.h>

#include "cards.h"
#include "pc/platform/settings.h"
#include "game/card_constants.h"

#define WORKSPACES 2   /* the screen's two panes' workspaces (a trade has two chests) */

static const void *owners[WORKSPACES];
static unsigned short picked[WORKSPACES][CARD_TABLE_ID_END];   /* [workspace][head]: the card on show, 0: the head */
static int next_slot;

static int slot_of(const void *workspace)
{
    int i;
    for (i = 0; i < WORKSPACES; i++) {
        if (owners[i] == workspace) return i;
    }
    i = next_slot++ % WORKSPACES;
    owners[i] = workspace;
    memset(picked[i], 0, sizeof(picked[i]));
    return i;
}

int BuildDeckFold_Active(void)
{
    return Cards_HaveVariants() && Settings_Get(SET_BUILD_DECK_FOLD_VARIANTS);
}

void BuildDeckFold_Begin(const void *workspace)
{
    int i = slot_of(workspace);
    memset(picked[i], 0, sizeof(picked[i]));
}

int BuildDeckFold_Head(int id)
{
    int base = Cards_VariantOf(id);
    return base ? base : id;
}

int BuildDeckFold_Shown(const void *workspace, int head)
{
    int shown;
    if ((unsigned)head >= CARD_TABLE_ID_END) return head;
    shown = picked[slot_of(workspace)][head];
    return shown ? shown : head;
}

void BuildDeckFold_Pick(const void *workspace, int id)
{
    int head = BuildDeckFold_Head(id);
    if ((unsigned)head >= CARD_TABLE_ID_END) return;
    picked[slot_of(workspace)][head] = (unsigned short)(id == head ? 0 : id);
}

int BuildDeckFold_Present(const unsigned char *chest, const unsigned char *deck, int id)
{
    return chest[id] != 0 || deck[id] != 0;
}

/* The entry's cards, the head first: `index` of them, 0 past the end. */
static int member(int head, int index)
{
    return index == 0 ? head : Cards_VariantMember(head, index - 1);
}

int BuildDeckFold_RowFlag(const unsigned char *chest, const unsigned char *deck, int head, int on, int held)
{
    int i, size = 1 + Cards_VariantCount(head), in_deck = 0;
    for (i = 0; i < size; i++) {
        int id = member(head, i);
        if (chest[id]) return on;
        if (deck[id]) in_deck = 1;
    }
    return in_deck ? held : 0;
}

int BuildDeckFold_ChestTotal(const unsigned char *chest, int head)
{
    int i, size = 1 + Cards_VariantCount(head), total = 0;
    for (i = 0; i < size; i++) total += chest[member(head, i)];
    return total;
}

int BuildDeckFold_Step(const void *workspace, const unsigned char *chest, const unsigned char *deck, int head,
                       int step)
{
    int size = 1 + Cards_VariantCount(head), at, n, shown = BuildDeckFold_Shown(workspace, head);
    if (size < 2 || step == 0) return 0;
    for (at = 0; at < size && member(head, at) != shown; at++) {}
    if (at == size) at = 0;
    for (n = 1; n < size; n++) {
        int next = ((at + (step > 0 ? n : -n)) % size + size) % size;
        int id = member(head, next);
        if (BuildDeckFold_Present(chest, deck, id)) {
            BuildDeckFold_Pick(workspace, id);
            return 1;
        }
    }
    return 0;
}

void BuildDeckFold_Settle(const void *workspace, const unsigned char *chest, const unsigned char *deck, int head)
{
    int i, size = 1 + Cards_VariantCount(head);
    if (BuildDeckFold_Present(chest, deck, BuildDeckFold_Shown(workspace, head))) return;
    for (i = 0; i < size; i++) {
        if (BuildDeckFold_Present(chest, deck, member(head, i))) {
            BuildDeckFold_Pick(workspace, member(head, i));
            return;
        }
    }
}
