#include "card_layout.h"
#include "cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/debug/log.h"
#include "game/card_constants.h"
#include <stdio.h>
#include <string.h>

static const char *const element_names[CARD_LAYOUT_ELEMENT_COUNT] = {
    "frame", "title", "description", "level_stars", "attribute", "atk", "def", "art"
};

/* cards.h's CARD_FRAME_* order, named as "frame"'s own sub-keys. */
static const char *const frame_kind_names[CARD_FRAME_COUNT] = {
    "monster", "magic", "trap", "ritual", "purple", "orange"
};

/* Logged only on change (MEMORIES_TRACE=card_layout), not every frame:
 * func_80028B08.c asks for atk/def/stars/attribute every frame a card
 * viewer is open, but a layout answer only actually changes when the
 * active mod's settings or manifest change. */
static CardLayoutPlacement last[CARD_LAYOUT_ELEMENT_COUNT];
static int last_set[CARD_LAYOUT_ELEMENT_COUNT];

/* The card CardLayout_SetCard last named, as a CARD_FRAME_* bucket
 * (cards.h): Cards_FrameColor's own explicit choice if a mod gave the card
 * one, else its Cards_Type (equip takes magic's); monster with no card
 * named yet, same as every call site that never draws a real card. */
static int current_kind = CARD_FRAME_MONSTER;

typedef struct {
    int mod;                   /* -1: no applied mod declares "card_layout" */
    const JsonValue *layout;   /* that mod's "card_layout" object */
    char frame_path[1024];     /* current_kind's "frame" image, joined with the mod's own directory; "" for none */
} LayoutSource;

/* "frame"'s sub-object for `kind`, falling back to "monster" when `kind`
 * has none of its own -- purple/orange are recolours of the monster frame
 * (hd_assets_pack.py), and a mod that has not drawn magic/trap/ritual's own
 * yet (notes/modding.md) still gets a frame, not none. */
static const JsonValue *frame_for_kind(const JsonValue *layout, int kind)
{
    const JsonValue *frame_set = Json_Member(layout, "frame");
    const JsonValue *frame = Json_Member(frame_set, frame_kind_names[kind]);
    return frame ? frame : Json_Member(frame_set, frame_kind_names[CARD_FRAME_MONSTER]);
}

void CardLayout_SetCard(int card_id)
{
    int color = Cards_FrameColor(card_id);
    int type;
    if (color >= 0) {
        current_kind = color;
        return;
    }
    type = Cards_Type(card_id);
    if (type == CARD_TYPE_MAGIC || type == CARD_TYPE_EQUIP) current_kind = CARD_FRAME_MAGIC;
    else if (type == CARD_TYPE_TRAP) current_kind = CARD_FRAME_TRAP;
    else if (type == CARD_TYPE_RITUAL) current_kind = CARD_FRAME_RITUAL;
    else current_kind = CARD_FRAME_MONSTER;
}

int CardLayout_IsSpell(void)
{
    return current_kind == CARD_FRAME_MAGIC || current_kind == CARD_FRAME_TRAP ||
           current_kind == CARD_FRAME_RITUAL;
}

/* `kind`'s "frame" image joined with the mod's own directory, into `out`;
 * "" for none (or one outside the mod). */
static void frame_path_of(int mod, const JsonValue *layout, int kind, char *out, size_t size)
{
    const JsonValue *frame = frame_for_kind(layout, kind);
    const char *file = Json_String(Json_Member(frame, "image"), NULL);
    out[0] = 0;
    if (file && *file) {
        if (!Paths_Contained(file) || snprintf(out, size, "%s/%s", Mods_Directory(mod), file) >= (int)size) {
            Mods_Note(Mods_Id(mod), "card_layout: \"frame\" \"image\" \"%s\" is outside the mod", file);
            out[0] = 0;
        }
    }
}

/* The last applied mod, in load order, that declares a "card_layout" key --
 * the same "a later mod's value wins" rule every other manifest key
 * follows (notes/modding.md). Re-walked on every call rather than cached:
 * this only runs while a big card is actually being drawn (a card viewer
 * open, a duel's own field or result cards), never the main loop, so
 * walking a handful of loaded mods is nothing worth caching against. */
static LayoutSource find_source(void)
{
    LayoutSource source;
    int i;

    source.mod = -1;
    source.layout = NULL;
    source.frame_path[0] = 0;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const JsonValue *layout;
        if (!Mods_Active(mod)) continue;
        layout = Json_Member(Mods_Manifest(mod), "card_layout");
        if (!layout) continue;
        source.mod = mod;
        source.layout = layout;
    }
    if (source.mod >= 0)
        frame_path_of(source.mod, source.layout, current_kind, source.frame_path,
                      sizeof(source.frame_path));
    return source;
}

static int full_bleed_of(const LayoutSource *source)
{
    /* Off until the player turns it on. A mod's own "settings" entry's
     * "default" is the Mods window's display only -- this fallback is the
     * one that actually answers; keep both in sync if either changes. */
    return source->mod >= 0 && Mods_Setting(Mods_Id(source->mod), "full_bleed", 0) != 0;
}

/* "<key>": {"x": ..., "y": ...}, each defaulting to retail's own place. */
static void point(const JsonValue *layout, const char *key, int *x, int *y, int dx, int dy)
{
    const JsonValue *part = Json_Member(layout, key);
    *x = (int)Json_Number(Json_Member(part, "x"), dx);
    *y = (int)Json_Number(Json_Member(part, "y"), dy);
}

/* "<key>": {"x": ..., "y": ..., "width": ..., "height": ...}, same defaults
 * convention as a "title" mod's image (title_config.h's TitleImage). */
