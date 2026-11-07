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
    "frame", "title", "description", "level_stars", "attribute", "atk", "def", "art", "atk_label", "dfd_label"
};

/* The viewer reserves exactly this retail card-plate footprint.  A custom
 * frame may be smaller, but never extend into its Library/Deck Builder UI. */
#define CARD_LAYOUT_FRAME_MAX_W 140
#define CARD_LAYOUT_FRAME_MAX_H 196

/* Hand overlays live on a 52x60 sprite.  Keep user values in a modest,
 * deliberately generous range so their additions to a screen position and
 * five-digit rows cannot wrap the PS1 short geometry fields. */
#define CARD_LAYOUT_HAND_COORD_MIN (-128)
#define CARD_LAYOUT_HAND_COORD_MAX 128
#define CARD_LAYOUT_HAND_MAX_W 128
#define CARD_LAYOUT_HAND_MAX_H 128

/* cards.h's CARD_FRAME_* order, named as "frame"'s own sub-keys. */
static const char *const frame_kind_names[CARD_FRAME_COUNT] = {
    "monster", "magic", "trap", "ritual", "purple", "orange"
};

/* Logged only on change (MEMORIES_TRACE=card_layout), not every frame:
 * func_80028B08.c asks for atk/def/stars/attribute every frame a card
 * viewer is open, but a layout answer only actually changes when the
 * active mod's settings or manifest change. */
static CardLayoutPart last[CARD_LAYOUT_ELEMENT_COUNT];
static int last_set[CARD_LAYOUT_ELEMENT_COUNT];

/* The card CardLayout_SetCard last named: its resolved frame style (or, for a
 * layout with no styles, the legacy "frame" of its kind) -- its color, picture path and
 * size, and whether it is laid out as a spell. A monster with no frame until
 * a call site sets a real card, as every call site that never draws one. */
static CardLayoutFrameStyle current = {.mod = -1};

typedef struct {
    int mod;                   /* -1: no applied mod declares "card_layout" */
    const JsonValue *layout;   /* that mod's "card_layout" object */
} LayoutSource;

/* "frame"'s sub-object for `kind`, falling back to "monster" when `kind`
 * has none of its own -- the OLD layout form, before "frame_styles": purple/
 * orange are recolors of the monster frame (hd_assets_pack.py), and a mod
 * that has not drawn magic/trap/ritual's own yet still gets a frame, not
 * none. */
static const JsonValue *frame_for_kind(const JsonValue *layout, int kind)
{
    const JsonValue *frame_set = Json_Member(layout, "frame");
    const JsonValue *frame = Json_Member(frame_set, frame_kind_names[kind]);
    return frame ? frame : Json_Member(frame_set, frame_kind_names[CARD_FRAME_MONSTER]);
}

/* The six frame colors a style's "hand_color" names. The disc's own labels
 * (monster, magic, trap, ritual) are accepted too. */
static const char *const color_names[CARD_FRAME_COUNT] = {"gold", "green", "pink", "blue", "purple", "orange"};
static int color_named(const char *text)
{
    int i;
    if (!text) return -1;
    for (i = 0; i < CARD_FRAME_COUNT; i++) {
        if (!strcmp(text, color_names[i]) || !strcmp(text, frame_kind_names[i])) return i;
    }
    return -1;
}

int CardLayout_IsSpell(void)
{
    return current.spell_layout;
}

/* A mod's own file `file` joined with its directory, into `out`; "" for
 * none (or one outside the mod). `what` names the key, for the note. */
static void mod_file(int mod, const char *file, const char *what, char *out, size_t size)
{
    out[0] = 0;
    if (file && *file) {
        if (!Paths_Contained(file) || snprintf(out, size, "%s/%s", Mods_Directory(mod), file) >= (int)size) {
            Mods_Note(Mods_Id(mod), "card_layout: %s \"image\" \"%s\" is outside the mod", what, file);
            out[0] = 0;
        }
    }
}

/* `kind`'s "frame" image joined with the mod's own directory, into `out`;
 * "" for none (or one outside the mod). */
static void frame_path_of(int mod, const JsonValue *layout, int kind, char *out, size_t size)
{
    const JsonValue *frame = frame_for_kind(layout, kind);
    mod_file(mod, Json_String(Json_Member(frame, "image"), NULL), "\"frame\"", out, size);
}

/* Whether a "card_layout" holds nothing but "frame_styles": a mod that only
 * adds styles (a cards mod's own frames) and does not set the layout's shared
 * keys (frame_for, default_style(s), the parts, the digits). Such a layout
 * never takes the shared keys away from the one before it. */
static int styles_only(const JsonValue *layout)
{
    const JsonValue *member = Json_At(layout, 0);
    return member && !Json_Next(member) && !strcmp(Json_Name(member), "frame_styles");
}

/* The last applied mod, in load order, that declares a "card_layout" key
 * with more than "frame_styles" in it -- the same "a later mod's value wins"
 * rule every other manifest key follows (notes/modding.md): the layout whose
 * shared keys the cards use. Frame styles come from every applied layout
 * (find_style). Re-walked on every call rather than cached: this only runs
 * while a big card is actually being drawn (a card viewer open, a duel's own
 * field or result cards), never the main loop, so walking a handful of loaded
 * mods is nothing worth caching against. */
static LayoutSource find_source(void)
{
    LayoutSource source;
    int i;

    source.mod = -1;
    source.layout = NULL;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const JsonValue *layout;
        if (!Mods_Active(mod)) continue;
        layout = Json_Member(Mods_Manifest(mod), "card_layout");
        if (!layout || styles_only(layout)) continue;
        source.mod = mod;
        source.layout = layout;
    }
    return source;
}

/* A mod's setting `key`: what the player set, else the "default" of the
 * mod's "settings" entry that names it (what the Mods window shows), else 0.
 * Reading the default here keeps the window and the game of one mind: a
 * mod whose frames are on by default is not drawn without them until the
 * player has touched the switch. */
