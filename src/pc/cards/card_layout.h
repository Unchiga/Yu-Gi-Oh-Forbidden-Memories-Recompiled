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

#include <stddef.h>

typedef enum {
    CARD_LAYOUT_FRAME,
    CARD_LAYOUT_TITLE,
    CARD_LAYOUT_DESCRIPTION,
    CARD_LAYOUT_LEVEL_STARS,
    CARD_LAYOUT_ATTRIBUTE,
    CARD_LAYOUT_ATK,
    CARD_LAYOUT_DEF,
    CARD_LAYOUT_ART,
    /* After ART: earlier releases' mods know ART as 7. */
    CARD_LAYOUT_ATK_LABEL,
    CARD_LAYOUT_DEF_LABEL,
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
    /* A multiply tint over whatever the element already draws (its own
     * "color": {"r", "g", "b"}, each 0-255), in the GPU's own neutral-at-0x80
     * convention (func_80028B08.c's win->field_0C): 0x80 each with no mod,
     * or a mod's element with no "color" of its own, leaves the retail/style
     * color exactly as it was -- not an independent paint, a filter on it. */
    int r, g, b;
} CardLayoutPart;

CardLayoutPart CardLayout_Part(CardLayoutElement element);
/* CardLayout_Part without the tint: the placement earlier releases' mods
 * know (its size is part of their ABI). */
typedef struct {
    int visible;
    int x, y;
    int w, h;
} CardLayoutPlacement;
/* The small, flat duel-hand card has its own coordinate system (52x60),
 * independent of the 140x196 card viewer.  These entries read a resolved
 * frame style's optional ``hand`` object: ``art``, ``atk``, ``def``,
 * ``atk_label`` and ``dfd_label``. */
typedef enum {
    CARD_LAYOUT_HAND_ART,
    CARD_LAYOUT_HAND_ATK,
    CARD_LAYOUT_HAND_DEF,
    CARD_LAYOUT_HAND_ATK_LABEL,
    CARD_LAYOUT_HAND_DEF_LABEL
} CardLayoutHandElement;

CardLayoutPlacement CardLayout_HandGet(int card_id, CardLayoutHandElement element);
int CardLayout_HandStyled(int card_id);
CardLayoutPlacement CardLayout_Get(CardLayoutElement element);
/* Older layouts put ``stars`` beside the shared card_layout keys and use
 * its x coordinate as the row's centre.  A frame style's own ``stars`` key
 * is newer and uses the renderer's natural rightmost-star anchor, so it can
 * be positioned directly in the editor.  This keeps the original HD anime
 * layout compatible without making new styles card-level dependent. */
int CardLayout_StarsCentered(void);
/* Whether `card_id`'s resolved full-bleed style explicitly has a `title`
 * entry. Unlike CardLayout_Get this is a pure lookup: render replay can ask
 * it after the game has moved on to another card without changing state. */
int CardLayout_TitleStyled(int card_id);
/* True when the current style's frame/title plate is hidden: the one thing that tells a
 * call site "there's no plate behind what I'm about to draw anymore"
 * without it having to infer that from an unrelated element. False with no
 * "card_layout" mod applied. */
int CardLayout_FullBleed(void);
/* Whether an applied layout is currently enabled.  Unlike FramePaths(), this
 * remains true for a palette/icon-only layout with no large frame image. */
int CardLayout_Active(void);
/* The frame texture's path on disk (the layout mod's own "frame" image,
 * already joined with its directory and validated the way a "title" mod's
 * image is -- card_layout_art.c just opens it), or "" when no mod applies
 * one. Decode failure there is not an error here: the caller (func_80028B08.c,
 * through CardLayoutArt_FrameTile) simply does not draw a frame. */
const char *CardLayout_FramePath(void);

/* Every distinct frame image the active layout can draw (one per
 * card kind, kinds sharing an image counted once), joined with the mod's
 * directory, into `paths`; 0 when no mod's layout is on. For
 * CardLayoutArt_Prewarm. */
int CardLayout_FramePaths(char (*paths)[1024], int max);

/* The layout's own ATK/DFD digits (the active `frame_styles` entry's
 * "digits", falling back to card_layout's "digits": {"image", "width",
 * "height", "step"}): `path` is the image (joined with the mod's directory,
 * card_layout_art.h's digit strip), `w`x`h` one digit's draw size in the
 * card's own units, `step` the distance between two digits' left edges.
 * 0, with `path` empty, when the layout gives none (or the mod's full_bleed
 * setting is off): the retail digits are drawn then. It does not depend on
 * the style having a frame image: a style that keeps retail's frame gets its
 * strip too, centred on the box retail's digits fill (func_80028B08.c). */
int CardLayout_Digits(char *path, size_t size, int *w, int *h, int *step);

/* An optional, active-style-only static ATK or DFD label. `dfd` is non-zero
 * for the defense label. The PNG's own texture is drawn at x/y and w/h in
 * card units; 0 means that style does not supply that label. It does not
 * depend on the style having a frame image: with the mod's full_bleed
 * setting on, a style that keeps retail's frame gets its label too, drawn
 * in place of retail's own caption (func_80028B08.c). */
