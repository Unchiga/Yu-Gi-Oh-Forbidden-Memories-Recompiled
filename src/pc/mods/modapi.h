#ifndef MEMORIES_MOD_API_H
#define MEMORIES_MOD_API_H
/* The interface a code mod is built against. A mod is a directory with a
 * mod.json in it, either beside the executable (the ones the release ships)
 * or in the player's own mods directory (paths.h); see notes/modding.md.
 *
 * A mod that only replaces or patches data on the disc needs no code at all:
 * its manifest names the files. A mod with code is one object file, the same
 * for every system (tools/pc/build_mod.py builds it), and defines one symbol,
 *
 *     int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);
 *
 * which fills `mod` in and returns nonzero to accept the load. It is called
 * when the mod is applied, never before, and the code then stays in the
 * process until the game exits.
 *
 * What the host offers here is what a mod can reach safely. There is no
 * network call and no way to name a file outside the mod's own directories:
 * paths are relative, and "..", absolute paths and drive letters are
 * refused. A code mod is still native code linked into the game -- it can
 * reach the whole game image the same way the port's own code does, which is
 * the point of it -- so install mods you trust, as you would any plugin.
 *
 * In a mod, <stdio.h> is the SDK's (src/pc/mods/sdk): FILE is opaque there.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Bumped for new host services, events or manifest features. A mod records
 * the version it was built against; the host refuses a mod built against a
 * later one. New host entries only ever go at the end, so a mod built
 * against an earlier version keeps working, and one built against a later
 * version can check host->api before it calls an entry the host may not
 * have.
 *   1  the first
 *   2  now_us, map_fixed; setting() reads MEMORIES_MOD_<ID>_<KEY> first
 *   3  managed events, registered state and stable card lookup
 *   4  hook/unhook any game function; symbol looks a name up at run time;
 *      provide/find share functions between mods; draw over the picture
 *      (MemoriesMod.overlay); save slot events; more of the C library
 *   5  duelist_id
 *   6  the STARCHIP event
 *   7  card_notes, card_tag: a card's notes and the tags in them
 *   8  limit: the numbers the game caps, as the mods' "limits" set them
 *   9  menu_item and the MENU event
 *  10  the MONSTER event and manifest features listed in notes/modding.md
 *  11  manifest features only; no host layout or event change
 *  12  sound_add, sound_play, sound_free: a code mod's own sounds
 *  13  "limits" "card_drops" (manifest only), which limit() also answers
 *  14  the tower's manifest features (notes/tower-mode.md) only; no host
 *      layout or event change
 *  15  "card_types" (manifest only); no host layout or event change
 *  16  "attributes" (manifest only); no host layout or event change
 *  17  a duelist's bust (notes/more-duelists.md) only; no host layout or
 *      event change
 *  18  the card frames' card_layout keys and a card's frame_style
 *      (notes/modding.md) only; no host layout or event change
 *  19  (held for the extended monster effects, PR #391)
 *  20  "fields" and the "change_field" monster effect (manifest only); no
 *      host layout or event change
 *  21  "models": battle models a mod adds, and a card's "model" naming one
 *      (manifest); no host layout or event change. host->disc_read also
 *      reads a mod's model at Cards_ModelSector (-1 for one bigger than a
 *      record: its base stands in), and AnimatedBattleModelProperties's
 *      model_id is unsigned (0x8000 up)
 *  22  packet_area: room for a mod's polygons past the game's 140 KB frame
 *      buffer (the mods' packet area, src/pc/memory.h)
 * A data mod declares its required version with min_api in mod.json. */
#include "mod_types.h"

typedef struct MemoriesModHost MemoriesModHost;