static int setting_declared(int mod, const char *key)
{
    const JsonValue *spec;

    for (spec = Json_At(Json_Member(Mods_Manifest(mod), "settings"), 0); spec; spec = Json_Next(spec)) {
        const char *name = Json_String(Json_Member(spec, "key"), NULL);
        if (name && !strcmp(name, key)) return 1;
    }
    return 0;
}

static int mod_setting(int mod, const char *key)
{
    const JsonValue *spec;
    int fallback = 0;

    for (spec = Json_At(Json_Member(Mods_Manifest(mod), "settings"), 0); spec; spec = Json_Next(spec)) {
        const char *name = Json_String(Json_Member(spec, "key"), NULL);
        if (name && !strcmp(name, key)) {
            fallback = (int)Json_Number(Json_Member(spec, "default"), 0);
            break;
        }
    }
    return Mods_Setting(Mods_Id(mod), key, fallback);
}

static int mod_full_bleed(int mod);

static int full_bleed_of(const LayoutSource *source)
{
    /* Off until the player turns it on, or the mod's own "full_bleed"
     * setting says it is on by default (a mod of styles alone: always on). */
    return source->mod >= 0 && mod_full_bleed(source->mod);
}

/* A mod's own "card_layout" object, NULL for none (or for no mod). */
static const JsonValue *layout_of(int mod)
{
    return mod >= 0 ? Json_Member(Mods_Manifest(mod), "card_layout") : NULL;
}

/* A mod's own "full_bleed" setting: the switch of its frame styles, each
 * layout mod having one of its own. A mod that only adds styles (a mod of
 * alternate frames: they are drawn for the cards that name them, no other) and
 * declares no such setting has them always on; declaring one gives the
 * player the switch. */
static int mod_full_bleed(int mod)
{
    if (mod < 0) return 0;
    if (!setting_declared(mod, "full_bleed") && styles_only(layout_of(mod)))
        return Mods_Setting(Mods_Id(mod), "full_bleed", 1) != 0;      /* on, unless a value was stored */
    return mod_setting(mod, "full_bleed") != 0;
}

/* Whether any applied layout mod has its frames on. */
static int any_layout_on(void)
{
    int i;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod) && layout_of(mod) && mod_full_bleed(mod)) return 1;
    }
    return 0;
}

/* The layout the current card's parts come from: the one that gave its style
 * (a style's own parts, then that layout's shared ones), else the shared
 * layout. */
static LayoutSource current_source(void)
{
    LayoutSource source;
    if (current.name[0] && current.mod >= 0) {
        source.mod = current.mod;
        source.layout = layout_of(current.mod);
        if (source.layout) return source;
    }
    return find_source();
}

/* The "frame_styles" entry `style` was resolved from. */
static const JsonValue *style_node_of(const CardLayoutFrameStyle *style)
{
    const JsonValue *styles = Json_Member(layout_of(style->mod), "frame_styles");
    return styles && style->name[0] ? Json_Member(styles, style->name) : NULL;
}

/* `part` ("x": ..., "y": ...), each defaulting to retail's own place. */
static void point(const JsonValue *part, int *x, int *y, int dx, int dy)
{
    *x = (int)Json_Number(Json_Member(part, "x"), dx);
    *y = (int)Json_Number(Json_Member(part, "y"), dy);
}

/* `part` ("x": ..., "y": ..., "width": ..., "height": ...), same defaults
 * convention as a "title" mod's image (title_config.h's TitleImage). */
static void rect(const JsonValue *part, int *x, int *y, int *w, int *h, int dx, int dy, int dw, int dh)
{
    *x = (int)Json_Number(Json_Member(part, "x"), dx);
    *y = (int)Json_Number(Json_Member(part, "y"), dy);
    *w = (int)Json_Number(Json_Member(part, "width"), dw);
    *h = (int)Json_Number(Json_Member(part, "height"), dh);
}

static int bounded_coordinate(const JsonValue *part, const char *key, int fallback, int minimum, int maximum)
{
    long value = Json_Number(Json_Member(part, key), fallback);

    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return (int)value;
}

static int frame_box_dimension(const JsonValue *frame, const char *key, int fallback, int maximum)
{
    /* Json_Number is a checked signed-whole-number parser, so this stays a
     * long until after both bounds checks (no floating NaN conversion path). */
    long value = Json_Number(Json_Member(frame, key), fallback);

    if (value < 1) return fallback;
    if (value > maximum) return maximum;
    return (int)value;
}

/* `part` ("color": {"r": ..., "g": ..., "b": ...}), each 0x80 (no change)
 * absent -- CardLayoutPart's own tint convention. */
static void tint(const JsonValue *part, int *r, int *g, int *b)
{
    const JsonValue *color = Json_Member(part, "color");
    *r = (int)Json_Number(Json_Member(color, "r"), 0x80);
    *g = (int)Json_Number(Json_Member(color, "g"), 0x80);
    *b = (int)Json_Number(Json_Member(color, "b"), 0x80);
}

/* Any movable card-view part can opt out without changing its coordinates:
 * `hide` is the documented spelling, while `hidden` and `show: false` make
 * manifests written with the other UI configuration conventions work too. */
static int part_visible(const JsonValue *part, int fallback)
{
    if (Json_Bool(Json_Member(part, "hide"), 0) || Json_Bool(Json_Member(part, "hidden"), 0)) return 0;
    if (Json_Member(part, "show") && !Json_Bool(Json_Member(part, "show"), 1)) return 0;
    return fallback;
}

/* A style's own "<key>" sub-object, when the currently active style has
 * one, else the layout's shared one -- so a mod with one custom frame can
 * keep stating atk/def/stars/attribute/art at the top level as before, but
 * a mod with several (each its own window) can give each style its own
 * instead of them all fighting over one shared box. A style's "<key>" is
 * taken whole, not merged field by field with the layout's: a style that
 * gives "atk" at all owns the point entirely, missing fields falling back
 * to the hardcoded retail default, never to the layout's own "atk". */
