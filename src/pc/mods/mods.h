#ifndef MEMORIES_PC_MODS_H
#define MEMORIES_PC_MODS_H
/* The mod system: what the port itself calls. Mods are directories, not
 * built-in code -- see modapi.h for what one contains and notes/modding.md
 * for how one is written. They are found beside the executable (`mods/`,
 * where the release's own mods live) and in the player's mods directory
 * (paths.h), which is where a player installs someone else's. Whether a mod
 * is applied is a setting, `mod.<id>`, saved with the rest of them. */

#define MODS_MAX 256
#include <stddef.h>
#include <stdint.h>
#include "mod_types.h"
struct JsonValue;

/* Find every mod and apply the ones the settings say are applied. Safe to
 * call again (settings reload): the directories are only scanned once. */
void Mods_Load(void);
/* The texture pack loader a "textures" mod goes through (src/pc/render/texture_pack.h),
 * `part` answering an entry's "setting" from the mod's settings; without
 * one, such a mod notes that this build has no texture packs. */
void Mods_SetTexturePack(int (*load)(const char *directory, unsigned rank,
                                     int (*part)(const char *setting, void *context), void *context,
                                     char *problems, size_t size),
                         void (*unload)(void));
/* Named "assets" use the texture pack unload/rebuild lifecycle: `load` for
 * an object listing the pictures, `folder` for a directory of them. */
void Mods_SetAssets(int (*load)(const char *directory, const struct JsonValue *assets, unsigned rank,
                                int (*part)(const char *, void *), void *context, char *problems, size_t size),
                    int (*folder)(const char *directory, unsigned rank,
                                  int (*part)(const char *, void *), void *context,
                                  char *problems, size_t size));
/* The audio replacement an "audio" mod goes through (src/pc/audio/replace.h):
 * `load` decodes a mod's files and returns how many it added, or -1 when the
 * object is malformed, with the first failure in `error`; `unload` drops
 * them. Without one, such a mod notes that this build has no audio. */
void Mods_SetAudio(int (*load)(int mod, const char *id, const char *directory, const struct JsonValue *audio,
                               char *error, size_t size),
                   void (*unload)(int mod));
/* A code mod's own sounds (host->sound_add/sound_play/sound_free): the
 * audio replacement's clips (src/pc/audio/replace.h). Without these the
 * host answers -1 and plays nothing. */
void Mods_SetAudioClips(int (*add)(int mod, const char *id, const int16_t *samples, size_t frames, int channels,
                                   unsigned rate),
                        int (*play)(int mod, int handle, int volume, int pan), void (*release)(int mod, int handle));
void Mods_Shutdown(void);
/* What a mod's host->pad reads; NULL: Platform_Pad. The game sets it
 * (libetc.c) so mods see the control client's bits and, while a recording
 * plays, the recording's (pc/debug/recorder.h). */
extern unsigned short (*Mods_PadSource)(int port);
/* The mods' rand seed (mod_libc.c), its size in *size: save states carry it. */
void *Mods_RandSeed(unsigned *size);

int Mods_Count(void);
const char *Mods_Id(int mod);
const char *Mods_Name(int mod);
/* Empty while all is well, else why this mod did not load. */
const char *Mods_Status(int mod);
/* Whether the player has this mod applied. A mod that wants a restart reads
 * back as applied as soon as the player applies it, because that is what the
 * settings now say; it is the next launch that puts it in place. */
int Mods_Enabled(int mod);
/* Whether changing this mod requires a fresh game process. */
int Mods_RequiresRestart(int mod);
/* Apply or remove a mod: loads its code the first time it is applied,
 * turns its data overrides on or off, and records the setting. The caller
 * saves the settings (the mods window reverts the change if that fails). */
void Mods_SetEnabled(int mod, int enabled);

/* The "cards" array of every applied mod, with the directory its images are
 * named from, in load order (Mods_Loaded, below), for src/pc/cards (json.h
 * reads them). A data mod needs no code for them. */
struct JsonValue;
void Mods_VisitCards(void (*visit)(const char *id, const char *directory, const struct JsonValue *cards, void *context),
                     void *context);
/* Every applied manifest, in load order.  Tables which have their own
 * top-level section use this rather than borrowing the cards visitor. */
void Mods_VisitManifests(void (*visit)(const char *id, const char *directory,
                                       const struct JsonValue *manifest, void *context), void *context);
/* The mods applied at startup, in the order they were loaded: where two
 * of them set the same thing, the later one wins. */
int Mods_LoadedCount(void);
int Mods_Loaded(int index);
/* One of a mod's settings (`mod.<id>.<key>`, or MEMORIES_MOD_<ID>_<KEY> for
 * the run), as a code mod's host->setting reads it. */