typedef struct {
    unsigned api;      /* MEMORIES_MOD_API */
    const char *name;  /* optional: the manifest's name is used when this is null */
    /* Once a frame, after the game has queued its own drawing and before the
     * picture is presented, and only while the mod is applied. */
    void (*frame)(void);
    /* The player applied (1) or removed (0) the mod while the game runs. */
    void (*applied)(int on);
    /* A save state was loaded: anything cached from the old game is stale. */
    void (*reset)(void);
    /* The game is closing. */
    void (*shutdown)(void);
    /* --- API 4 ---
     * Draw over the picture, at the window's own resolution, with the host's
     * draw_text and fill (only valid in here). The overlay is drawn again
     * only when overlay_signature returns something new, so return a number
     * that changes with what you draw; without one it is drawn every
     * frame. Only while the mod is applied. */
    void (*overlay)(void);
    unsigned (*overlay_signature)(void);
} MemoriesMod;

struct MemoriesModHost {
    unsigned api;            /* the host's MEMORIES_MOD_API */
    const char *id;          /* this mod's id, from its manifest */
    const char *directory;   /* where this mod was loaded from */
    void *reserved;          /* the host's own; do not touch */

    /* Whether the player has this mod applied. */
    int (*applied)(const MemoriesModHost *host);
    /* A line in the game's log, under the "mods" channel, and whether that
     * channel is on -- worth asking before working out what to say. */
    void (*log)(const MemoriesModHost *host, const char *format, ...);
    int (*log_enabled)(const MemoriesModHost *host);

    /* A file the mod ships, read-only, named relative to its directory. */
    FILE *(*open_asset)(const MemoriesModHost *host, const char *relative);
    /* A file of the mod's own, in the player's directory, opened with the
     * usual fopen modes. This is the only place a mod may write. */
    FILE *(*open_data)(const MemoriesModHost *host, const char *relative, const char *mode);

    /* The mod's settings, kept in the game's settings file as
     * `mod.<id>.<key>`; whole numbers, saved with the player's settings.
     * From API 2, the environment variable MEMORIES_MOD_<ID>_<KEY> (both
     * uppercased, anything but letters and digits an underscore; decimal, or
     * hexadecimal after 0x) wins over the file for the run, which is how a
     * knob is tried without writing it down. set_setting ignores a key that
     * is not letters, digits, '_' and '-', and "order", which the manager
     * keeps for the mod's load order. */
    int (*setting)(const MemoriesModHost *host, const char *key, int fallback);
    void (*set_setting)(const MemoriesModHost *host, const char *key, int value);

    /* The first sector of a file on the disc by its retail path
     * ("\\DATA\\MODEL.MRG;1"), or -1; and a bulk read of 2048-byte blocks
     * beside the drive model, which does not disturb the game's streaming. */
    int (*disc_file_start)(const MemoriesModHost *host, const char *iso_path);
    int (*disc_read)(const MemoriesModHost *host, int lba, int sectors, void *out);

    /* Normalized pad buttons of port 0 or 1, before managed input hooks. */
    unsigned short (*pad)(const MemoriesModHost *host, int port);

    /* --- API 2 --- */

    /* A clock for timing things, in microseconds from an arbitrary start. */
    uint64_t (*now_us)(const MemoriesModHost *host);
    /* Zeroed read/write memory at exactly `address`, which must be a multiple
     * of 64 KiB (Windows reserves memory in those), for the life of the
     * process. NULL if any of the range is already taken. For a mod that
     * needs data at a fixed guest-sized address, as the game's own model
     * code does; anything else should use malloc. */
    void *(*map_fixed)(const MemoriesModHost *host, uintptr_t address, size_t size);
    /* --- API 3 ---
     * A token of zero means registration failed. Hooks remain registered
     * while disabled but never run; failed initialization removes them.
     * Register during Init; do not install raw pointers in game save data. */
    int (*subscribe)(const MemoriesModHost *, unsigned event, int priority, MemoriesModCallback);
    void (*unsubscribe)(const MemoriesModHost *, int token);
    /* One pointer-free state buffer per mod, persisted in save states.
     * Keep it alive until shutdown. Bump version whenever its layout changes.
     * SAVE/BEFORE and LOAD/AFTER let a mod pack/unpack its state here. */
    int (*register_state)(const MemoriesModHost *, void *data, size_t size, unsigned version);
    /* Resolve a stable "mod-id:card-key" identity after card tables build. */
    int (*card_id)(const MemoriesModHost *, const char *identity);

