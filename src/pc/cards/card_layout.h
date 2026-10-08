#ifndef MEMORIES_PC_CARD_LAYOUT_H
#define MEMORIES_PC_CARD_LAYOUT_H
/* One place that answers "where does this element of the big-card display
 * go, and should it even be drawn", for the three retail call sites that
 * would otherwise each repeat their own lookup and their own magic numbers
 * -- duel_effect_resource_setup.c's frame list-membership, func_800283F4.c's
 * description box, and func_80028B08.c's title/stats/stars/attribute/art/
 * frame. Re-tuning where an element sits, or adding a layout style, touches
 * card_layout.c only; the call sites stay as they are.
 *
 * Unlike an earlier, settings-based version of this feature, nothing here
 * is a built-in toggle: every answer comes from whichever applied mod
 * declares a "card_layout" key in its manifest (notes/modding.md, "Card
 * layout"), the same way a mod's "title"/"menu" key is read
 * (title_config.h). With no such mod applied, every element answers with
 * retail's own position -- byte-identical behaviour, nothing to configure.
 *
 * x/y are an offset in the same coordinate neighbourhood as the retail code
 * they replace (func_80028B08.c's own +0x13, +0x9D, ... constants): added
 * to whatever base the call site already adds its own retail constant to
 * (win->field_30's own position, or a per-star cursor), never an
 * independent origin. */

typedef enum {
    CARD_LAYOUT_FRAME,
    CARD_LAYOUT_TITLE,
    CARD_LAYOUT_DESCRIPTION,
    CARD_LAYOUT_LEVEL_STARS,
    CARD_LAYOUT_ATTRIBUTE,
    CARD_LAYOUT_ATK,
    CARD_LAYOUT_DEF,
    CARD_LAYOUT_ART,
    CARD_LAYOUT_ELEMENT_COUNT
} CardLayoutElement;

typedef struct {
    int visible; /* 0: the call site leaves this element out entirely */
    int x, y;
    /* w/h: only CARD_LAYOUT_ART, CARD_LAYOUT_ATTRIBUTE and CARD_LAYOUT_FRAME
     * use these -- the first two to stretch a picture to a size independent
     * of its own texel footprint (func_80028B08.c's CardLayout_DrawArt,
     * reused for both), the frame for its own total box
     * (CardLayout_DrawFrame's draw rect). Every other element keeps these
     * 0 -- retail's own sprite resource already fixes its draw size. */
    int w, h;
} CardLayoutPlacement;

CardLayoutPlacement CardLayout_Get(CardLayoutElement element);
/* True when the frame/title plate is hidden: the one thing that tells a
 * call site "there's no plate behind what I'm about to draw anymore"
 * without it having to infer that from an unrelated element. False with no
 * "card_layout" mod applied. */
int CardLayout_FullBleed(void);
/* The frame texture's path on disk (the layout mod's own "frame" image,
 * already joined with its directory and validated the way a "title" mod's
 * image is -- card_layout_art.c just opens it), or "" when no mod applies
 * one. Decode failure there is not an error here: the caller (func_80028B08.c,
 * through CardLayoutArt_FrameTile) simply does not draw a frame. */
const char *CardLayout_FramePath(void);

/* Every distinct frame image the active full-bleed layout can draw (one per
 * card kind, kinds sharing an image counted once), joined with the mod's
 * directory, into `paths`; 0 when no mod's layout is on. For
 * CardLayoutArt_Prewarm. */
int CardLayout_FramePaths(char (*paths)[1024], int max);

/* True when the full-bleed layout has no ritual frame image of its own, or
 * it is the same file as magic's: a ritual spell then wears magic's colour
 * (Cards_FrameColor). A layout with its own ritual frame keeps it. */
int CardLayout_RitualWearsMagic(void);

/* Which card the next CardLayout_Get/CardLayout_FramePath/CardLayout_IsSpell
 * answer for: its frame (monster/magic/trap/ritual/purple/orange, cards.h
 * CARD_FRAME_*) comes from its own Cards_FrameColor if a mod set one, else
 * its Cards_Type (equip takes magic's). Monster (id 0, invalid) until a
 * call site sets a real one -- every retail call site draws a real card,
 * so this only matters for the three that now call it, not for anything
 * that never does. */
void CardLayout_SetCard(int card_id);
/* True when the current card's frame is magic/trap/ritual's: no ATK/DEF/
 * level stars to draw (none of those three cards have them), and
 * CARD_LAYOUT_ART/CARD_LAYOUT_ATTRIBUTE answer from the mod's "spell"
 * object instead of its "art"/"attribute" (monster/purple/orange keep
 * today's keys) -- a different place for the same elements, not different
 * ones; func_80028B08.c still draws retail's own art/attribute textures
 * there unchanged. */
int CardLayout_IsSpell(void);

#endif