static const JsonValue *layout_part(const JsonValue *style_node, const JsonValue *layout, const char *key)
{
    const JsonValue *part = style_node ? Json_Member(style_node, key) : NULL;
    return part ? part : Json_Member(layout, key);
}

/* A digit strip owns the positions of the numeric values it draws. The
 * earlier style/layout-level `atk` and `def` entries remain a fallback for
 * existing manifests. */
static const JsonValue *digit_stat_part(const JsonValue *style_node, const JsonValue *layout, const char *key)
{
    const JsonValue *digits = layout_part(style_node, layout, "digits");
    const JsonValue *part = Json_Member(digits, key);
    return part ? part : layout_part(style_node, layout, key);
}

int CardLayout_Digits(char *path, size_t size, int *w, int *h, int *step)
{
    LayoutSource source = current_source();
    const JsonValue *styles;
    const JsonValue *style_node;
    const JsonValue *digits;

    path[0] = 0;
    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    /* Match the other layout elements: an active frame style owns its
     * digits image, with the old card_layout-level object retained as the
     * fallback for existing mods. */
    styles = Json_Member(source.layout, "frame_styles");
    style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    digits = layout_part(style_node, source.layout, "digits");
    mod_file(source.mod, Json_String(Json_Member(digits, "image"), NULL), "\"digits\"", path, size);
    if (!path[0]) return 0;
    *w = (int)Json_Number(Json_Member(digits, "width"), 10);
    *h = (int)Json_Number(Json_Member(digits, "height"), 12);
    *step = (int)Json_Number(Json_Member(digits, "step"), *w);
    return *w > 0 && *h > 0 && *step > 0;
}

/* Whether the active style keeps the game's own frame ("game_frame") and does not give the part `key`: the name, the
 * words and the numbers are then the game's own, where it puts them, though the style is full bleed. */
static int game_look(const char *key)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style_node;

    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    if (!Json_Bool(Json_Member(style_node, "game_frame"), 0)) return 0;
    return !strcmp(key, "digits") ? 1 : !Json_Member(style_node, key);
}

int CardLayout_GameTitle(void)
{
    return game_look("title");
}

int CardLayout_GameLabel(int dfd)
{
    return game_look(dfd ? "dfd_label" : "atk_label");
}

int CardLayout_PartsOverArt(void)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style_node;

    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    return Json_Bool(Json_Member(style_node, "parts_over_art"), 0);
}

int CardLayout_StarImage(char *path, size_t size, int *w, int *h, int *step)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style_node, *stars;

    path[0] = 0;
    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    stars = layout_part(style_node, source.layout, "stars");
    mod_file(source.mod, Json_String(Json_Member(stars, "image"), NULL), "\"stars\"", path, size);
    if (!path[0]) return 0;
    *w = frame_box_dimension(stars, "width", 9, 32);
    *h = frame_box_dimension(stars, "height", 9, 32);
    *step = frame_box_dimension(stars, "step", *w, 64);
    return 1;
}

int CardLayout_StatLabel(int dfd, char *path, size_t size, int *x, int *y, int *w, int *h)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style, *label;

    path[0] = 0;
    if (source.mod < 0 || !full_bleed_of(&source)) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    /* Labels are deliberately style-owned: unlike a digit strip, a label
     * belongs to a particular frame's stat panel and must not leak to a
     * different style that has differently shaped boxes. */
    label = Json_Member(style, dfd ? "dfd_label" : "atk_label");
    mod_file(source.mod, Json_String(Json_Member(label, "image"), NULL),
             dfd ? "\"dfd_label\"" : "\"atk_label\"", path, size);
    if (!path[0]) return 0;
    *x = (int)Json_Number(Json_Member(label, "x"), 0);
    *y = (int)Json_Number(Json_Member(label, "y"), 0);
    *w = frame_box_dimension(label, "width", 16, CARD_LAYOUT_FRAME_MAX_W);
    *h = frame_box_dimension(label, "height", 8, CARD_LAYOUT_FRAME_MAX_H);
    return *w > 0 && *h > 0;
}

/* card_constants.h's CARD_TYPE_MAGIC..CARD_TYPE_EQUIP are consecutive: the
 * "[Normal Magic Card]"/"[Trap Card]"/"[Ritual Magic Card]"/"[Equip Magic
 * Card]" caption's own key, in that order. */
static const char *const type_label_names[CARD_NON_MONSTER_TYPE_COUNT] = {"magic", "trap", "ritual", "equip"};

int CardLayout_TypeLabel(int kind, char *path, size_t size, int *x, int *y, int *w, int *h, int *visible)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style, *type_labels, *label;
    int idx = kind - CARD_TYPE_MAGIC;

    path[0] = 0;
    *visible = 1;
    if (idx < 0 || idx >= CARD_NON_MONSTER_TYPE_COUNT) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    /* One shared "type_label" object, a kind's own name its sub-key --
     * style-owned first, the layout's shared one its fallback, same rule as
     * every other per-style part (layout_part). Visibility reads this way
     * even with full-bleed off: a position-only layout (no replacement
     * image anywhere) can still hide retail's own built-in caption. */
    type_labels = layout_part(style, source.layout, "type_label");
    label = Json_Member(type_labels, type_label_names[idx]);
    *visible = part_visible(label, 1);
    /* Hidden means hidden: a style that supplies both "hide" and an "image"
     * (switching one back off without deleting the other) must not still
     * get a replacement drawn out from under it -- stop here, the same as
     * "hide" with no image at all does, rather than resolving the image and
     * leaving the caller to notice *visible is false after already having
     * what it needs to draw it. */
    if (!*visible || source.mod < 0 || !full_bleed_of(&source)) return 0;
    mod_file(source.mod, Json_String(Json_Member(label, "image"), NULL), "\"type_label\"", path, size);
    if (!path[0]) return 0;
    *x = (int)Json_Number(Json_Member(label, "x"), 0);
    *y = (int)Json_Number(Json_Member(label, "y"), 0);
    *w = frame_box_dimension(label, "width", 56, CARD_LAYOUT_FRAME_MAX_W);
    *h = frame_box_dimension(label, "height", 16, CARD_LAYOUT_FRAME_MAX_H);
    return *w > 0 && *h > 0;
}