int Mods_Setting(const char *id, const char *key, int fallback);
/* One of a mod's files of `key` ("text", "font"; src/pc/text): entry `index`
 * of a name or a list of them, each a string or {"file": ..., "setting": ...}
 * with an optional "value". Returns 0 past the last entry, else 1 with `path`
 * the file inside the mod, or empty when the entry is left out: its declared
 * setting (as the mod was applied) is 0, or is not the entry's "value" when
 * it has one; or it names no file in the mod (noted). A setting the mod does
 * not declare is noted and the file used. `name` is the file as written. */
int Mods_File(int mod, const char *key, int index, char *path, size_t size, const char **name);
/* Whether one entry of a mod's rule list ("fusions", "equips", "rituals";
 * src/pc/cards/tables.c) is used: as a Mods_File entry, its "setting" (and
 * "value") may leave it out; a setting the mod does not declare is noted
 * and the entry used. `where` names the entry ("fusions[3]"). */
int Mods_EntryUsed(const char *id, const struct JsonValue *entry, const char *where);
/* Say why a mod is not quite what it asked for: on stderr and beside it in
 * the Mods window, after the notes it has ("; "), so a reader's reason and
 * a later summary (Starter_Check, Packs_Build) both show; a note it already
 * has is not added twice. */
void Mods_Note(const char *id, const char *format, ...);

/* Called once the game's frame is on its way to the GPU (libgpu's GsDrawOt),
 * which is where an extra pass can draw over the finished picture. */
void Mods_DrawFrame(void);
/* The applied mods' overlays (MemoriesMod.overlay), drawn with the port's
 * menu text; the rectangle they covered, or zeros. Their signature changes
 * whenever one of them would draw something else (`frame` for a mod that
 * gives none). */
struct MenuCanvas;
void Mods_DrawOverlay(struct MenuCanvas *canvas, int scale,
                      void (*text)(struct MenuCanvas *, int, int, const char *, uint32_t, int),
                      int (*width)(const char *, int), int *x, int *y, int *w, int *h);
unsigned Mods_OverlaySignature(unsigned frame);
/* Drop everything cached from the running game: a resumed save state is
 * another game. */
void Mods_Reset(void);

/* Data overrides, from the drive model (libds.c): replace the 2048 bytes of
 * user data a sector delivers. Interrupt context, so this reads its tables
 * and copies; nothing is allocated or opened here. Returns nonzero when the
 * sector was changed. */
int Mods_DiscSector(int lba, void *user_data);
/* Effective named-file metadata, virtual backing and texture provenance.
 * These lookups allocate nothing and are safe in the drive callback. */
int Mods_DiscFileInfo(int retail_lba, int *lba, unsigned *size);
int Mods_DiscSource(int lba, int *physical_lba);
int Mods_DiscOrigin(int lba);
unsigned Mods_DiscSignature(void);

/* Manager metadata and configuration. Borrowed strings live until exit. */
const struct JsonValue *Mods_Manifest(int mod);
const char *Mods_Metadata(int mod, const char *key);
const char *Mods_Directory(int mod);
/* Where players put new mods (MEMORIES_MODS_DIR, else the user mods
 * folder), created if missing; 0 on success. */
int Mods_InstallDirectory(char *out, size_t size);
/* A mod folder put in the player's mods folder while the game runs (the
 * Android Mods panel's Import mod..., import.h), read as the startup scan
 * reads one. A new id joins the list, off (its setting is recorded as off:
 * the caller saves the settings), at the end, so no other mod's index
 * moves. A mod with that id that was never put in place takes the new
 * manifest; one that was (or is) in place keeps what it has until the next
 * launch, now needs a restart for any change, and sets *later. The mod's
 * index, or -1 when there is no mod.json or no room. */
int Mods_Discover(const char *directory, int *later);
/* Whether a mod is (or was, this launch) in place: its code, data, or
 * pictures are the files read then, so they must not change under it. */
int Mods_InUse(int mod);
/* The mods' list or a manifest changed: the next Mods_Overlaps works its
 * lines out again. */
void Mods_OverlapsForget(void);
const char *Mods_Origin(int mod);
int Mods_Active(int mod);
int Mods_Failed(int mod);
/* The mod has code to load (a "library" or "libraries"), whether or not
 * this game can load it. */
int Mods_HasCode(int mod);
/* What the mods in `enabled` change in common, loading in the order `ranks`
 * gives (each mod's Load order, as the Mods window stages it), or the saved
 * order when `ranks` is NULL, with the settings `values` gives (values[mod]
 * [option], staged in the window) or the saved ones when NULL: overlap.h.
 * Worked out again only when the set, the order, a mod's settings or the
 * code mods' hooks change; NULL when memory ran out. MEMORIES_TRACE=mods
 * logs every line each time. */