static void rect(const JsonValue *layout, const char *key, int *x, int *y, int *w, int *h,
                 int dx, int dy, int dw, int dh)
{
    const JsonValue *part = Json_Member(layout, key);
    *x = (int)Json_Number(Json_Member(part, "x"), dx);
    *y = (int)Json_Number(Json_Member(part, "y"), dy);
    *w = (int)Json_Number(Json_Member(part, "width"), dw);
    *h = (int)Json_Number(Json_Member(part, "height"), dh);
}

int CardLayout_FullBleed(void)
{
    LayoutSource source = find_source();
    return full_bleed_of(&source);
}

const char *CardLayout_FramePath(void)
{
    static char path[1024];
    LayoutSource source = find_source();
    snprintf(path, sizeof(path), "%s", source.frame_path);
    return path;
}

int CardLayout_FramePaths(char (*paths)[1024], int max)
{
    LayoutSource source = find_source();
    int kind, count = 0;

    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    for (kind = 0; kind < CARD_FRAME_COUNT && count < max; kind++) {
        int i;
        frame_path_of(source.mod, source.layout, kind, paths[count], 1024);
        if (!paths[count][0]) continue;
        for (i = 0; i < count && strcmp(paths[i], paths[count]); i++) {}
        if (i == count) count++;   /* a kind that shares another's image (ritual/magic) adds nothing */
    }
    return count;
}

CardLayoutPlacement CardLayout_Get(CardLayoutElement element)
{
    CardLayoutPlacement p = {1, 0, 0, 0, 0};
    LayoutSource source = find_source();
    int full_bleed = full_bleed_of(&source);
    int spell = CardLayout_IsSpell();

    switch (element) {
    case CARD_LAYOUT_FRAME: {
        /* "frame": {"<kind>": {"image": ..., "width": ..., "height": ...}}
         * -- the whole box CardLayout_DrawFrame (func_80028B08.c) draws its
         * texture into; 140x196 with no mod's own size given is this
         * feature's own first measured value, kept as the sensible
         * default, not a retail equivalent (retail has no unified frame
         * concept to default to). */
        const JsonValue *frame = frame_for_kind(source.layout, current_kind);
        p.visible = !full_bleed;
        p.w = (int)Json_Number(Json_Member(frame, "width"), 140);
        p.h = (int)Json_Number(Json_Member(frame, "height"), 196);
        break;
    }
    case CARD_LAYOUT_TITLE:
        p.visible = !full_bleed;
        break;
    case CARD_LAYOUT_DESCRIPTION:
        /* Always visible, independent of full_bleed: the description
         * TextBox's own backdrop (func_800283F4.c's D_8009B240) is on
         * screen either way, so hiding only the text would leave it
         * looking empty for no reason. */
        p.visible = 1;
        break;
    case CARD_LAYOUT_LEVEL_STARS:
        /* Magic/trap/ritual never reach this call (func_80028B08.c's own
         * obj->field_68 < 0x14 gate, unrelated to full_bleed): none of the
         * three have a level to draw stars for, in retail or full-bleed. */
        if (full_bleed) point(source.layout, "stars", &p.x, &p.y, 0x77, 0x20);
        else { p.x = 0x77; p.y = 0x20; }
        break;
    case CARD_LAYOUT_ATTRIBUTE:
        /* Monster/purple/orange: the mod's "attribute" rect. Magic/trap/
         * ritual: "spell"."icon" instead -- a different place for the same
         * element (func_80028B08.c still draws retail's own elemental
         * texture there, unchanged; those three kinds just have no
         * meaningful one, same as they have no ATK/DEF/level). */
        if (full_bleed) {
            if (spell) rect(Json_Member(source.layout, "spell"), "icon", &p.x, &p.y, &p.w, &p.h, 0x6E, 0xD, 0, 0);
            else rect(source.layout, "attribute", &p.x, &p.y, &p.w, &p.h, 0x6E, 0xD, 0, 0);
        } else { p.x = 0x6E; p.y = 0xD; }
        break;
    case CARD_LAYOUT_ATK:
        /* Spell: never reached (same gate as CARD_LAYOUT_LEVEL_STARS). */
        if (full_bleed) point(source.layout, "atk", &p.x, &p.y, 0x61, 0x9D);
        else { p.x = 0x61; p.y = 0x9D; }
        break;
    case CARD_LAYOUT_DEF:
        if (full_bleed) point(source.layout, "def", &p.x, &p.y, 0x61, 0xAB);
        else { p.x = 0x61; p.y = 0xAB; }
        break;
    case CARD_LAYOUT_ART:
        /* w/h at 0 (retail) unless full-bleed and the mod gives a rect:
         * func_80028B08.c reads w != 0 as "stretch the art", so an empty
         * "art" key here (no mod, or full_bleed off) correctly draws
         * nothing special. Spell reads "spell"."art" instead of "art" --
         * magic/trap/ritual's own art box, not monster's. */
        if (full_bleed) {
            if (spell) rect(Json_Member(source.layout, "spell"), "art", &p.x, &p.y, &p.w, &p.h, 0, 0, 0, 0);
            else rect(source.layout, "art", &p.x, &p.y, &p.w, &p.h, 0, 0, 0, 0);
        }
        break;
    default:
        break;
    }

    if (element >= 0 && element < CARD_LAYOUT_ELEMENT_COUNT &&
        (!last_set[element] || last[element].visible != p.visible ||
         last[element].x != p.x || last[element].y != p.y ||
         last[element].w != p.w || last[element].h != p.h)) {
        LOG(LOG_CARD_LAYOUT, "%s: visible=%d x=%d y=%d w=%d h=%d (full_bleed=%d kind=%s)",
            element_names[element], p.visible, p.x, p.y, p.w, p.h, full_bleed, frame_kind_names[current_kind]);
        last[element] = p;
        last_set[element] = 1;
    }
    return p;
}