    /* --- API 4 ---
     * Replace one of the game's functions (anything in src/game, named
     * directly: `host->hook(host, Duel_DrawFieldCards, my_draw, &original)`)
     * with `replacement`, which has the same signature. While the mod is
     * applied every call goes to `replacement`; `*original`, when given, is
     * kept pointing at what it displaced -- the game's function, or another
     * mod's replacement made earlier -- so a hook that calls it wraps the
     * function rather than replacing it. `original` must point at storage
     * that lives as long as the mod (a static variable, not a local): the
     * host rewrites it whenever other mods are applied or removed, so read
     * it at each call. Removing the mod
     * takes its hooks out of the way at once. A token, or 0 when `function`
     * is not a game function (port code and the C library cannot be hooked). */
    int (*hook)(const MemoriesModHost *, void *function, void *replacement, void **original);
    void (*unhook)(const MemoriesModHost *, int token);
    /* The address of a game or port name, as the loader binds a mod's
     * undefined names, or NULL: for a name a mod can do without. */
    void *(*symbol)(const MemoriesModHost *, const char *name);
    /* Share something with other mods under a name of this mod's own
     * (letters, digits, '_' and '-'); another mod finds it as
     * "<this mod's id>:<name>" -- in its MemoriesModInit too, when it
     * "requires" this mod, which is then initialized first. 0 when the name
     * is not valid or the table is full; find gives NULL when no loaded mod
     * provides that. What is shared stays loaded until the game exits,
     * whether or not its mod is applied. */
    int (*provide)(const MemoriesModHost *, const char *name, void *pointer);
    void *(*find)(const MemoriesModHost *, const char *qualified);
    /* Overlay drawing, for MemoriesMod.overlay: the canvas's size in pixels
     * and the scale the port draws its own menus at (1 at 480 lines, more in
     * a bigger window); text (ASCII) with `middle` its vertical centre and
     * its width; a rectangle blended in at `alpha` (0-255). Colors are
     * 0xRRGGBB. Outside the overlay callback these do nothing. */
    void (*overlay_size)(const MemoriesModHost *, int *width, int *height, int *scale);
    void (*draw_text)(const MemoriesModHost *, int x, int middle, const char *text, uint32_t rgb, int scale);
    int (*text_width)(const MemoriesModHost *, const char *text, int scale);
    void (*fill)(const MemoriesModHost *, int x, int y, int w, int h, uint32_t rgb, unsigned alpha);

    /* --- API 5 ---
     * Resolve a stable "mod-id:duelist-key" identity to the id that duelist
     * has this run, as card_id does for a card: the id depends on which mods
     * are applied, in what order, and what slots they asked for, so it cannot
     * be written down in advance. 0 for a duelist not here this run, and for
     * one of the disc's own, which its id already names
     * (pc/free_duel/duelists.h). Answers once the duelist list is built,
     * which is before any screen shows it. */
    int (*duelist_id)(const MemoriesModHost *, const char *identity);

    /* --- API 6 ---
     * No new host entries. MEMORIES_EVENT_STARCHIP (mod_types.h) fires when
     * an end-of-duel StarChip prize is about to be added to the save
     * (Mods_AwardStarchips). */