struct ModsOverlaps;
const struct ModsOverlaps *Mods_Overlaps(const int *enabled, const int *ranks, const int *const *values);
/* Where mod `mod` stands in the last overlaps' list of mods, -1 if not in it. */
int Mods_OverlapPlace(int mod);
/* What the overlaps name cards, duelists and hooked functions by, once the
 * game knows them (pc/cards Cards_Named, Cards_NameUtf8 and a card's base,
 * type and attribute, pc/free_duel
 * Duelists_Named, pc/debug Symbols_Lookup). Once both the cards and the
 * duelists are set, the applied mods' overlaps go to the log under
 * MEMORIES_TRACE=mods. */
void Mods_SetOverlapCards(int (*card)(const char *text, long number), int (*name)(int id, char *out, size_t size),
                          int (*info)(int id, int *base, int *type, int *attribute));
void Mods_SetOverlapDuelists(int (*duelist)(const char *text));
void Mods_SetFunctionNames(const char *(*lookup)(uintptr_t address, uintptr_t *offset));
int Mods_OptionCount(int mod);
const struct JsonValue *Mods_Option(int mod, int option);
int Mods_OptionValue(int mod, int option);
int Mods_OptionValid(int mod, int option, int value);
/* A key a mod's settings may use (mod.<id>.<key>): letters, digits, '_' and
 * '-', and not one the manager keeps for itself ("order"). */
int Mods_SettingKeyValid(const char *key);
int Mods_OptionSet(int mod, int option, int value);
/* Validate the whole proposed set, before saving/changing anything. */
int Mods_CheckManifest(int mod, char *error, size_t size);
int Mods_Compatible(int mod, const int *enabled, char *error, size_t size);
/* The load order of the enabled mods, or -1 on a cycle; order[] then holds
 * the mods that could still be placed, ended by -1. */
int Mods_Order(const int *enabled, int *order, char *error, size_t size);
/* A mod whose requirement is in `enabled` but only goes in place at the next
 * launch (it asks for a restart, or waits on one that does) cannot go live
 * before it either: the requirement it waits on, or -1 when there is none. */
int Mods_WaitsForRestart(int mod, const int *enabled);
int Mods_ProfileValue(const char *name, const char *key, int fallback);
int Mods_Validate(const int *enabled, char *error, size_t size);
int Mods_Apply(const int *enabled, char *error, size_t size);
int Mods_ProfileSave(const char *name);
/* Why the last Mods_ProfileSave could not write its file ("<path>: <reason>.",
 * Paths_WriteError), or "" when it failed on the name or succeeded. */
const char *Mods_ProfileSaveError(void);
int Mods_ProfileRead(const char *name, int *enabled);
void Mods_SetCardSignature(unsigned signature);
unsigned Mods_CardSignature(void);
/* The card packs' files and pictures (pc/cards/packs.h): a "packs" file and
 * its images are not in the manifest the signature hashes. 0 without packs. */
void Mods_SetPackSignature(unsigned signature);
unsigned Mods_PackSignature(void);
void Mods_SetCardResolver(int (*resolve)(const char *));
/* The same for a duelist identity (pc/free_duel/duelists.h), which the free
   duel list injects once it is built. */
void Mods_SetDuelistResolver(int (*resolve)(const char *));
/* A card's notes and a tag's value in them (pc/cards/cards.h Cards_Notes,
   Cards_NoteTag), for the host's card_notes and card_tag. */
void Mods_SetCardNotes(const char *(*notes)(int id), int (*tag)(int id, const char *key, char *out, size_t size));
void Mods_Dispatch(MemoriesModEvent *event);
int Mods_Notify(unsigned type, int a, int b, int c);
unsigned Mods_Sequence(int mod);
/* Counts each mod put in place or taken out (never an unchanged one), so a cache
 * of what mods supplied can tell it is out of date. */
unsigned Mods_Changes(void);
int Mods_RuntimeOption(int mod, int option);
/* One of an applied mod's settings changed (Mods_OptionSet): its texture
 * pack, whose parts may follow the setting, is loaded again. */
void Mods_OptionChanged(int mod, int option);
unsigned Mods_CodeHash(int mod);
unsigned Mods_Signature(void);
int Mods_DamageLife(int side, int life, int damage, int kind);
/* The mods' "limits" by name (pc/cards/tables.h, Tables_Limit), which the
 * card tables hand over once they are built; `fallback` until then, and for
 * a name it does not know. Also the mod API's `limit` (API 8). */
void Mods_SetLimitSource(long (*source)(const char *name));
/* Where host->menu_item (API 9) finds the title menus' items (title_menu.c). */
void Mods_SetMenuItemSource(const char *(*source)(int index));
long Mods_Limit(const char *name, long fallback);
/* Add a duel StarChip prize to *balance (cap 999999, or a mod's "limits"). Dispatches
 * MEMORIES_EVENT_STARCHIP; returns the resulting balance. */
int Mods_AwardStarchips(unsigned *balance, int prize);
#endif