/* Why a layout's frame choice could not be made, said once a layout. */
static const JsonValue *noted_layout;
static char noted_what[96];
static void note_once(const LayoutSource *source, const char *what, const char *name)
{
    char key[96];
    snprintf(key, sizeof(key), "%s %s", what, name);
    if (noted_layout == source->layout && !strcmp(noted_what, key)) return;
    noted_layout = source->layout;
    snprintf(noted_what, sizeof(noted_what), "%s", key);
    Mods_Note(Mods_Id(source->mod), "card_layout: %s \"%s\"", what, name);
}

/* Whether a "frame_for" rule applies to `card`: every condition it names
 * ("class", "tag") holds, and its "setting" (a bool the mod declares), if
 * any, is on. A rule that names none applies to every card. Released layouts
 * (v0.3.0's Forbidden Memories HD pack, tools/pc/anime_frame_layout.py) pick
 * their per-class styles with these rules, so they stay supported. */
static int rule_applies(const LayoutSource *source, const JsonValue *rule, int card, int cls)
{
    const char *key = Json_String(Json_Member(rule, "setting"), NULL);
    const char *text;
    if (key && !mod_setting(source->mod, key)) return 0;
    text = Json_String(Json_Member(rule, "class"), NULL);
    if (text && strcmp(text, Cards_ClassName(cls))) return 0;
    text = Json_String(Json_Member(rule, "tag"), NULL);
    if (text && !Cards_HasTag(card, text)) return 0;
    return 1;
}

/* `default_styles.kinds` is deliberately friendlier than the internal class
 * names: "monster" includes ordinary and effect monsters; "magic" and
 * "ritual" are accepted alongside their precise class spellings.  A precise
 * effect_monster/ritual_spell entry wins over the broader alias. */
static const char *default_style_for_kind(const JsonValue *layout, int cls)
{
    const JsonValue *kinds = Json_Member(Json_Member(layout, "default_styles"), "kinds");
    const char *name;

    if (!kinds) return NULL;
    if (cls == CARD_CLASS_EFFECT_MONSTER &&
        (name = Json_String(Json_Member(kinds, "effect_monster"), NULL))) return name;
    if ((cls == CARD_CLASS_MONSTER || cls == CARD_CLASS_EFFECT_MONSTER) &&
        (name = Json_String(Json_Member(kinds, "monster"), NULL))) return name;
    if (cls == CARD_CLASS_SPELL) {
        if ((name = Json_String(Json_Member(kinds, "magic"), NULL))) return name;
        if ((name = Json_String(Json_Member(kinds, "spell"), NULL))) return name;
    }
    if (cls == CARD_CLASS_EQUIP && (name = Json_String(Json_Member(kinds, "equip"), NULL))) return name;
    if (cls == CARD_CLASS_RITUAL_SPELL) {
        if ((name = Json_String(Json_Member(kinds, "ritual"), NULL))) return name;
        if ((name = Json_String(Json_Member(kinds, "ritual_spell"), NULL))) return name;
    }
    if (cls == CARD_CLASS_TRAP && (name = Json_String(Json_Member(kinds, "trap"), NULL))) return name;
    return NULL;
}

/* Type keys use the normal card-type spellings: Dragon, Zombie, Magic,
 * Trap, Ritual, Equip, etc.  Cards_TypeNamed already accepts case, spaces
 * and punctuation variations, so a manifest author need not duplicate the
 * engine's type-number table. */
static const char *default_style_for_type(const JsonValue *layout, int card)
{
    const JsonValue *types = Json_Member(Json_Member(layout, "default_styles"), "types");
    const JsonValue *entry;
    int type = Cards_Type(card);

    for (entry = Json_At(types, 0); entry; entry = Json_Next(entry)) {
        const char *name = Json_String(entry, NULL);
        if (name && Cards_TypeNamed(Json_Name(entry)) == type) return name;
    }
    return NULL;
}

/* The mod that declared `card` (the one its identity names), -1 for a retail
 * card or none. */
static int declaring_mod(int card)
{
    const char *identity = Cards_Identity(card);
    const char *colon = identity ? strchr(identity, ':') : NULL;
    size_t length;
    int i;

    if (!colon) return -1;
    length = (size_t)(colon - identity);
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const char *id = Mods_Id(mod);
        if (strlen(id) == length && !strncmp(id, identity, length)) return mod;
    }
    return -1;
}

/* Finds the frame style `name` among the applied layouts: in the mod `prefer`
 * names first (-1: none), then in the shared layout `shared` (NULL: none),
 * then in the newest mod that declares it. Into `owner` (its mod and layout)
 * and `node`; 0 when no layout has it. A name two mods declare, found by none
 * of the first two, is noted once: the newer mod's is used. */