    /* --- API 7 ---
     * A card's notes: the "notes" its "cards" entries gave
     * (notes/more-cards.md), every applied mod's in load order with a line
     * between two, or NULL. The game itself plays by none of it; it is there
     * for the modder, and for a mod that reads tags from it, as RPG Maker's
     * note boxes are read:
     *
     *     "notes": "Burns the opponent when it lands. <burn: 300> <no-fusion>"
     *
     * card_tag writes the value of tag `key` ("burn" gives "300",
     * "no-fusion" gives "") to `out`, cut to fit and ended with a '\0', and
     * returns its whole length, as snprintf does; -1 when the card's notes
     * have no such tag. Names match in any case, spaces around a name or a
     * value do not count, and the last of two tags of one name counts.
     * Answers once the card tables are built, which is before the title. */
    const char *(*card_notes)(const MemoriesModHost *, int id);
    int (*card_tag)(const MemoriesModHost *, int id, const char *key, char *out, size_t size);

    /* --- API 8 ---
     * A limit the game plays by this run, after every applied mod's
     * "limits" (notes/gameplay-tables.md): "attack" and "defense" (the most
     * a monster has, 9999 on the disc), "life_points" (the start against the
     * CPU), "life_points_max" (how far healing goes; 0 while it stops at the
     * start), "two_player_start", "two_player_max", "two_player_step",
     * "starchips", "chest", "free_duel_record" and "two_player_record";
     * and the other values by their keys (the FM Editor's Values tab):
     * "deck_copies", "swords_turns", "crush_card", "spellbinding_circle",
     * "shadow_spell", "rank_score.start", "rank_score.exodia",
     * "rank_score.deck_out", "starchip_prize.S" to "starchip_prize.D" and
     * "new_game_starchips"; from API 13 "card_drops", the cards a mod locks
     * every won duel to (0 while none does and the player's Game > Card
     * drops decides). -1 for a name it does not know. Answers once the card tables are
     * built, which is before the title. A mod that deals damage or bonuses of
     * its own reads the caps here rather than assume 9999. */
    long (*limit)(const MemoriesModHost *, const char *name);

    /* --- API 9 ---
     * The name of item `index` of the title's menus, as MEMORIES_EVENT_MENU
     * gives it: an entry's ("new_game" ... "save", 0 to 10) or a button's
     * "mod-id:button-id" (11 on); NULL for none. A button's number depends
     * on the mods applied, so a mod knows its own by name. */
    const char *(*menu_item)(const MemoriesModHost *, int index);

    /* --- API 12 ---
     * Sounds of the mod's own, played as the game's sound effects are: on
     * the same channels, under the sound-effect volume. sound_add copies
     * `frames` frames of `channels` interleaved signed 16-bit samples at
     * `rate` Hz and returns a handle, or -1; never from an interrupt.
     * sound_play starts one once, `volume` 0-255 and `pan` -128 (left) to
     * 128 (right), and is nonzero when it did; one still playing starts
     * again from its beginning, as a game sound effect takes its own voice
     * again. sound_free stops and frees one. Both sound_add and sound_play
     * work only while the mod is applied (not from MemoriesModInit): the
     * host frees a mod's sounds when it is turned off, and their handles go
     * with them, so a mod adds them when it needs them, or in applied(1). */
    int (*sound_add)(const MemoriesModHost *, const int16_t *samples, size_t frames, int channels, unsigned rate);
    int (*sound_play)(const MemoriesModHost *, int sound, int volume, int pan);
    void (*sound_free)(const MemoriesModHost *, int sound);

    /* --- API 22 ---
     * `bytes` of this frame's share of the mods' packet area (src/pc/memory.h),
     * for polygons the game's own 140 KB frame buffer has no room for, or
     * NULL (none left this frame, or a build without one: then use the game's
     * buffer as before). Packets there link into the ordering tables like any
     * other: sort into it by pointing the packet cursor D_800FE240 at it
     * around GsSortPoly and the like, and putting the cursor back after. Good
     * until the frame's list is drawn (DrawOTag): ask again each frame. */
    void *(*packet_area)(const MemoriesModHost *, size_t bytes);
};

/* The symbol a mod's object defines, and its type. */
typedef int (*MemoriesModEntry)(const MemoriesModHost *host, MemoriesMod *mod);
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);

#endif
