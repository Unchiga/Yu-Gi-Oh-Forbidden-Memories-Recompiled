#ifndef MEMORIES_PC_MODS_H
#define MEMORIES_PC_MODS_H
/* The mod system: what the port itself calls. Mods are directories, not
 * built-in code -- see modapi.h for what one contains and notes/modding.md
 * for how one is written. They are found beside the executable (`mods/`,
 * where the release's own mods live), in a mods folder the player chose
 * (Mods_Folder, below) and in the player's mods directory (paths.h), which
 * is where a player installs someone else's. They are read in that order,
 * and on one id the later wins: the chosen folder over the release's mods,
 * the user directory's over both. MEMORIES_MODS_DIR, when set, is the only
 * folder read. The game never writes to the chosen folder. Whether a mod
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
/* The mouse for host->mouse and host->capture_mouse (API 23): the window's
 * motion since the last call and the buttons held, with
 * MEMORIES_MOUSE_ESCAPE taken in when `escape` (the mod is the one that
 * claimed it); and the capture's mode, 0 none, 1 the pointer and Escape, 2
 * Escape only. Without these the host answers no motion and captures
 * nothing. */
void Mods_SetMouse(void (*motion)(int *dx, int *dy, unsigned *buttons, int escape), void (*capture)(int mode));
/* The pointer and the wheel for host->pointer, host->wheel and
 * host->take_wheel (API 24): the pointer's place in the game's picture and
 * the buttons held, 1 when it is a mouse's over the picture and nothing of
 * the port's has it; the wheel's notches since the last call; and whether a
 * notch is also a D-pad press (0 when a mod has taken the wheel). Without
 * these the host answers no pointer and no notches. */
void Mods_SetPointer(int (*pointer)(int *x, int *y, unsigned *buttons), int (*wheel)(void), void (*take)(int on));
/* The MEMORIES_HUD_* parts the applied mods hide (host->hide_hud), or 0. */
unsigned Mods_HiddenHud(void);
/* Where the window shows the game's picture, for host->overlay_picture:
 * its rectangle on the overlay's canvas and the game pixels across it (320,
 * or 426 widened). The window sets it whenever it lays the picture out. */
void Mods_SetPicture(int x, int y, int w, int h, int game_width);
/* The applied mods' overlays drawn straight over a picture of w x h
 * 0xFFRRGGBB pixels, as a window of its size shows them, with the port's
 * menu text (MEMORIES_DUMP_OVERLAY's frame dumps). */
struct MenuCanvas;
void Mods_DrawPictureOverlay(uint32_t *pixels, int w, int h, int game_width,
                             void (*text)(struct MenuCanvas *, int, int, const char *, uint32_t, int),
                             int (*width)(const char *, int));
void Mods_Shutdown(void);
/* What a mod's host->pad reads; NULL: Platform_Pad. The game sets it
 * (libetc.c) so mods see the control client's bits and, while a recording
 * plays, the recording's (pc/debug/recorder.h). */
extern unsigned short (*Mods_PadSource)(int port);
/* What tells a mod's own MIPS in a model's control modules (notes/modding.md,
 * "Attack code: C, not MIPS"): `mips`, how many of the six control modules in
 * a MODEL.MRG record's module stages (44 sectors from its sector 180) are a
 * mod's, with a line for each in `detail`; `read`, the disc's sectors as the
 * game reads them, the mods' changes in. The game gives them before Mods_Load
 * (pc/overlays/model_module_class.h, Menu_LoadMods); without them (the tests)
 * the mods' models are not checked. `mips` also says how many a code mod's C
 * runs (mod API 29) and whose; `disc_shape`, the shape whose disc row a mod's
 * row repeats (0 for none), for the log.
 *
 * The check runs once every mod has started and again after each change to
 * what is applied or to the mods' rows, so a model's modules are told with
 * the C any applied mod gives for them: the result is the mod's module note
 * (Mods_ModuleNote), never its status. */
void Mods_SetModuleCheck(int (*mips)(const unsigned char *stages, char *detail, size_t size, int *mod_native,
                                     const char **mod_owner),
                         int (*read)(int lba, int sectors, void *out),
                         int (*disc_shape)(int primary, int slot, uint32_t identifier, uint32_t span,
                                           const char *sha256));
/* An applied mod's line on its models' control modules: how many are MIPS
 * code (a warning: notes/modding.md, "Attack code: C, not MIPS") and how
 * many a code mod's C runs; "" for none. Mods_ModuleMips is the first
 * count. */
const char *Mods_ModuleNote(int mod);
int Mods_ModuleMips(int mod);
/* Holds the module notes' check while a set of changes is made (Mods_Apply):
 * it runs once, when the last hold is let go (0), if anything it reads
 * changed. Holds nest. */