static int find_style(const char *name, int prefer, const LayoutSource *shared, LayoutSource *owner,
                      const JsonValue **node)
{
    static char noted[96];
    int i, found = -1;

    if (prefer >= 0 && Mods_Active(prefer)) {
        const JsonValue *layout = layout_of(prefer);
        const JsonValue *hit = Json_Member(Json_Member(layout, "frame_styles"), name);
        if (hit) {
            owner->mod = prefer;
            owner->layout = layout;
            *node = hit;
            return 1;
        }
    }
    if (shared && shared->mod >= 0) {
        const JsonValue *hit = Json_Member(Json_Member(shared->layout, "frame_styles"), name);
        if (hit) {
            *owner = *shared;
            *node = hit;
            return 1;
        }
    }
    for (i = Mods_LoadedCount() - 1; i >= 0; i--) {
        int mod = Mods_Loaded(i);
        const JsonValue *layout, *hit;
        if (!Mods_Active(mod)) continue;
        layout = layout_of(mod);
        hit = Json_Member(Json_Member(layout, "frame_styles"), name);
        if (!hit) continue;
        if (found < 0) {
            found = mod;
            owner->mod = mod;
            owner->layout = layout;
            *node = hit;
        } else {
            char key[96];
            snprintf(key, sizeof(key), "%s|%s|%s", name, Mods_Id(found), Mods_Id(mod));
            if (strcmp(noted, key)) {
                snprintf(noted, sizeof(noted), "%s", key);
                Mods_Note(Mods_Id(found), "card_layout: frame style \"%s\" is also declared by \"%s\"; this mod's is used",
                          name, Mods_Id(mod));
            }
            break;
        }
    }
    return found >= 0;
}

/* The newest applied mod with a "card_layout" (the one a missing style is
 * noted against), -1 for none. */
static int newest_layout_mod(void)
{
    int i;
    for (i = Mods_LoadedCount() - 1; i >= 0; i--) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod) && layout_of(mod)) return mod;
    }
    return -1;
}

/* The frame style of `card`, into `out` (with its picture's path when
 * `want_image`); 0, nothing, when no layout is on or gives one, or the style
 * is unusable (the card is then drawn as retail does).
 *
 * A card's own named style comes first: from the mod that declared the card,
 * else the newest mod that has it, and only while that mod's "full_bleed" is
 * on (off, the card is laid out as any other). Otherwise the shared layout
 * (find_source) picks by the card's type, kind, color and its rules, while its
 * own "full_bleed" is on. Each style is drawn by its owner's own parts and
 * pictures, whichever mod the card's style came from. */
static int resolve_style(int card, CardLayoutFrameStyle *out, int want_image)
{
    LayoutSource shared = find_source(), owner;
    int cls = Cards_Class(card);
    const JsonValue *styles = shared.mod >= 0 ? Json_Member(shared.layout, "frame_styles") : NULL;
    const JsonValue *style = NULL, *hand;
    const char *name = NULL, *image;
    const JsonValue *rule;
    int override = Cards_FrameOverride(card);
    const char *card_style = Cards_FrameStyle(card);

    memset(out, 0, sizeof(*out));
    out->mod = -1;
    owner.mod = -1;
    owner.layout = NULL;
    /* The retail viewer's card plate occupies at most 140x196 units. */
    out->width = CARD_LAYOUT_FRAME_MAX_W;
    out->height = CARD_LAYOUT_FRAME_MAX_H;
    out->spell_layout = cls == CARD_CLASS_SPELL || cls == CARD_CLASS_EQUIP || cls == CARD_CLASS_RITUAL_SPELL ||
                        cls == CARD_CLASS_TRAP;
    if (card_style && *card_style) {
        if (!any_layout_on()) return 0;
        if (find_style(card_style, declaring_mod(card), NULL, &owner, &style)) {
            name = card_style;
            if (!mod_full_bleed(owner.mod)) style = NULL;   /* its mod is off: the card is as any other */
        } else {
            int mod = newest_layout_mod();
            if (mod >= 0) {
                LayoutSource noted;
                noted.mod = mod;
                noted.layout = layout_of(mod);
                note_once(&noted, "no \"frame_styles\" entry named", card_style);
            }
            return 0;
        }
    }
    if (!style) {
        int shared_on = shared.mod >= 0 && full_bleed_of(&shared);
        const LayoutSource *first = shared_on ? &shared : NULL;
        LayoutSource probe;
        const JsonValue *probed;

        name = NULL;
        if (shared_on && !styles) {
            /* A layout from before "frame_styles": "frame" holds a picture a kind
             * (monster, magic, trap, ritual, purple, orange). The kind is the
             * card's own frame color, else its class's; a ritual spell with no
             * ritual picture of its own (or magic's) wears magic's. */
            const JsonValue *frame_set = Json_Member(shared.layout, "frame");
            const JsonValue *frame;
            const char *own, *magic;
            int kind = override;
            if (kind < 0) {
                if (cls == CARD_CLASS_EFFECT_MONSTER) kind = CARD_FRAME_ORANGE;
                else if (cls == CARD_CLASS_SPELL || cls == CARD_CLASS_EQUIP) kind = CARD_FRAME_MAGIC;
                else if (cls == CARD_CLASS_TRAP) kind = CARD_FRAME_TRAP;
                else if (cls == CARD_CLASS_RITUAL_SPELL) kind = CARD_FRAME_RITUAL;
                else kind = CARD_FRAME_MONSTER;
            }
            own = Json_String(Json_Member(Json_Member(frame_set, "ritual"), "image"), NULL);
            magic = Json_String(Json_Member(frame_for_kind(shared.layout, CARD_FRAME_MAGIC), "image"), NULL);
            if (kind == CARD_FRAME_RITUAL && (!own || !*own || (magic && !strcmp(own, magic)))) kind = CARD_FRAME_MAGIC;
            frame = frame_for_kind(shared.layout, kind);
            out->mod = shared.mod;
            out->color = kind;
            out->full_bleed = 1;
            snprintf(out->name, sizeof(out->name), "%s", frame_kind_names[kind]);
            out->width = frame_box_dimension(frame, "width", CARD_LAYOUT_FRAME_MAX_W, CARD_LAYOUT_FRAME_MAX_W);
            out->height = frame_box_dimension(frame, "height", CARD_LAYOUT_FRAME_MAX_H, CARD_LAYOUT_FRAME_MAX_H);
            if (want_image)
                mod_file(shared.mod, Json_String(Json_Member(frame, "image"), NULL), "\"frame\"", out->image,
                         sizeof(out->image));
            return 1;
        }
        /* Resolve every presentation consistently: an explicit card assignment
         * (above), then its printed type and broad kind (default_styles), then
         * the released order -- a style named for the card's "frame" color (in
         * any applied layout), the first "frame_for" rule that applies, the
         * global default -- and finally retail. */
        if (shared_on) {
            name = default_style_for_type(shared.layout, card);
            if (!name) name = default_style_for_kind(shared.layout, cls);
        }
        probe.mod = -1;
        probe.layout = NULL;
        if (!name && override >= 0 && find_style(color_names[override], -1, first, &probe, &probed) &&
            mod_full_bleed(probe.mod)) {
            name = color_names[override];
        }
        if (shared_on) {
            for (rule = Json_At(Json_Member(shared.layout, "frame_for"), 0); !name && rule; rule = Json_Next(rule)) {
                if (rule_applies(&shared, rule, card, cls)) name = Json_String(Json_Member(rule, "style"), NULL);
            }
            if (!name) name = Json_String(Json_Member(shared.layout, "default_style"), NULL);
        }
        if (!name || !*name) return 0;
        if (!find_style(name, -1, first, &owner, &style)) {
            if (shared_on) note_once(&shared, "no \"frame_styles\" entry named", name);
            return 0;
        }
        if (!mod_full_bleed(owner.mod)) return 0;   /* a style of a mod that is off: retail */
    }
    /* The name is looked up again later (the per-style parts): a cut one
     * would find nothing there. */
    if (strlen(name) >= sizeof(out->name)) {
        note_once(&owner, "\"frame_styles\" name longer than 31 characters", name);
        return 0;
    }
    /* "hand_colour", the key's first spelling, is still read; "hand_color" wins when both are there. */
    hand = Json_Member(style, "hand_color");
    if (!hand) hand = Json_Member(style, "hand_colour");
    out->color = color_named(Json_String(hand, name));
    if (out->color < 0) {
        note_once(&owner, "\"hand_color\" is not gold, green, pink, blue, purple or orange in style", name);
        return 0;
    }
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->mod = owner.mod;
    image = Json_String(Json_Member(style, "image"), NULL);
    /* A style with a picture is full-bleed by default.  Fallback styles may
     * deliberately omit it: they still select a hand palette, but preserve
     * the retail large-card plate for every card that does not opt in. */
    out->full_bleed = Json_Bool(Json_Member(style, "full_bleed"), image && *image);
    out->width = frame_box_dimension(style, "width", CARD_LAYOUT_FRAME_MAX_W, CARD_LAYOUT_FRAME_MAX_W);
    out->height = frame_box_dimension(style, "height", CARD_LAYOUT_FRAME_MAX_H, CARD_LAYOUT_FRAME_MAX_H);
    if (want_image)
    {
        mod_file(owner.mod, image, "\"frame_styles\" style", out->image,
                 sizeof(out->image));
        mod_file(owner.mod, Json_String(Json_Member(style, "hand_image"), NULL), "\"frame_styles\" hand_image",
                 out->hand_image, sizeof(out->hand_image));
        mod_file(owner.mod, Json_String(Json_Member(style, "field_image"), NULL), "\"frame_styles\" field_image",
                 out->field_image, sizeof(out->field_image));
        mod_file(owner.mod, Json_String(Json_Member(style, "field_back_image"), NULL),
                 "\"frame_styles\" field_back_image", out->field_back_image, sizeof(out->field_back_image));
        mod_file(owner.mod, Json_String(Json_Member(style, "back_image"), NULL), "\"frame_styles\" back_image",
                 out->back_image, sizeof(out->back_image));
        mod_file(owner.mod, Json_String(Json_Member(style, "library_icon"), NULL), "\"frame_styles\" library_icon",
                 out->library_icon, sizeof(out->library_icon));
    }
    return 1;
}