int CardLayout_StatLabel(int dfd, char *path, size_t size, int *x, int *y, int *w, int *h);

/* `kind`'s own "type_label" entry: card_constants.h's CARD_TYPE_MAGIC,
 * CARD_TYPE_TRAP, CARD_TYPE_RITUAL or CARD_TYPE_EQUIP -- the "[Normal Magic
 * Card]"/"[Trap Card]"/"[Ritual Magic Card]"/"[Equip Magic Card]" caption
 * retail draws under the title for every non-monster card, from its own
 * built-in art at a position the layout never used to touch. `*visible`
 * answers whether anything should be drawn there at all ("hide"/"hidden"/
 * "show": false, the same convention every other movable part takes) --
 * filled in regardless of the return value, and true with no "card_layout"
 * mod applied, matching retail always drawing it. 1 on return (`*visible`
 * also true) is a style's own replacement image, its path, x/y and w/h in
 * card units; 0 leaves retail's own built-in artwork in place, still subject
 * to `*visible`. */
int CardLayout_TypeLabel(int kind, char *path, size_t size, int *x, int *y, int *w, int *h, int *visible);

/* A card's frame style: what the layout draws for it, picked once for the
 * card view's big frame and the hand's small one alike, so they cannot
 * disagree. `color` is the style's "hand_color" (cards.h CARD_FRAME_*, the
 * palette row of every small frame); `spell_layout` is whether the card is
 * laid out as a spell (no ATK/DFD/level; its "spell" art and icon), which
 * follows the card's class, never its style. `image` is the picture's path,
 * filled only for the current card (CardLayout_SetCard). */
typedef struct {
    int color;
    int spell_layout;
    int full_bleed;
    int width, height;
    char name[32];
    char image[1024];
    char hand_image[1024];
    char field_image[1024];
    char field_back_image[1024];
    char back_image[1024];
    char library_icon[1024];
} CardLayoutFrameStyle;

/* The style the layout gives `card_id`, into `style` (its `image` empty); 1,
 * or 0 when the anime frame is off or the layout has none to give (retail's
 * frame colors then). A layout says it in "frame_styles" (name -> {"image",
 * "hand_color", "width", "height", "full_bleed"}), `default_styles`,
 * "frame_for" (rules, first that applies wins: {"class" | "tag", "setting",
 * "style"}) and "default_style". A card's own named "frame_style" wins,
 * followed by printed type, kind, a style named for the card's frame color,
 * the frame_for rules and then the global default. A layout
 * with a "frame" per kind and no "frame_styles" is read as before: the kind
 * of the card, and a ritual spell with no picture of its own wears magic's. */
int CardLayout_FrameStyleOf(int card_id, CardLayoutFrameStyle *style);
/* CardLayout_FrameStyleOf's first fields, as earlier releases' mods know
 * the style (its size is part of their ABI). */
typedef struct {
    int color;
    int spell_layout;
    int width, height;
    char name[32];
    char image[1024];
} CardLayoutStyle;
int CardLayout_StyleOf(int card_id, CardLayoutStyle *style);
/* A style's optional "hand_image", joined with the layout mod's directory
 * and reduced to the duel sprite's 52x60 texels. It replaces the duel's
 * flat hand/field-card frame; "" leaves the retail sprite in place. */
const char *CardLayout_HandFramePath(int card_id);
/* A style's optional "field_image", for the 3D duel-field card quad
 * (func_80015EF4's POLY_GT4, upright on the battlefield -- distinct from
 * the flat hand sprite above). There is deliberately no fallback to
 * "hand_image": a hand-only style leaves the 3D field card entirely retail.
 * "" leaves retail's Cards_FrameColor palette-row recolor in place. */
const char *CardLayout_FieldFramePath(int card_id);
/* A style's optional "field_back_image", for the same flat hand/field sprite
 * above while the card is face down (duel_card_frame_draw.c's field_69
 * branch). A real card's own hand is never face down to its owner, so this
 * is reached only by a face-down or mid-flip field card; no fallback to
 * "hand_image"/"field_image" -- an unset "" leaves retail's own generic
 * card-back sprite in place rather than showing the style's front art on
 * what is supposed to be its back. */
const char *CardLayout_FieldBackPath(int card_id);
/* A style's optional "back_image", for the big card-viewer's own rotating
 * "card turning" sprite (Library, Build Deck and the duel effect card
 * viewer all share DuelEffect_UpdateCardViewerState,
 * display_object_render_sprite_sheet.c's DisplayObject_RenderSpriteSheet).
 * No fallback; "" leaves retail's own swirl in place. */
const char *CardLayout_BackImagePath(int card_id);
/* The style's optional 12x14 Library-grid cell image. */
const char *CardLayout_LibraryIconPath(int card_id);

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