void Mods_HoldModuleNotes(int hold);
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
 * reads them). A data mod needs no code for them. Read from each mod.json for
 * Cards_Build, which frees them as it ends (Mods_CardsRelease): a value is gone by
 * then, and this is not to be called again. */
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
 * the run), as the game reads it from a manifest. Until the player or the
 * environment gives it a value, it reads as the "default" the mod's
 * "settings" declare for it -- what the Mods window shows. `fallback`
 * answers for a key the mod does not declare, or declares without a
 * "default", and for no such mod. (A code mod's host->setting is mods.c's
 * host_setting: it answers with the fallback the mod passes.) */
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
/* Whether a mod's frame callback, in the last Mods_DrawFrame, asked that
 * its frame be drawn only in the scaled picture (host->picture_only_frame). */
int Mods_PictureOnlyFrame(void);
/* The applied mods' overlays (MemoriesMod.overlay), drawn with the port's
 * menu text; the rectangle they covered, or zeros. Their signature changes
 * whenever one of them would draw something else (`frame` for a mod that
 * gives none). */
struct MenuCanvas;
void Mods_DrawOverlay(struct MenuCanvas *canvas, int scale,
                      void (*text)(struct MenuCanvas *, int, int, const char *, uint32_t, int),
                      int (*width)(const char *, int), int *x, int *y, int *w, int *h);
unsigned Mods_OverlaySignature(unsigned frame);
/* Where the overlays go instead of the canvas: with a sink set a pass of
 * Mods_DrawOverlay rasters nothing, reports no rectangle, and hands the
 * sink its calls in order. The sink keeps what it needs: between `begin`
 * and `end` the pixels are the mod's and alive, after `end` they may not be.
 * `clip` is x0, y0, x1, y1 of the canvas; `forget` says pixels are gone. */
typedef struct ModsOverlaySink {
    void (*begin)(void);
    void (*fill)(const int clip[4], int x, int y, int w, int h, uint32_t rgb, unsigned alpha);
    void (*image)(const int clip[4], const uint32_t *pixels, int w, int h, uint32_t hash, int x, int y, int dw, int dh,
                  unsigned alpha);
    void (*end)(void);
    void (*forget)(const uint32_t *pixels);
} ModsOverlaySink;
void Mods_SetOverlaySink(const ModsOverlaySink *sink);
/* The pictures read ahead for the mods (host->prefetch_image): the bytes of
 * those read and not yet taken, how many jobs there are in all, and how
 * many of them are still to read or being read. */
size_t Mods_Prefetched(int *jobs, int *unread);
/* The overlays once more, only listed, the canvas untouched, and compared
 * with the calls that last drew it (Mods_DrawOverlay, or a
 * Mods_RedrawOverlayIn after this): the rectangles of the canvas that
 * differ, x, y, w, h each into `rects`, at most `max` of them. 0 when
 * nothing differs; -1 when there is nothing to compare with (no pass on a
 * canvas of this size yet, or one that could not be listed whole), and
 * then everything is to be drawn again. */
int Mods_OverlayChanges(struct MenuCanvas *canvas, int scale,
                        void (*text)(struct MenuCanvas *, int, int, const char *, uint32_t, int),
                        int (*width)(const char *, int), int *rects, int max);
/* Then the overlays drawn in those rectangles alone, which the caller has
 * cleared; outside them the canvas is left as it is, which is what this
 * pass draws there. 0 when the pass was not the one listed (a mod that
 * draws something else each time it is asked): nothing on the canvas can
 * then be trusted, and it is to be drawn again whole. */
int Mods_RedrawOverlayIn(struct MenuCanvas *canvas, int scale,
                         void (*text)(struct MenuCanvas *, int, int, const char *, uint32_t, int),
                         int (*width)(const char *, int), const int *rects, int count);
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
/* The battle models mods add ("models"), in load order. Mods_Model gives
 * model i's name, its "base" (a monster of the disc it stands in for where
 * the game looks a model up as a card, or NULL) and its mod: 0, or -1 past
 * the last. src/pc/cards/cards.c names them.
 * Mods_ModelSector: the first sector of its record as a native extra reads
 * it (host->disc_read), or -1. Once its file is found gone or changed it
 * is -1 until the next launch. Mods_ModelRead serves host->disc_read:
 * `sectors` from `lba` on, within one record, or 0 when its file cannot be
 * read as it was at startup.
 * Mods_ModelWindow: reads it into battle slot
 * `slot`'s window (0 or 1), where the drive finds it, and gives the window's
 * first sector, or -1; only while no transfer runs (Model_LoadMonsterMerge
 * waits for them first). Mods_ModelWindows: the windows' models, which save
 * states carry, and Mods_ModelRebind reads them in again after a load.
 * Mods_ModelForget: the slot loads something else.
 * A model bigger than a record (Mods_ModelGeometry: its bytes of model
 * data, 0 for a record) has no sector; its window holds the record's other
 * stages, and Mods_ModelWindow puts its model data in the slot's arena
 * (Mods_ModelArena: `arena` 0 or 1 for the slots, 2 to check one in; NULL
 * before a bigger model is added), where the slot parses it:
 * Mods_ModelPending until Mods_ModelParsed. A save state brings the arenas
 * and Mods_ModelPendings itself (src/pc/cards/battle_models.c), after the
 * windows. Mods_ModelPages: the texture pages ("texture_pages") of the
 * model in slot `slot`'s window, 64x256 halfwords each, and how many; NULL
 * and 0 for none. */