int CardLayout_FrameStyleOf(int card_id, CardLayoutFrameStyle *style)
{
    return resolve_style(card_id, style, 0);
}

int CardLayout_StyleOf(int card_id, CardLayoutStyle *style)
{
    CardLayoutFrameStyle full;
    int found = CardLayout_FrameStyleOf(card_id, &full);

    memset(style, 0, sizeof(*style));
    style->color = full.color;
    style->spell_layout = full.spell_layout;
    style->width = full.width;
    style->height = full.height;
    memcpy(style->name, full.name, sizeof(style->name));
    memcpy(style->image, full.image, sizeof(style->image));
    return found;
}

/* The hand renderer cannot use ``current``: it draws several duel cards in
 * one pass, while the large-card viewer has one selected card.  Resolve the
 * style from the passed card id instead. */
static const JsonValue *hand_part_for(int card_id, const char *name)
{
    CardLayoutFrameStyle style;
    const JsonValue *hand;

    if (!resolve_style(card_id, &style, 0)) return NULL;
    hand = Json_Member(style_node_of(&style), "hand");
    return Json_Member(hand, name);
}

int CardLayout_HandStyled(int card_id)
{
    CardLayoutFrameStyle style;

    return resolve_style(card_id, &style, 0) && Json_Member(style_node_of(&style), "hand") != NULL;
}

CardLayoutPlacement CardLayout_HandGet(int card_id, CardLayoutHandElement element)
{
    static const char *const names[] = {"art", "atk", "def", "atk_label", "dfd_label"};
    static const int defaults[][4] = {
        {6, 6, 40, 32}, {14, 40, 8, 8}, {14, 48, 8, 8}, {5, 40, 8, 8}, {5, 48, 8, 8}
    };
    CardLayoutPlacement p = {1, 0, 0, 0, 0};
    const JsonValue *part;

    if (element < CARD_LAYOUT_HAND_ART || element > CARD_LAYOUT_HAND_DEF_LABEL) return p;
    part = hand_part_for(card_id, names[element]);
    p.x = bounded_coordinate(part, "x", defaults[element][0],
                             CARD_LAYOUT_HAND_COORD_MIN, CARD_LAYOUT_HAND_COORD_MAX);
    p.y = bounded_coordinate(part, "y", defaults[element][1],
                             CARD_LAYOUT_HAND_COORD_MIN, CARD_LAYOUT_HAND_COORD_MAX);
    p.w = frame_box_dimension(part, "width", defaults[element][2], CARD_LAYOUT_HAND_MAX_W);
    p.h = frame_box_dimension(part, "height", defaults[element][3], CARD_LAYOUT_HAND_MAX_H);
    p.visible = part_visible(part, 1);
    return p;
}

void CardLayout_HandPlace(int card_id, int element, int *out)
{
    CardLayoutPlacement p = CardLayout_HandGet(card_id, (CardLayoutHandElement)element);

    out[0] = p.visible;
    out[1] = p.x;
    out[2] = p.y;
    out[3] = p.w;
    out[4] = p.h;
}

const char *CardLayout_HandFramePath(int card_id)
{
    static char path[1024];
    CardLayoutFrameStyle style;

    path[0] = 0;
    if (resolve_style(card_id, &style, 1)) snprintf(path, sizeof(path), "%s", style.hand_image);
    return path;
}

const char *CardLayout_FieldFramePath(int card_id)
{
    static char path[1024];
    CardLayoutFrameStyle style;

    path[0] = 0;
    if (resolve_style(card_id, &style, 1)) snprintf(path, sizeof(path), "%s", style.field_image);
    return path;
}

const char *CardLayout_FieldBackPath(int card_id)
{
    static char path[1024];
    CardLayoutFrameStyle style;

    path[0] = 0;
    if (resolve_style(card_id, &style, 1)) snprintf(path, sizeof(path), "%s", style.field_back_image);
    return path;
}

const char *CardLayout_BackImagePath(int card_id)
{
    static char path[1024];
    CardLayoutFrameStyle style;

    path[0] = 0;
    if (resolve_style(card_id, &style, 1)) snprintf(path, sizeof(path), "%s", style.back_image);
    return path;
}

const char *CardLayout_LibraryIconPath(int card_id)
{
    static char path[1024];
    CardLayoutFrameStyle style;

    path[0] = 0;
    if (resolve_style(card_id, &style, 1)) snprintf(path, sizeof(path), "%s", style.library_icon);
    return path;
}

void CardLayout_SetCard(int card_id)
{
    CardLayoutFrameStyle style;
    int cls = Cards_Class(card_id);

    memset(&current, 0, sizeof(current));
    current.mod = -1;
    current.spell_layout = cls == CARD_CLASS_SPELL || cls == CARD_CLASS_EQUIP || cls == CARD_CLASS_RITUAL_SPELL ||
                           cls == CARD_CLASS_TRAP;
    current.width = CARD_LAYOUT_FRAME_MAX_W;
    current.height = CARD_LAYOUT_FRAME_MAX_H;
    if (resolve_style(card_id, &style, 1)) current = style;
    LOG(LOG_CARD_LAYOUT, "SetCard id=%d class=%d image=%s", card_id, cls, current.image);
}

int CardLayout_FullBleed(void)
{
    return current.mod >= 0 && mod_full_bleed(current.mod) && current.full_bleed;
}

int CardLayout_Active(void)
{
    return any_layout_on();
}

int CardLayout_TitleStyled(int card_id)
{
    CardLayoutFrameStyle style;

    /* Only a full-bleed style, as the low-resolution title mask (func_80028B08.c) */
    if (!resolve_style(card_id, &style, 0) || !style.full_bleed) return 0;
    return layout_part(style_node_of(&style), layout_of(style.mod), "title") != NULL;
}

int CardLayout_StarsCentered(void)
{
    LayoutSource source = current_source();
    const JsonValue *styles, *style_node;

    if (source.mod < 0 || !full_bleed_of(&source) || !current.full_bleed) return 0;
    styles = Json_Member(source.layout, "frame_styles");
    style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    /* Before frame_styles gained per-style parts, the bundled anime layout
     * used the shared ``stars.x`` as a centre coordinate.  Keep that
     * established manifest convention.  A style's own stars object is the
     * editor-facing form and deliberately remains a literal anchor. */
    return Json_Member(style_node, "stars") == NULL &&
           Json_Member(source.layout, "stars") != NULL;
}

const char *CardLayout_FramePath(void)
{
    return current.image;
}

/* Every picture the frames of every applied layout whose "full_bleed" is on
 * may draw, each once. */
int CardLayout_FramePaths(char (*paths)[1024], int max)
{
    int count = 0, i;

    for (i = 0; i < Mods_LoadedCount() && count < max; i++) {
        int mod = Mods_Loaded(i);
        const JsonValue *layout, *styles, *style;
        int kind;

        if (!Mods_Active(mod) || !mod_full_bleed(mod)) continue;
        layout = layout_of(mod);
        if (!layout) continue;
        styles = Json_Member(layout, "frame_styles");
        if (styles) {
            for (style = Json_At(styles, 0); style && count < max; style = Json_Next(style)) {
                int k;
                mod_file(mod, Json_String(Json_Member(style, "image"), NULL), "\"frame_styles\" style", paths[count], 1024);
                if (!paths[count][0]) continue;
                for (k = 0; k < count && strcmp(paths[k], paths[count]); k++) {}
                if (k == count) count++;   /* styles that share a picture add nothing */
            }
            continue;
        }
        for (kind = 0; kind < CARD_FRAME_COUNT && count < max; kind++) {
            int k;
            frame_path_of(mod, layout, kind, paths[count], 1024);
            if (!paths[count][0]) continue;
            for (k = 0; k < count && strcmp(paths[k], paths[count]); k++) {}
            if (k == count) count++;
        }
    }
    return count;
}