int Mods_ModelCount(void);
/* A model's "base", once the cards can say what it names. Mods_ModelBaseCard: 0
 * until then, when Mods_Model's `base` has it; from then on Mods_Model's `base` is
 * NULL, and a model dropped since still answers (it loads as that). */
int Mods_ModelBaseCard(int i, int *card);
/* As Cards_Build ends: no block a save state holds keeps a manifest's value. */
void Mods_ModelsSettle(int (*card)(const struct JsonValue *));
int Mods_Model(int i, const char **name, const struct JsonValue **base, int *mod);
int Mods_ModelSector(int i);
int Mods_ModelRead(int lba, int sectors, void *out);
int Mods_ModelWindow(int slot, int i);
int *Mods_ModelWindows(unsigned *size);
void Mods_ModelRebind(void);
void Mods_ModelForget(int slot);
int Mods_ModelGeometry(int i);
unsigned char *Mods_ModelArena(int arena, unsigned *size);
int Mods_ModelPending(int slot);
const unsigned char *Mods_ModelPages(int slot, int *count);
int *Mods_ModelPendings(unsigned *size);
void Mods_ModelParsed(int slot);

/* Manager metadata and configuration. Borrowed strings live until exit. "fusions" and
 * "cards" are empty lists in it: they stay in the mod.json (Mods_EachFusion,
 * Mods_CardsDocument). */
const struct JsonValue *Mods_Manifest(int mod);
/* A mod's "fusions", which is not in its manifest: each entry in turn from the
 * file, gone when `each` returns (json.h). The count, -1 on a mistake. */
long Mods_EachFusion(const char *id, int (*each)(const struct JsonValue *, long, void *), void *context, char *error,
                     size_t error_size);
unsigned Mods_FusionsHash(int mod); /* 0 with none */
/* "cards", also not in the manifest: read for Mods_VisitCards and kept until
 * Mods_CardsRelease, as Cards_Build ends; or a document of them for the caller, which
 * Json_Frees it. */
unsigned Mods_CardsHash(int mod); /* 0 with none */
struct JsonDocument *Mods_CardsDocument(int mod);
void Mods_CardsRelease(void);
long Mods_FusionsSize(int mod); /* bytes in the file, 0 for none */
const char *Mods_Metadata(int mod, const char *key);
const char *Mods_Directory(int mod);
/* Where players put new mods (MEMORIES_MODS_DIR, else the user mods
 * folder), created if missing; 0 on success. */
int Mods_InstallDirectory(char *out, size_t size);
/* The mods folder the player chose, one more folder the game reads (origin
 * "folder"), between the release's mods and the user directory's: the first
 * line of mods-folder.txt in the user directory, else
 * MEMORIES_MODS_FOLDER, which wins when it is set (set to nothing, it says
 * there is none). A path that is not absolute is taken from the program
 * directory, a '\' is a '/' on Windows. 0 and the path in `out`, or -1 when
 * there is none. Whether it can be read is not looked at here. Mods are
 * scanned once, at the first Mods_Load: a change applies at the next launch.
 * MEMORIES_MODS_DIR set means no other folder is read, this one neither. */
int Mods_Folder(char *out, size_t size);
/* Choose the mods folder (writes mods-folder.txt, atomically); NULL or ""
 * chooses none (the file goes). Double quotes around the path (Explorer's
 * "Copy as path") are dropped. The folder need not exist or be readable yet,
 * as when the permission for it is asked for after. 0: saved (or cleared);
 * `why` may then still say that MEMORIES_MODS_FOLDER or MEMORIES_MODS_DIR is
 * set and overrides it, now and until it is unset, for the window to show.
 * -1, with the reason in `why`: the file could not be written, the name holds
 * a line break, is too long, is a drive's current folder ("C:", "C:foo": the
 * full path is needed), or is a folder the game reads or writes already: the
 * release's mods/, MEMORIES_MODS_DIR, the user directory (where the game
 * keeps its mods, saves and downloads) or a folder in one. Folders are
 * compared after taking out "." and ".." and following links where the folder
 * exists (a junction on Windows is not followed). Does not change this
 * launch's list. */
int Mods_SetFolder(const char *path, char *why, size_t why_size);
/* Why this launch did not read the folder of Mods_Folder (it is gone, is not a
 * folder, cannot be read, is a folder the game reads or writes already, or MEMORIES_MODS_DIR
 * is set); "" when it was read or there is none, and before Mods_Load. Said in
 * the log only, never as a notice: for the Mods window to show. Borrowed. */
const char *Mods_FolderProblem(void);
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
/* A mod whose folder the Android Mods panel's Remove deleted (import.h,
 * Mods_ImportRemoveInstalled), while the game runs. Only a mod nothing was
 * loaded from this launch (not Mods_InUse) can go: -1 for any other. Its
 * slot stays in the list, so no other mod's index moves (events and saved
 * state are keyed by it), but it is off for good, cannot be applied, and
 * Mods_Removed says so: the panel leaves it out. A mod imported later with
 * its id takes the slot again (Mods_Discover). */
int Mods_Forget(int mod);
int Mods_Removed(int mod);
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