CardLayoutPart CardLayout_Part(CardLayoutElement element)
{
    CardLayoutPart p = {1, 0, 0, 0, 0, 0x80, 0x80, 0x80};
    LayoutSource source = current_source();
    int full_bleed = full_bleed_of(&source) && current.full_bleed;
    /* The setting off (a fresh install's default): every part where retail
     * has it. A layout's top-level places are measured on its own frame
     * (the HD pack's anime frame), not on the retail plate. */
    if (!full_bleed_of(&source)) source.layout = NULL;
    int spell = CardLayout_IsSpell();
    const JsonValue *styles = Json_Member(source.layout, "frame_styles");
    const JsonValue *style_node = styles && *current.name ? Json_Member(styles, current.name) : NULL;
    const JsonValue *part = NULL;

    switch (element) {
    case CARD_LAYOUT_FRAME: {
        /* The current card's frame style, at most 140x196: the retail
         * viewer's own card-plate footprint. This is intentionally not a
         * movable manifest part: only full_bleed replaces the retail plate.
         * A style's optional back_image uses this same box. */
        p.visible = !full_bleed || Json_Bool(Json_Member(style_node, "game_frame"), 0);
        p.w = current.width;
        p.h = current.height;
        break;
    }
    case CARD_LAYOUT_TITLE:
        /* The retail title sprite already contains the card's current,
         * localized name. A full-bleed frame keeps it only when the style
         * (or shared layout) explicitly supplies a title placement. */
        part = layout_part(style_node, source.layout, "title");
        p.visible = !full_bleed || part != NULL || game_look("title");
        point(part, &p.x, &p.y, 0xC, 0xE);
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_DESCRIPTION:
        /* The description panel is not a movable card part. */
        p.visible = 1;
        break;
    case CARD_LAYOUT_LEVEL_STARS:
        /* Magic/trap/ritual never reach this call (func_80028B08.c's own
         * obj->field_68 < 0x14 gate, unrelated to full_bleed): none of the
         * three have a level to draw stars for, in retail or full-bleed.
         * Position and tint read from the mod's "stars" key either way --
         * only the centring math downstream (func_80028B08.c) still checks
         * full_bleed, since a retail-shaped plate has no box to centre in. */
        part = layout_part(style_node, source.layout, "stars");
        point(part, &p.x, &p.y, 0x77, 0x20);
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_ATTRIBUTE:
        /* Monster/purple/orange: the mod's (or the style's own) "attribute"
         * rect. Magic/trap/ritual: "spell"."icon" instead -- a different
         * place for the same element (func_80028B08.c still draws retail's
         * own elemental texture there, unchanged; those three kinds just
         * have no meaningful one, same as they have no ATK/DEF/level).
         * Position and tint apply with no full-bleed mod too; width/height
         * (the stretch into a custom frame's own cutout) stay retail's 0x0
         * outside one -- a fallback style has no custom art there to
         * stretch into. */
        if (spell) part = Json_Member(layout_part(style_node, source.layout, "spell"), "icon");
        else part = layout_part(style_node, source.layout, "attribute");
        rect(part, &p.x, &p.y, &p.w, &p.h, 0x6E, 0xD, 0, 0);
        tint(part, &p.r, &p.g, &p.b);
        if (!full_bleed) { p.w = 0; p.h = 0; }
        break;
    case CARD_LAYOUT_ATK:
        /* Spell: never reached (same gate as CARD_LAYOUT_LEVEL_STARS). */
        part = digit_stat_part(style_node, source.layout, "atk");
        /* On the game's own frame the numbers' default is the game's place: 0x61, 0x9D is their top left corner
         * and a full bleed's x, y their centre (four digits 6 wide and 13 high). */
        if (!part && game_look("digits")) point(part, &p.x, &p.y, 0x61 + 12, 0x9D + 6);
        else point(part, &p.x, &p.y, 0x61, 0x9D);
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_DEF:
        part = digit_stat_part(style_node, source.layout, "def");
        if (!part && game_look("digits")) point(part, &p.x, &p.y, 0x61 + 12, 0xAB + 6);
        else point(part, &p.x, &p.y, 0x61, 0xAB);
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_ATK_LABEL:
        part = layout_part(style_node, source.layout, "atk_label");
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_DEF_LABEL:
        part = layout_part(style_node, source.layout, "dfd_label");
        tint(part, &p.r, &p.g, &p.b);
        break;
    case CARD_LAYOUT_ART:
        /* w/h stay 0 (retail) unless full-bleed and the layout gives an
         * art object at all: func_80028B08.c reads w != 0 as "stretch the
         * art".  Once an object exists, however, its omitted fields mean
         * the retail art rectangle.  That makes {"y": 84} a useful
         * position-only override, rather than silently leaving the retail
         * sprite at its original position.  Spell reads "spell"."art"
         * instead of "art" -- magic/trap/ritual's own art box, not
         * monster's. */
        if (spell) part = Json_Member(layout_part(style_node, source.layout, "spell"), "art");
        else part = layout_part(style_node, source.layout, "art");
        if (full_bleed) {
            if (part) rect(part, &p.x, &p.y, &p.w, &p.h, 0x13, 0x32, 0x66, 0x60);
        }
        break;
    default:
        break;
    }

    p.visible = part_visible(part, p.visible);

    if (element >= 0 && element < CARD_LAYOUT_ELEMENT_COUNT &&
        (!last_set[element] || last[element].visible != p.visible ||
         last[element].x != p.x || last[element].y != p.y ||
         last[element].w != p.w || last[element].h != p.h ||
         last[element].r != p.r || last[element].g != p.g || last[element].b != p.b)) {
        LOG(LOG_CARD_LAYOUT, "%s: visible=%d x=%d y=%d w=%d h=%d color=%d,%d,%d (full_bleed=%d kind=%s)",
            element_names[element], p.visible, p.x, p.y, p.w, p.h, p.r, p.g, p.b, full_bleed, current.name);
        last[element] = p;
        last_set[element] = 1;
    }
    return p;
}

CardLayoutPlacement CardLayout_Get(CardLayoutElement element)
{
    CardLayoutPart part = CardLayout_Part(element);
    CardLayoutPlacement p = {part.visible, part.x, part.y, part.w, part.h};
    return p;
}
