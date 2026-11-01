/* The mod system (mods.h): finding mods, applying them, and the services
 * they are given (modapi.h).
 *
 * A mod is a directory with a mod.json in it. The release's own mods sit in
 * `mods/` beside the executable; a player's own go in the mods directory of
 * their user directory (paths.h), and win when both hold the same id. The
 * manifest names the mod, says whether it needs a restart, and carries two
 * kinds of content, either or both:
 *
 *  - code: one object file, the same on every system, which the game's own
 *    loader (object_loader.c) links in when the mod is first applied and
 *    keeps for as long as the game runs (unloading one while the game holds
 *    pointers into it is how a port crashes for no visible reason);
 *  - data overrides, which replace or patch what the disc delivers, so that
 *    a mod of card statistics or artwork needs no code at all.
 *
 * What a mod may reach is the host table in modapi.h. There is no network
 * call in it and no way to name a file outside the mod's own directory and
 * its private data directory: Paths_Contained refuses absolute paths, "..",
 * and drive letters. A mod's code is still native code in this process,
 * which is what makes a mod like 3D Monsters possible at all, so the window
 * shows where each mod came from and the notes say plainly that installing
 * one is trusting it. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS, MAP_FIXED_NOREPLACE */
#include "pc/compat/fs.h"
#include "mods.h"
#include "modapi.h"
#include "exports.h"
#include "object_loader.h"
#include "json.h"
#include "manifest_code.h"
#include "events.h"
#include "hooks.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/platform/platform.h"
#include "pc/platform/menu.h"
#include "pc/debug/log.h"
#include "pc/debug/symbols.h"
#include "pc/sdk/disc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include "pc/compat/mman.h"
#include "pc/memory.h" /* the mods' packet area */
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define ID_MAX 64
#define NAME_MAX_ 96
#define PATH_MAX_ 1024
#define STATUS_MAX 512
#define SECTOR 2048
#define REGIONS_MAX 64
#define PATCHES_MAX 1024
/* One MODEL.MRG record (Model_LoadMonsterMerge, src/game/model.h). */
#define MODEL_RECORD_SECTORS 0x114
/* Where a native extra reads a mod's model's record (host_disc_read): model
 * i from MODEL_SECTORS + i records on, past every position the drive can
 * address, so no file of the disc moves for them. */
#define MODEL_SECTORS 0x1000000
#define MODELS_MAX 0x7FFF        /* model ids 0x8000 up, kept plus one in a u16 (pc/cards/cards.h) */
#define MODEL_WINDOWS 2          /* the battle's two model slots */
/* A bigger model's file: its model data (the record's first stage, 96
 * sectors on the disc) is MODEL_GEOMETRY_MAX sectors at most, and the
 * record's other stages, MODEL_TAIL_SECTORS, follow it. The battle parses
 * that data in an arena of the slot's past guest RAM (MODEL_ARENA_BASE,
 * held free by guest/image.c), the third arena for checking one first. */
#define MODEL_STAGE0_SECTORS 96
#define MODEL_TAIL_SECTORS (MODEL_RECORD_SECTORS - MODEL_STAGE0_SECTORS)
#define MODEL_GEOMETRY_MAX 512
/* A model's own texture pages ("texture_pages"), after the record's other
 * stages: 64x256 halfwords of VRAM each, 16 sectors, the slot drawing from
 * a texture bank of its own (src/pc/cards/battle_models.h). */
#define MODEL_PAGE_SECTORS 16
#define MODEL_PAGES_MAX 8
#ifndef MODEL_ARENA_BASE        /* a test's own (tests/pc/disc_test.c) */
#define MODEL_ARENA_BASE 0x90A00000u
#endif
#define MODEL_ARENA_SIZE 0x180000u
#define MODEL_ARENAS (MODEL_WINDOWS + 1)

#define OBJECT_MAX (64u << 20)   /* object_loader.c's own limit */

typedef struct {
    char id[ID_MAX];
    char name[NAME_MAX_];
    char directory[PATH_MAX_];
    char library[NAME_MAX_];     /* empty when the mod is data only */
    char status[STATUS_MAX];
    char warnings[STATUS_MAX];   /* what the manifest got wrong: shown again whenever status is reset */
    const char *origin;          /* "shipped", "folder" or "installed", for the window */
    int restart;                 /* the manifest asks for a fresh process */
    int default_enabled;
    int enabled;                 /* the player's choice, from the settings */
    int active;                  /* and whether it is in place right now */
    int failed;
    int initialized;
    int data_prepared;
    int broken;                  /* it failed to load; it cannot be applied */
    int removed;                 /* its folder was deleted while the game runs (Mods_Forget) */
    int *runtime_options;
    unsigned sequence;
    unsigned code_hash;
    LoadedObject object;         /* the mod's code, once loaded */
    MemoriesMod hooks;
    MemoriesModHost host;
    JsonDocument *manifest;
    const JsonValue *data;       /* the "data" array, applied when enabled */
    const JsonValue *cards;      /* the "cards" array: cards the mod adds (src/pc/cards) */
    const JsonValue *models;     /* the "models" array: battle models the mod adds */
    char textures[PATH_MAX_];    /* a texture pack directory inside the mod, or empty */
    const JsonValue *assets;     /* named image replacements from mod.json, or NULL */
    char asset_dir[PATH_MAX_];   /* "assets": a directory whose PNGs are named by their path */
    const JsonValue *audio;      /* the "audio" object: replacement sounds (src/pc/audio/replace.h) */
    int sounds;                  /* its code added sounds of its own (host->sound_add) */
} Mod;

/* One stretch of the disc a mod replaces, and one run of patched bytes
 * inside a sector. Both are read from interrupt context (Mods_DiscSector),
 * so they are built before they are published and nothing frees them while
 * `overrides_live` is set. */
typedef struct {
    int mod, lba, sectors, file;
    unsigned char *image;    /* the replacement file, mapped read-only */
    size_t image_size, mapped;
    unsigned hash;
} Region;

typedef struct {
    int mod, lba, offset, length, file;
    unsigned char *bytes;
} Patch;

/* File identities remain retail LBAs; only readers see the virtual positions.
 * A patch's lba is file-relative when file >= 0, physical otherwise. */
typedef struct {
    int retail_lba, retail_sectors, lba, sectors, reserved;
    unsigned retail_size, size;
} DiscFile;
static DiscFile disc_files[REGIONS_MAX];
static int disc_file_count, disc_layout_frozen, preparing_data;

static Mod mods[MODS_MAX];
static int mod_count;
static unsigned activation_sequence;
static unsigned change_count;   /* Mods_Changes */
static int scanned;
static int shut_down;   /* Mods_Shutdown has run: no mod code is called again */

static Region regions[REGIONS_MAX];
static int region_count;
/* The models mods add ("models"): only their files' names, sizes and hashes
 * are kept. A record is read when it is wanted, into the window of the
 * battle slot that loads it (Mods_ModelWindow) or for a native extra
 * (host_disc_read, at Mods_ModelSector). */
typedef struct {
    int mod;
    const char *name;
    const JsonValue *base;
    char *path;
    unsigned size, hash;
    int stale;      /* its file was found gone or changed: not read again until the next launch */
    int geometry;   /* sectors of model data of a model bigger than a record; 0 for a record */
    int pages;      /* its own texture pages, after the record's other stages */
} ModModel;
static ModModel *models;
static int model_count, model_capacity;
static unsigned char *arenas[MODEL_ARENAS];
/* The battle slots whose window holds a bigger model that has not been
 * parsed yet (Mods_ModelPending). */
static int pending[MODEL_WINDOWS];
/* The texture pages of the model in each battle slot's window. */
static unsigned char *window_pages[MODEL_WINDOWS];
static int window_page_count[MODEL_WINDOWS];
/* The battle slots' windows: MODEL_RECORD_SECTORS sectors each from
 * window_lba, past the disc and the files it relocates, where the drive
 * reads the record the slot's last load asked for. Mods_DiscSector reads
 * them from interrupt context: the buffers are allocated once, with the
 * first model, and only written while no transfer runs. */
static unsigned char *window_data[MODEL_WINDOWS];
static int window_model[MODEL_WINDOWS] = {-1, -1};
static int window_lba = -1;
static int read_model(int model, int first, int count, unsigned char *out);
static Patch patches[PATCHES_MAX];
static int patch_count;
static int published_regions, published_patches;
/* The mods applied at startup, in the order they were loaded. */
static int loaded[MODS_MAX], loaded_count;
static volatile int overrides_live;
static int override_low, override_high;
static void activate(int index, int on);

static unsigned hash_bytes(unsigned hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Wanted(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "%s", message);
}

/* A manifest's id, name and file names are cut to fit their fields, between
 * two UTF-8 characters. */
static void copy_text(char *out, size_t size, const char *text)
{
    size_t length = strlen(text);
    if (length >= size) length = Menu_TextFit(text, size - 1);
    memcpy(out, text, length);
    out[length] = '\0';
}

static void note(Mod *mod, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(mod->status, sizeof(mod->status), format, arguments);
    va_end(arguments);
    Menu_TextTrim(mod->status);
    fprintf(stderr, "memories-pc: mod %s: %s\n", mod->id, mod->status);
}

/* Something the player should see that does not stop the mod: added to its
 * status after whatever is there, so several warnings show together. With
 * `lasting` it is also kept in `warnings`, which status goes back to when
 * the mod is applied again (a manifest's problems do not go away). */
static void warn(Mod *mod, int lasting, const char *format, ...)
{
    char message[STATUS_MAX];
    va_list arguments;
    size_t length;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: mod %s: warning: %s\n", mod->id, message);
    length = strlen(mod->status);
    snprintf(mod->status + length, sizeof(mod->status) - length, "%s%s", length ? "; " : "", message);
    Menu_TextTrim(mod->status);
    if (lasting) {
        length = strlen(mod->warnings);
        snprintf(mod->warnings + length, sizeof(mod->warnings) - length, "%s%s", length ? "; " : "", message);
        Menu_TextTrim(mod->warnings);
    }
}

/* --- settings -------------------------------------------------------- */

/* Both names return 0 when they do not fit: a cut name would be another
 * setting, so the caller treats it as unset. A mod id always fits (ID_MAX);
 * a key a mod passes may not. */
static int setting_key(char *out, size_t size, const char *id, const char *key)
{
    int length = key ? snprintf(out, size, "mod.%s.%s", id, key) : snprintf(out, size, "mod.%s", id);
    return length >= 0 && (size_t)length < size;
}

/* MEMORIES_MOD_<ID>, or MEMORIES_MOD_<ID>_<KEY> for one of its settings,
 * uppercased, with everything that is not a letter or a digit turned into an
 * underscore: MEMORIES_MOD_3D_MONSTERS=0 for a test run. */
static int environment_name(char *name, size_t size, const char *id, const char *key)
{
    size_t i;
    int length = snprintf(name, size, "MEMORIES_MOD_%s%s%s", id, key ? "_" : "", key ? key : "");
    if (length < 0 || (size_t)length >= size) return 0;
    for (i = strlen("MEMORIES_MOD_"); name[i]; i++) {
        name[i] = isalnum((unsigned char)name[i]) ? (char)toupper((unsigned char)name[i]) : '_';
    }
    return 1;
}

static int environment_choice(const char *id, int fallback)
{
    char name[ID_MAX + 16];
    const char *text;
    if (!environment_name(name, sizeof(name), id, NULL)) return fallback;
    text = getenv(name);
    return text && *text ? atoi(text) != 0 : fallback;
}

/* --- the host table -------------------------------------------------- */

static Mod *owner(const MemoriesModHost *host)
{
    return host ? (Mod *)host->reserved : NULL;
}

static int host_applied(const MemoriesModHost *host)
{
    Mod *mod = owner(host);
    return mod && mod->enabled;
}

static void host_log(const MemoriesModHost *host, const char *format, ...)
{
    char message[512];
    va_list arguments;
    Mod *mod = owner(host);
    if (!Log_Enabled(LOG_MODS)) return; /* a mod may log every frame: traced only */
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "%s: %s", mod ? mod->id : "mod", message);
}

static int host_log_enabled(const MemoriesModHost *host)
{
    (void)host;
    return Log_Enabled(LOG_MODS);
}

static FILE *host_open_asset(const MemoriesModHost *host, const char *relative)
{
    Mod *mod = owner(host);
    char path[PATH_MAX_];
    if (!mod || !Paths_Contained(relative)) return NULL;
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, relative) >= (int)sizeof(path)) return NULL;
    return fopen(path, "rb");
}

static FILE *host_open_data(const MemoriesModHost *host, const char *relative, const char *mode)
{
    Mod *mod = owner(host);
    char relative_path[PATH_MAX_], path[PATH_MAX_];
    if (!mod || !Paths_Contained(relative) || !mode) return NULL;
    if (snprintf(relative_path, sizeof(relative_path), "mod-data/%s/%s", mod->id, relative) >= (int)sizeof(relative_path)) {
        return NULL;
    }
    if (Paths_User(path, sizeof(path), relative_path)) return NULL;
    return fopen(path, mode);
}

static int host_setting(const MemoriesModHost *host, const char *key, int fallback)
{
    Mod *mod = owner(host);
    char name[256];
    const char *text;
    if (!mod || !key || !*key) return fallback;
    text = environment_name(name, sizeof(name), mod->id, key) ? getenv(name) : NULL;
    if (text && *text) return (int)strtol(text, NULL, 0);
    if (!setting_key(name, sizeof(name), mod->id, key)) return fallback;
    return Settings_GetNamed(name, fallback);
}

static void host_set_setting(const MemoriesModHost *host, const char *key, int value)
{
    Mod *mod = owner(host);
    char name[256];
    /* The same keys a declared setting may have: a mod cannot write its own
     * load order, or a key the manager would refuse to show. */
    if (!mod || !Mods_SettingKeyValid(key) || !setting_key(name, sizeof(name), mod->id, key)) return;
    Settings_SetNamed(name, value);
}

static int host_disc_file_start(const MemoriesModHost *host, const char *iso_path)
{
    (void)host;
    return iso_path ? Memories_DiscFileStart(iso_path) : -1;
}

static int host_disc_read(const MemoriesModHost *host, int lba, int sectors, void *out)
{
    (void)host;
    if (!out || sectors <= 0) return 0;
    if (lba >= MODEL_SECTORS) return Mods_ModelRead(lba, sectors, out);
    return Memories_DiscReadSectors(lba, sectors, out);
}

int Mods_ModelRead(int lba, int sectors, void *out)
{
    int first;
    if (!out || sectors <= 0 || lba < MODEL_SECTORS) return 0;
    first = (lba - MODEL_SECTORS) % MODEL_RECORD_SECTORS;
    if (sectors > MODEL_RECORD_SECTORS - first) sectors = MODEL_RECORD_SECTORS - first;
    return read_model((lba - MODEL_SECTORS) / MODEL_RECORD_SECTORS, first, sectors, out);
}

unsigned short (*Mods_PadSource)(int port);

static unsigned short host_pad(const MemoriesModHost *host, int port)
{
    (void)host;
    return Mods_PadSource ? Mods_PadSource(port) : Platform_Pad(port);
}

static uint64_t host_now_us(const MemoriesModHost *host)
{
    struct timespec now;
    (void)host;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void *host_map_fixed(const MemoriesModHost *host, uintptr_t address, size_t size)
{
    void *wanted = (void *)address, *got;
    if (!owner(host) || !address || address % 0x10000u || !size || address + size < address) return NULL;
    /* On Windows compat/mman.h's VirtualAlloc, which refuses an address
     * that is already taken; the 64-bit game holds the mod arenas' range
     * from the start (image.c), and the mmap there gives it back first. */
    got = Memories_MapFixed(wanted, size, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
                            -1, 0);
    return got == MAP_FAILED ? NULL : got;
}

/* The mods' packet area (API 22, src/pc/memory.h): mapped on the first ask,
 * at the first 1 MiB of physical 8..16 MiB that is free, at its guest
 * (direct, KSEG0) address so a packet's own pointer and its link agree. */
static void *host_packet_area(const MemoriesModHost *host, size_t bytes)
{
#ifdef MEMORIES_TRANSLATED
    (void)host;
    (void)bytes;
    return NULL; /* guest memory is not host memory there */
#else
    static int tried;
    if (!owner(host)) return NULL;
    if (!tried) {
        uint32_t physical;
        tried = 1;
        for (physical = MEMORIES_MOD_PACKETS_FIRST; !Memories_ModPacketBase && physical < MEMORIES_MOD_PACKETS_LAST;
             physical += MEMORIES_MOD_PACKETS_SIZE) {
            if (Memories_MapModPackets(physical)) {
                LOG(LOG_MODS, "packet area for mods at physical 0x%x, %u KiB", (unsigned)physical,
                    (unsigned)(MEMORIES_MOD_PACKETS_SIZE / 1024));
            }
        }
        if (!Memories_ModPacketBase) {
            fprintf(stderr, "memories-pc: mods: no room for the mods' packet area: they use the game's\n");
        }
    }
    return Memories_TakeModPackets(bytes);
#endif
}

static int (*card_resolver)(const char *);
static unsigned card_signature;
void Mods_SetCardSignature(unsigned signature) { card_signature = signature; }
unsigned Mods_CardSignature(void) { return card_signature; }
static unsigned pack_signature;
void Mods_SetPackSignature(unsigned signature) { pack_signature = signature; }
unsigned Mods_PackSignature(void) { return pack_signature; }
void Mods_SetCardResolver(int (*resolve_card)(const char *)) { card_resolver = resolve_card; }
static int host_card_id(const MemoriesModHost *host, const char *identity)
{ (void)host; return card_resolver ? card_resolver(identity) : 0; }
static int (*duelist_resolver)(const char *);
void Mods_SetDuelistResolver(int (*resolve)(const char *)) { duelist_resolver = resolve; }
static int host_duelist_id(const MemoriesModHost *host, const char *identity)
{
    /* Duelists_Find answers -1 for one it does not have; the API says 0, as
       card_id does. */
    int id;
    (void)host;
    if (!duelist_resolver) return 0;
    id = duelist_resolver(identity);
    return id > 0 ? id : 0;
}
static long (*limit_source)(const char *);
void Mods_SetLimitSource(long (*source)(const char *)) { limit_source = source; }
long Mods_Limit(const char *name, long fallback)
{
    long value = limit_source && name ? limit_source(name) : -1;
    return value < 0 ? fallback : value;
}
static long host_limit(const MemoriesModHost *host, const char *name)
{ (void)host; return limit_source && name ? limit_source(name) : -1; }
static const char *(*menu_item_source)(int);
void Mods_SetMenuItemSource(const char *(*source)(int)) { menu_item_source = source; }
static const char *host_menu_item(const MemoriesModHost *host, int index)
{ (void)host; return menu_item_source ? menu_item_source(index) : NULL; }
/* Mods_SetAudioClips: the audio replacement's clips. */
static int (*clip_add)(int, const char *, const int16_t *, size_t, int, unsigned);
static int (*clip_play)(int, int, int, int);
static void (*clip_free)(int, int);
static int host_sound_add(const MemoriesModHost *host, const int16_t *samples, size_t frames, int channels,
                          unsigned rate)
{
    Mod *mod = owner(host);
    /* Only while applied: turning the mod off frees them (Mods_Apply), and
     * from MemoriesModInit, which runs once, they would not come back. */
    int sound = mod && mod->active && clip_add ? clip_add((int)(mod - mods), mod->id, samples, frames, channels, rate)
                                               : -1;
    if (sound > 0) mod->sounds = 1;
    return sound;
}
static int host_sound_play(const MemoriesModHost *host, int sound, int volume, int pan)
{
    Mod *mod = owner(host);
    return mod && mod->active && clip_play ? clip_play((int)(mod - mods), sound, volume, pan) : 0;
}
static void host_sound_free(const MemoriesModHost *host, int sound)
{
    Mod *mod = owner(host);
    if (mod && clip_free) clip_free((int)(mod - mods), sound);
}
static const char *(*notes_source)(int);
static int (*tag_source)(int, const char *, char *, size_t);
void Mods_SetCardNotes(const char *(*notes)(int), int (*tag)(int, const char *, char *, size_t))
{
    notes_source = notes;
    tag_source = tag;
}
static const char *host_card_notes(const MemoriesModHost *host, int id)
{ (void)host; return notes_source ? notes_source(id) : NULL; }
static int host_card_tag(const MemoriesModHost *host, int id, const char *key, char *out, size_t size)
{
    (void)host;
    if (out && size) *out = '\0';
    return tag_source ? tag_source(id, key, out, size) : -1;
}
static int host_subscribe(const MemoriesModHost *host, unsigned event, int priority, MemoriesModCallback callback)
{ return owner(host) ? Mods_Subscribe((int)(owner(host) - mods), event, priority, callback) : 0; }
static void host_unsubscribe(const MemoriesModHost *host, int token)
{ if (owner(host)) Mods_Unsubscribe((int)(owner(host) - mods), token); }
static int host_register_state(const MemoriesModHost *host, void *data, size_t size, unsigned version)
{ return owner(host) ? Mods_RegisterState((int)(owner(host) - mods), data, size, version) : 0; }

static int host_hook(const MemoriesModHost *host, void *function, void *replacement, void **original)
{ return owner(host) ? Hooks_Add((int)(owner(host) - mods), function, replacement, original) : 0; }
static void host_unhook(const MemoriesModHost *host, int token)
{ if (owner(host)) Hooks_Remove((int)(owner(host) - mods), token); }
static void *host_symbol(const MemoriesModHost *host, const char *name)
{ return owner(host) && name ? Mods_Lookup(name) : NULL; }
static int host_provide(const MemoriesModHost *host, const char *name, void *pointer)
{ return owner(host) ? Mods_Provide((int)(owner(host) - mods), name, pointer) : 0; }
static void *host_find(const MemoriesModHost *host, const char *qualified)
{ return owner(host) ? Mods_Find(qualified) : NULL; }

/* --- drawing over the picture ------------------------------------------ */

/* Only while Mods_DrawOverlay runs a mod's overlay callback. */
static struct {
    MenuCanvas *canvas;
    int scale;
    void (*text)(MenuCanvas *, int, int, const char *, uint32_t, int);
    int (*width)(const char *, int);
    int x0, y0, x1, y1;   /* what the mods drew, to report as the overlay's bounds */
} overlay;

static void overlay_touch(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > overlay.canvas->width) x1 = overlay.canvas->width;
    if (y1 > overlay.canvas->height) y1 = overlay.canvas->height;
    if (x >= x1 || y >= y1) return;
    if (overlay.x0 >= overlay.x1) { overlay.x0 = x; overlay.y0 = y; overlay.x1 = x1; overlay.y1 = y1; return; }
    if (x < overlay.x0) overlay.x0 = x;
    if (y < overlay.y0) overlay.y0 = y;
    if (x1 > overlay.x1) overlay.x1 = x1;
    if (y1 > overlay.y1) overlay.y1 = y1;
}

static void host_overlay_size(const MemoriesModHost *host, int *width, int *height, int *scale)
{
    int on = owner(host) && overlay.canvas;
    if (width) *width = on ? overlay.canvas->width : 0;
    if (height) *height = on ? overlay.canvas->height : 0;
    if (scale) *scale = on ? overlay.scale : 1;
}

static int host_text_width(const MemoriesModHost *host, const char *text, int scale)
{
    return owner(host) && overlay.width && text ? overlay.width(text, scale < 1 ? 1 : scale) : 0;
}

static void host_draw_text(const MemoriesModHost *host, int x, int middle, const char *text, uint32_t rgb, int scale)
{
    if (!owner(host) || !overlay.canvas || !text) return;
    if (scale < 1) scale = 1;
    overlay.text(overlay.canvas, x, middle, text, rgb & 0xFFFFFFu, scale);
    overlay_touch(x, middle - 10 * scale, overlay.width(text, scale), 20 * scale);
}

static void host_fill(const MemoriesModHost *host, int x, int y, int w, int h, uint32_t rgb, unsigned alpha)
{
    MenuCanvas *canvas = overlay.canvas;
    unsigned inverse;
    if (!owner(host) || !canvas || w <= 0 || h <= 0) return;
    if (alpha > 255) alpha = 255;
    inverse = 255 - alpha;
    for (int row = y < 0 ? 0 : y; row < y + h && row < canvas->height; row++) {
        for (int column = x < 0 ? 0 : x; column < x + w && column < canvas->width; column++) {
            uint32_t *pixel = canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)column, under = *pixel;
            unsigned r = ((rgb >> 16 & 255) * alpha + (under >> 16 & 255) * inverse) / 255;
            unsigned g = ((rgb >> 8 & 255) * alpha + (under >> 8 & 255) * inverse) / 255;
            unsigned b = ((rgb & 255) * alpha + (under & 255) * inverse) / 255;
            unsigned a = alpha + (under >> 24) * inverse / 255;
            *pixel = a << 24 | r << 16 | g << 8 | b;
        }
    }
    overlay_touch(x, y, w, h);
}

void Mods_DrawOverlay(MenuCanvas *canvas, int scale, void (*text)(MenuCanvas *, int, int, const char *, uint32_t, int),
                      int (*width)(const char *, int), int *x, int *y, int *w, int *h)
{
    *x = *y = *w = *h = 0;
    if (shut_down || !canvas || !canvas->pixels || !text || !width) return;
    overlay.canvas = canvas;
    overlay.scale = scale < 1 ? 1 : scale;
    overlay.text = text;
    overlay.width = width;
    overlay.x0 = overlay.y0 = overlay.x1 = overlay.y1 = 0;
    for (int i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.overlay) mods[i].hooks.overlay();
    }
    overlay.canvas = NULL;
    if (overlay.x0 < overlay.x1) {
        *x = overlay.x0; *y = overlay.y0; *w = overlay.x1 - overlay.x0; *h = overlay.y1 - overlay.y0;
    }
}

unsigned Mods_OverlaySignature(unsigned frame)
{
    unsigned signature = 0;
    if (shut_down) return 0;
    for (int i = 0; i < mod_count; i++) {
        const MemoriesMod *hooks = &mods[i].hooks;
        if (!mods[i].active || !mods[i].initialized || !hooks->overlay) continue;
        signature = signature * 31u + (unsigned)i + 1u;
        signature = signature * 2654435761u + (hooks->overlay_signature ? hooks->overlay_signature() : frame);
    }
    return signature;
}

static void fill_host(Mod *mod)
{
    mod->host.hook = host_hook;
    mod->host.unhook = host_unhook;
    mod->host.symbol = host_symbol;
    mod->host.provide = host_provide;
    mod->host.find = host_find;
    mod->host.overlay_size = host_overlay_size;
    mod->host.draw_text = host_draw_text;
    mod->host.text_width = host_text_width;
    mod->host.fill = host_fill;
    mod->host.subscribe = host_subscribe;
    mod->host.unsubscribe = host_unsubscribe;
    mod->host.register_state = host_register_state;
    mod->host.card_id = host_card_id;
    mod->host.duelist_id = host_duelist_id;
    mod->host.card_notes = host_card_notes;
    mod->host.card_tag = host_card_tag;
    mod->host.limit = host_limit;
    mod->host.menu_item = host_menu_item;
    mod->host.sound_add = host_sound_add;
    mod->host.sound_play = host_sound_play;
    mod->host.sound_free = host_sound_free;
    mod->host.packet_area = host_packet_area;
    mod->host.api = MEMORIES_MOD_API;
    mod->host.id = mod->id;
    mod->host.directory = mod->directory;
    mod->host.reserved = mod;
    mod->host.applied = host_applied;
    mod->host.log = host_log;
    mod->host.log_enabled = host_log_enabled;
    mod->host.open_asset = host_open_asset;
    mod->host.open_data = host_open_data;
    mod->host.setting = host_setting;
    mod->host.set_setting = host_set_setting;
    mod->host.disc_file_start = host_disc_file_start;
    mod->host.disc_read = host_disc_read;
    mod->host.pad = host_pad;
    mod->host.now_us = host_now_us;
    mod->host.map_fixed = host_map_fixed;
}

/* --- data overrides -------------------------------------------------- */

static int disc_file(int lba, unsigned size)
{
    int i;
    for (i = 0; i < disc_file_count; i++)
        if (disc_files[i].retail_lba == lba) return i;
    if (i == REGIONS_MAX || !size || size > (unsigned)INT_MAX - SECTOR) return -1;
    disc_files[i] = (DiscFile){lba, (int)((size + SECTOR - 1) / SECTOR), lba,
                             (int)((size + SECTOR - 1) / SECTOR), 0, size, size};
    disc_file_count++;
    return i;
}

/* Prepare all data before any mod initializer may cache a position. After
 * publication, even a failed code mod must not move another mod's file. */
static int layout_disc_files(void)
{
    int order[REGIONS_MAX], next = -1;
    for (int i = 0; i < disc_file_count; i++) {
        DiscFile *file = &disc_files[i];
        int maximum = file->retail_sectors, sectors;
        unsigned size = file->retail_size; /* readers never see a partial value */
        for (int j = 0; j < region_count; j++) if (regions[j].file == i) {
            sectors = (int)((regions[j].image_size + SECTOR - 1) / SECTOR);
            if (sectors > maximum) maximum = sectors;
            size = (unsigned)regions[j].image_size;
        }
        sectors = (int)(((size_t)size + SECTOR - 1) / SECTOR);
        file->size = size;
        file->sectors = sectors < file->retail_sectors ? file->retail_sectors : sectors;
        if (!disc_layout_frozen) {
            file->lba = file->retail_lba;
            file->reserved = maximum > file->retail_sectors ? maximum : 0;
        }
        order[i] = i;
        for (int j = i; j > 0 && disc_files[order[j]].retail_lba < disc_files[order[j - 1]].retail_lba; j--) {
            int swap = order[j]; order[j] = order[j - 1]; order[j - 1] = swap;
        }
    }
    if (disc_layout_frozen) return 1;
    for (int i = 0; i < disc_file_count; i++) {
        DiscFile *file = &disc_files[order[i]];
        if (!file->reserved) continue;
        if (next < 0) next = Memories_DiscSectorCount();
        /* One guard sector, and room for the drive's one-past-end position. */
        if (next < 0 || next >= MEMORIES_DISC_MAX_LBA ||
            file->reserved > MEMORIES_DISC_MAX_LBA - next - 1) return 0;
        file->lba = next;
        next += file->reserved + 1;
    }
    /* The battle slots' model windows follow the relocated files. */
    window_lba = -1;
    if (model_count) {
        if (next < 0) next = Memories_DiscSectorCount();
        if (next < 0 || next >= MEMORIES_DISC_MAX_LBA ||
            MODEL_WINDOWS * MODEL_RECORD_SECTORS > MEMORIES_DISC_MAX_LBA - next - 1) return 0;
        window_lba = next;
    }
    return 1;
}

static int read_file(int model, unsigned char *out);

/* Model `model`'s whole record, as read_file reads it, into `out` (a
 * record's bytes); a file shorter than the record reads as zeros past its
 * end. 0 for a model bigger than a record, which has no record of its own. */
static int read_record(int model, unsigned char *out)
{
    if (model < 0 || model >= model_count || models[model].geometry || !read_file(model, out)) return 0;
    memset(out + models[model].size, 0, (size_t)MODEL_RECORD_SECTORS * SECTOR - models[model].size);
    return 1;
}

/* Model `model`'s file, whole (models[model].size bytes), into `out`: 0 when
 * it cannot be read or is no longer what it was at startup, its size or its
 * bytes (a mod updated in place is used from the next launch, as its other
 * files are), so a parser is never handed a model that changed under it. */
static int read_file(int model, unsigned char *out)
{
    size_t size, got = 0;
    struct stat info;
    int file;
    if (model < 0 || model >= model_count || !models[model].path || models[model].stale) return 0;
#ifdef _WIN32
    file = open(models[model].path, O_RDONLY | O_BINARY);
#else
    file = open(models[model].path, O_RDONLY);
#endif
    /* Gone, another size or other bytes: stale. Not opened or not read
     * whole (a scanner holding the file): tried again next time. */
    if (file < 0) {
        if (errno == ENOENT) models[model].stale = 1;
        return 0;
    }
    if (fstat(file, &info) || (uint64_t)info.st_size != models[model].size) {
        if (!fstat(file, &info) && (uint64_t)info.st_size != models[model].size) models[model].stale = 1;
        close(file);
        return 0;
    }
    size = models[model].size;
    while (got < size) {
#ifdef _WIN32
        int chunk = _read(file, out + got, (unsigned)(size - got));
#else
        ssize_t chunk = read(file, out + got, size - got);
#endif
        if (chunk <= 0) break;
        got += (size_t)chunk;
    }
    close(file);
    if (got < size) return 0;
    if (hash_bytes(2166136261u, out, size) != models[model].hash) {
        models[model].stale = 1;
        return 0;
    }
    return 1;
}

/* Sectors first to first + count of model `model`'s record, as read_record
 * reads it; 0 when it cannot. */
static int read_model(int model, int first, int count, unsigned char *out)
{
    unsigned char *record;
    int ok;
    if (first < 0 || count <= 0 || count > MODEL_RECORD_SECTORS - first) return 0;
    if (first == 0 && count == MODEL_RECORD_SECTORS) return read_record(model, out) ? count : 0;
    if (!(record = malloc((size_t)MODEL_RECORD_SECTORS * SECTOR))) return 0;
    ok = read_record(model, record);
    if (ok) memcpy(out, record + (size_t)first * SECTOR, (size_t)count * SECTOR);
    free(record);
    return ok ? count : 0;
}

int Mods_ModelCount(void) { return model_count; }
int Mods_Model(int i, const char **name, const JsonValue **base, int *mod)
{
    if (i < 0 || i >= model_count || !models[i].path) return -1;
    if (name) *name = models[i].name;
    if (base) *base = models[i].base;
    if (mod) *mod = models[i].mod;
    return 0;
}

int Mods_ModelSector(int i)
{
    return i >= 0 && i < model_count && models[i].path && !models[i].stale && !models[i].geometry
               ? MODEL_SECTORS + i * MODEL_RECORD_SECTORS : -1;
}

/* Fills battle slot `slot`'s window with model i's record, and for a bigger
 * model its arena with the model data (`data`: not when a save state is
 * loaded, which brings the arena as the game left it). */
static int fill_window(int slot, int i, int data)
{
    unsigned char *file = NULL;
    int ok;
    window_model[slot] = -1;
    pending[slot] = 0;
    window_page_count[slot] = 0;
    if (i >= 0 && i < model_count && models[i].geometry) {
        size_t front = (size_t)models[i].geometry * SECTOR, stage0 = (size_t)MODEL_STAGE0_SECTORS * SECTOR;
        ok = arenas[slot] && (file = malloc(models[i].size)) != NULL && read_file(i, file);
        if (ok) {
            /* The record's first stage is consumed without being copied
             * (Mods_ModelPending): the window holds the rest. */
            memset(window_data[slot], 0, stage0);
            memcpy(window_data[slot] + stage0, file + front, (size_t)MODEL_TAIL_SECTORS * SECTOR);
            if (models[i].pages) {
                size_t bytes = (size_t)models[i].pages * MODEL_PAGE_SECTORS * SECTOR;
                if (!window_pages[slot]) window_pages[slot] = malloc((size_t)MODEL_PAGES_MAX * MODEL_PAGE_SECTORS * SECTOR);
                ok = window_pages[slot] != NULL;
                if (ok) memcpy(window_pages[slot], file + front + (size_t)MODEL_TAIL_SECTORS * SECTOR, bytes);
                if (ok) window_page_count[slot] = models[i].pages;
            }
            if (ok && data) {
                memcpy(arenas[slot], file, front);
                memset(arenas[slot] + front, 0, MODEL_ARENA_SIZE - front);
                pending[slot] = 1;
            }
        }
        free(file);
    } else {
        ok = read_record(i, window_data[slot]);
    }
    /* The record as it was at startup, or nothing: the caller then loads
     * another, and a transfer a save state resumes reads zeros. */
    if (!ok) {
        memset(window_data[slot], 0, (size_t)MODEL_RECORD_SECTORS * SECTOR);
        say("model %d: %s cannot be read as it was at startup", i,
            i >= 0 && i < model_count && models[i].path ? models[i].path : "(none)");
        return -1;
    }
    window_model[slot] = i;
    return window_lba + slot * MODEL_RECORD_SECTORS;
}

int Mods_ModelWindow(int slot, int i)
{
    if (slot < 0 || slot >= MODEL_WINDOWS || window_lba < 0 || !window_data[slot]) return -1;
    return fill_window(slot, i, 1);
}

void Mods_ModelForget(int slot)
{
    if (slot < 0 || slot >= MODEL_WINDOWS || !window_data[slot]) return;
    window_model[slot] = -1;
    pending[slot] = 0;
    window_page_count[slot] = 0;
    memset(window_data[slot], 0, (size_t)MODEL_RECORD_SECTORS * SECTOR);
}

int Mods_ModelPending(int slot) { return slot >= 0 && slot < MODEL_WINDOWS && pending[slot]; }
const unsigned char *Mods_ModelPages(int slot, int *count)
{
    int pages = slot >= 0 && slot < MODEL_WINDOWS ? window_page_count[slot] : 0;
    if (count) *count = pages;
    return pages ? window_pages[slot] : NULL;
}
int *Mods_ModelPendings(unsigned *size)
{
    *size = sizeof(pending);
    return pending;
}
void Mods_ModelParsed(int slot) { if (slot >= 0 && slot < MODEL_WINDOWS) pending[slot] = 0; }

unsigned char *Mods_ModelArena(int arena, unsigned *size)
{
    if (size) *size = MODEL_ARENA_SIZE;
    return arena >= 0 && arena < MODEL_ARENAS ? arenas[arena] : NULL;
}

int Mods_ModelGeometry(int i)
{
    return i >= 0 && i < model_count && models[i].path ? models[i].geometry * SECTOR : 0;
}

int *Mods_ModelWindows(unsigned *size)
{
    *size = sizeof(window_model);
    return window_model;
}

void Mods_ModelRebind(void)
{
    for (int slot = 0; slot < MODEL_WINDOWS; slot++) {
        pending[slot] = 0;
        if (window_model[slot] >= 0) fill_window(slot, window_model[slot], 0);
    }
}

static int model_window(int lba)
{
    if (window_lba < 0 || lba < window_lba || lba - window_lba >= MODEL_WINDOWS * MODEL_RECORD_SECTORS) return -1;
    return (lba - window_lba) / MODEL_RECORD_SECTORS;
}

int Mods_DiscFileInfo(int retail_lba, int *lba, unsigned *size)
{
    if (!overrides_live) return 0;
    for (int i = 0; i < disc_file_count; i++) if (disc_files[i].retail_lba == retail_lba) {
        if (lba) *lba = disc_files[i].lba;
        if (size) *size = disc_files[i].size;
        return 1;
    }
    return 0;
}

static int virtual_file(int lba)
{
    if (!overrides_live) return -1;
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        if (file->reserved && lba >= file->lba && lba - file->lba < file->sectors) return i;
    }
    return -1;
}

int Mods_DiscSource(int lba, int *source)
{
    int i = virtual_file(lba);
    if (overrides_live && model_window(lba) >= 0) {
        *source = -1;
        return 1;
    }
    if (i < 0) return 0;
    *source = lba - disc_files[i].lba < disc_files[i].retail_sectors
        ? disc_files[i].retail_lba + lba - disc_files[i].lba : -1;
    return 1;
}

int Mods_DiscOrigin(int lba)
{
    if (!overrides_live) return lba;
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        if (file->reserved && lba >= file->retail_lba && lba - file->retail_lba < file->retail_sectors)
            return file->lba + lba - file->retail_lba;
    }
    return lba;
}

unsigned Mods_DiscSignature(void)
{
    unsigned hash = 2166136261u;
    int relocated = 0;
    for (int i = 0; i < disc_file_count; i++) relocated |= disc_files[i].reserved != 0;
    if (!region_count && !patch_count && !relocated && !model_count) return 0;
    if (model_count) {   /* without models the signature is the one it always was */
        for (int i = 0; i < model_count; i++) {
            hash = hash_bytes(hash, &models[i].hash, sizeof(models[i].hash));
            hash = hash_bytes(hash, &models[i].size, sizeof(models[i].size));
            /* the pages move where the model data ends: a model with none hashes as before */
            if (models[i].pages) hash = hash_bytes(hash, &models[i].pages, sizeof(models[i].pages));
        }
        hash = hash_bytes(hash, &window_lba, sizeof(window_lba));
    }
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        hash = hash_bytes(hash, &file->retail_lba, sizeof(file->retail_lba));
        hash = hash_bytes(hash, &file->lba, sizeof(file->lba));
        hash = hash_bytes(hash, &file->size, sizeof(file->size));
    }
    for (int i = 0; i < region_count; i++) {
        hash = hash_bytes(hash, &regions[i].hash, sizeof(regions[i].hash));
        hash = hash_bytes(hash, &regions[i].lba, sizeof(regions[i].lba));
        hash = hash_bytes(hash, &regions[i].sectors, sizeof(regions[i].sectors));
    }
    for (int i = 0; i < patch_count; i++) {
        hash = hash_bytes(hash, &patches[i].lba, sizeof(patches[i].lba));
        hash = hash_bytes(hash, &patches[i].offset, sizeof(patches[i].offset));
        hash = hash_bytes(hash, patches[i].bytes, (size_t)patches[i].length);
    }
    return hash;
}

static void publish_overrides(void)
{
    int i, low = 0x7fffffff, high = -1;
    layout_disc_files(); /* capacity is checked before adding a region */
    for (i = 0; i < region_count; i++) {
        if (regions[i].lba < low) low = regions[i].lba;
        if (regions[i].lba + regions[i].sectors - 1 > high) high = regions[i].lba + regions[i].sectors - 1;
    }
    for (i = 0; i < patch_count; i++) {
        if (patches[i].file >= 0) {
            const DiscFile *file = &disc_files[patches[i].file];
            if (file->retail_lba < low) low = file->retail_lba;
            if (file->retail_lba + file->retail_sectors - 1 > high)
                high = file->retail_lba + file->retail_sectors - 1;
            continue;
        }
        if (patches[i].lba < low) low = patches[i].lba;
        if (patches[i].lba > high) high = patches[i].lba;
    }
    for (i = 0; i < disc_file_count; i++) if (disc_files[i].reserved) {
        if (disc_files[i].lba < low) low = disc_files[i].lba;
        if (disc_files[i].lba + disc_files[i].sectors - 1 > high)
            high = disc_files[i].lba + disc_files[i].sectors - 1;
    }
    if (window_lba >= 0) {
        if (window_lba < low) low = window_lba;
        if (window_lba + MODEL_WINDOWS * MODEL_RECORD_SECTORS - 1 > high)
            high = window_lba + MODEL_WINDOWS * MODEL_RECORD_SECTORS - 1;
    }
    published_regions = region_count;
    published_patches = patch_count;
    override_low = low;
    override_high = high;
    /* The tables are written before the interrupt is told to read them. */
    __asm__ volatile("" ::: "memory");
    overrides_live = high >= low;
}

/* A replacement file, read-only, for as long as the mod is applied. Linux
 * maps it; Windows has no mmap of a file the port could declare by hand
 * without <windows.h>, so it is read into memory. */
static void *map_file(int file, size_t size)
{
#ifdef _WIN32
    unsigned char *image = malloc(size);
    size_t got = 0;
    if (!image) return NULL;
    while (got < size) {
        int chunk = _read(file, image + got, (unsigned)(size - got > 0x10000000u ? 0x10000000u : size - got));
        if (chunk <= 0) { free(image); return NULL; }
        got += (size_t)chunk;
    }
    return image;
#else
    void *image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, file, 0);
    return image == MAP_FAILED ? NULL : image;
#endif
}

static void unmap_file(void *image, size_t size)
{
#ifdef _WIN32
    (void)size;
    free(image);
#else
    munmap(image, size);
#endif
}

static void drop_overrides(int mod)
{
    int i, kept = 0, owned = 0;
    mods[mod].data_prepared = 0;
    /* Many failure paths call this for mods with no data. Unpublishing for
     * them would briefly hide every relocated file from the drive. */
    for (i = 0; i < region_count; i++) owned |= regions[i].mod == mod;
    for (i = 0; i < patch_count; i++) owned |= patches[i].mod == mod;
    for (i = 0; i < model_count; i++) owned |= models[i].mod == mod;
    if (!owned) return;
    overrides_live = 0;   /* the drive model stops looking before anything goes */
    __asm__ volatile("" ::: "memory");
    for (i = 0; i < region_count; i++) {
        if (regions[i].mod != mod) { regions[kept++] = regions[i]; continue; }
        if (regions[i].image) unmap_file(regions[i].image, regions[i].mapped);
    }
    region_count = kept;
    for (i = 0, kept = 0; i < patch_count; i++) {
        if (patches[i].mod != mod) { patches[kept++] = patches[i]; continue; }
        free(patches[i].bytes);
    }
    patch_count = kept;
    /* Once the disc's layout is frozen the cards have taken the models'
     * places (Cards_ModelId): a dropped mod's models stay in the list, with
     * no file, so that no other mod's moves; they load as their bases. */
    for (i = 0, kept = 0; i < model_count; i++) {
        if (models[i].mod != mod) { models[kept++] = models[i]; continue; }
        for (int slot = 0; slot < MODEL_WINDOWS; slot++) if (window_model[slot] == i) window_model[slot] = -1;
        free(models[i].path);
        models[i].path = NULL;
        if (disc_layout_frozen) kept++;
    }
    model_count = kept;
    publish_overrides();
}

/* "26 25" or "2625": the bytes a tutorial writes out. Returns how many were
 * read, or -1 if the text is not pairs of hexadecimal digits. */
static int read_bytes(const char *text, unsigned char **out)
{
    size_t digits = 0, i;
    unsigned char *bytes;
    int value = 0, half = 0;
    for (i = 0; text[i]; i++) {
        if (isxdigit((unsigned char)text[i])) digits++;
        else if (!isspace((unsigned char)text[i]) && text[i] != ',') return -1;
    }
    if (!digits || digits % 2) return -1;
    bytes = malloc(digits / 2);
    if (!bytes) return -1;
    for (i = 0, digits = 0; text[i]; i++) {
        int digit;
        if (!isxdigit((unsigned char)text[i])) continue;
        digit = isdigit((unsigned char)text[i]) ? text[i] - '0' : tolower((unsigned char)text[i]) - 'a' + 10;
        value = value * 16 + digit;
        if (half) { bytes[digits++] = (unsigned char)value; value = 0; }
        half = !half;
    }
    *out = bytes;
    return (int)digits;
}

/* The disc's streamed files: XA audio and the STR movie. Their sectors are
 * MODE2 Form 2, 2304 bytes each (an XA sector's sound, a movie's
 * interleaved sound), where an override writes the 2048 of a data sector:
 * a replacement or patch would leave the rest of each sector's old sound
 * after the new bytes. Their sounds are replaced from files instead, with
 * the "audio" key (src/pc/audio/replace.h). The name of the streamed file
 * the sectors [lba, lba + sectors) reach into, or NULL. */
static const char *streamed_file(const char *file, int lba, int sectors)
{
    static const char *const streamed[] = {"\\DATA\\MASTER.XA;1", "\\DATA\\MOVIE.STR;1"};
    size_t i;
    if (file) {
        const char *dot = strrchr(file, '.');
        size_t length = dot ? strcspn(dot, ";") : 0;
        if (dot && ((length == 3 && !strncmp(dot, ".XA", 3)) || (length == 4 && !strncmp(dot, ".STR", 4)))) return file;
        return NULL;
    }
    for (i = 0; i < sizeof(streamed) / sizeof(streamed[0]); i++) {
        int start;
        unsigned size;
        if (Memories_DiscOriginalFileInfo(streamed[i], &start, &size) || start < 0) continue;
        if (lba < start + (int)((size + SECTOR - 1) / SECTOR) && start < lba + sectors) return streamed[i];
    }
    return NULL;
}

/* One patch, split at the sector boundaries it crosses. Bytes another mod
 * patches too are the later mod's: the Mods window says so. */
static int add_patch(Mod *mod, int index, int file, int lba, int offset, const unsigned char *bytes, int length)
{
    int warned = 0;
    while (length > 0) {
        int here = SECTOR - offset, i;
        if (here > length) here = length;
        if (patch_count >= PATCHES_MAX) {
            note(mod, "more than %d patched byte runs", PATCHES_MAX);
            return 0;
        }
        for (i = 0; i < patch_count && !warned; i++) {
            const Patch *other = &patches[i];
            if (other->mod != index && other->file == file && other->lba == lba &&
                other->offset < offset + here && offset < other->offset + other->length) {
                warn(mod, 0, "patches the same bytes as %s; later patch wins", mods[other->mod].id);
                warned = 1;
            }
        }
        patches[patch_count].bytes = malloc((size_t)here);
        if (!patches[patch_count].bytes) return 0;
        memcpy(patches[patch_count].bytes, bytes, (size_t)here);
        patches[patch_count].mod = index;
        patches[patch_count].file = file;
        patches[patch_count].lba = lba;
        patches[patch_count].offset = offset;
        patches[patch_count].length = here;
        patch_count++;
        bytes += here;
        length -= here;
        offset = 0;
        lba++;
    }
    return 1;
}

static int add_region(Mod *mod, int index, int named, int lba, int sectors, const char *replacement)
{
    char path[PATH_MAX_];
    struct stat info;
    int file;
    void *image;
    if (region_count >= REGIONS_MAX) {
        note(mod, "more than %d replaced files", REGIONS_MAX);
        return 0;
    }
    if (!Paths_Contained(replacement)) {
        note(mod, "\"replace\": %s is outside the mod", replacement);
        return 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, replacement) >= (int)sizeof(path)) return 0;
#ifdef _WIN32
    file = open(path, O_RDONLY | O_BINARY);
#else
    file = open(path, O_RDONLY);
#endif
    if (file < 0 || fstat(file, &info) || info.st_size <= 0) {
        if (file >= 0) close(file);
        note(mod, "cannot read %s", replacement);
        return 0;
    }
    if ((uint64_t)info.st_size > (uint64_t)MEMORIES_DISC_MAX_LBA * SECTOR ||
        (named < 0 && (uint64_t)info.st_size > (uint64_t)sectors * SECTOR)) {
        close(file);
        note(mod, "%s exceeds %s capacity; replacement rejected", replacement,
             named < 0 ? "the raw sector region" : "the virtual disc address");
        return 0;
    }
    image = map_file(file, (size_t)info.st_size);
    close(file);
    if (!image) {
        note(mod, "cannot map %s", replacement);
        return 0;
    }
    regions[region_count].mod = index;
    regions[region_count].file = named;
    regions[region_count].lba = lba;
    regions[region_count].sectors = sectors;
    regions[region_count].image = image;
    regions[region_count].image_size = (size_t)info.st_size;
    regions[region_count].mapped = (size_t)info.st_size;
    regions[region_count].hash = hash_bytes(2166136261u, image, (size_t)info.st_size);
    for (int i = 0; i < region_count; i++) {
        const Region *other = &regions[i];
        if (named >= 0 ? other->file == named
                       : other->file < 0 && other->mod != index && other->lba < lba + sectors &&
                             lba < other->lba + other->sectors) {
            warn(mod, 0, "%s replaces the same %s as %s; later replacement wins", replacement,
                 named >= 0 ? "disc file" : "sectors", mods[other->mod].id);
            break;
        }
    }
    region_count++;
    if (!layout_disc_files()) {
        note(mod, "virtual disc address space exhausted by %s; replacement rejected", replacement);
        return 0; /* apply_overrides rolls back every contribution of this mod */
    }
    return 1;
}

/* One of a mod's "models": a MODEL.MRG record of its own, read from the
 * mod's file whenever the game or a native extra wants it. */
/* The arenas of the bigger models, mapped once, with the first one. */
static int map_arenas(void)
{
    for (int i = 0; i < MODEL_ARENAS; i++) {
        void *got;
        if (arenas[i]) continue;
        got = Memories_MapFixed((void *)(uintptr_t)(MODEL_ARENA_BASE + (unsigned)i * MODEL_ARENA_SIZE),
                                MODEL_ARENA_SIZE, PROT_READ | PROT_WRITE,
                                MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got == MAP_FAILED) return 0;
        arenas[i] = got;
    }
    return 1;
}

static int add_model(Mod *mod, int index, const JsonValue *entry, int at)
{
    const char *name = Json_String(Json_Member(entry, "name"), NULL);
    const char *file = Json_String(Json_Member(entry, "file"), NULL);
    char path[PATH_MAX_];
    struct stat info;
    unsigned char *image;
    const JsonValue *pages_value = Json_Member(entry, "texture_pages");
    long pages = Json_Number(pages_value, 0);
    int handle, geometry = 0;
    if (!name || !*name || !file) {
        note(mod, "models[%d] needs a \"name\" and a \"file\"", at);
        return 0;
    }
    if (model_count >= MODELS_MAX) {
        note(mod, "more than %d models", MODELS_MAX);
        return 0;
    }
    if (pages_value && (pages < 0 || pages > MODEL_PAGES_MAX || Json_TypeOf(pages_value) != JSON_NUMBER)) {
        note(mod, "models[%d]: \"texture_pages\" is a count from 0 to %d", at, MODEL_PAGES_MAX);
        return 0;
    }
    if (!Paths_Contained(file)) {
        note(mod, "models[%d]: %s is outside the mod", at, file);
        return 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, file) >= (int)sizeof(path)) return 0;
#ifdef _WIN32
    handle = open(path, O_RDONLY | O_BINARY);
#else
    handle = open(path, O_RDONLY);
#endif
    if (handle < 0 || fstat(handle, &info) || info.st_size <= 0) {
        if (handle >= 0) close(handle);
        note(mod, "cannot read %s", file);
        return 0;
    }
    if (pages && (uint64_t)info.st_size <= (uint64_t)MODEL_RECORD_SECTORS * SECTOR) {
        close(handle);
        note(mod, "%s: its %ld texture pages would follow the record's other stages, and it is no larger than a "
                  "record; model rejected", file, pages);
        return 0;
    }
    if ((uint64_t)info.st_size > (uint64_t)MODEL_RECORD_SECTORS * SECTOR) {
        /* A bigger model: whole sectors, model data, the record's other
         * stages, then its texture pages (notes/modding.md, "Bigger models"). */
        uint64_t sectors = (uint64_t)info.st_size / SECTOR, after = MODEL_TAIL_SECTORS + (uint64_t)pages * MODEL_PAGE_SECTORS;
        if (info.st_size % SECTOR || sectors <= after || sectors - after > MODEL_GEOMETRY_MAX ||
            (pages && sectors - after < MODEL_STAGE0_SECTORS)) {
            close(handle);
            note(mod, "%s is larger than a model record (%d sectors) and is not a bigger model (whole sectors, "
                      "%d of model data at most, %d at least with pages, then the record's other %d, then %ld "
                      "texture pages of %d); "
                      "model rejected",
                 file, MODEL_RECORD_SECTORS, MODEL_GEOMETRY_MAX, MODEL_STAGE0_SECTORS, MODEL_TAIL_SECTORS, pages,
                 MODEL_PAGE_SECTORS);
            return 0;
        }
        if (!map_arenas()) {
            close(handle);
            note(mod, "%s: no room for a bigger model's data at 0x%X; model rejected", file, MODEL_ARENA_BASE);
            return 0;
        }
        geometry = (int)(sectors - after);
    }
    /* Read once, for its hash (the mod set's signature, which save states check). */
    image = map_file(handle, (size_t)info.st_size);
    close(handle);
    if (!image) {
        note(mod, "cannot read %s", file);
        return 0;
    }
    if (model_count == model_capacity) {
        int capacity = model_capacity ? model_capacity * 2 : 16;
        ModModel *grown = realloc(models, (size_t)capacity * sizeof(*models));
        if (!grown) { unmap_file(image, (size_t)info.st_size); return 0; }
        models = grown;
        model_capacity = capacity;
    }
    for (int slot = 0; slot < MODEL_WINDOWS; slot++)
        if (!window_data[slot] && !(window_data[slot] = calloc(MODEL_RECORD_SECTORS, SECTOR))) {
            unmap_file(image, (size_t)info.st_size);
            return 0;
        }
    for (int i = 0; i < model_count; i++)
        if (!strcmp(models[i].name, name)) {
            if (models[i].mod == index) warn(mod, 0, "two models are named %s; the later one wins", name);
            else warn(mod, 0, "model %s is also %s's; the later one wins", name, mods[models[i].mod].id);
            break;
        }
    models[model_count].mod = index;
    models[model_count].name = name;
    models[model_count].base = Json_Member(entry, "base");
    models[model_count].size = (unsigned)info.st_size;
    models[model_count].hash = hash_bytes(2166136261u, image, (size_t)info.st_size);
    models[model_count].stale = 0;
    models[model_count].geometry = geometry;
    models[model_count].pages = (int)pages;
    unmap_file(image, (size_t)info.st_size);
    if (!(models[model_count].path = strdup(path))) return 0;
    model_count++;
    if (!layout_disc_files()) {
        note(mod, "virtual disc address space exhausted by the model windows");
        return 0;   /* apply_overrides rolls back every contribution of this mod */
    }
    return 1;
}

/* The "data" array of a manifest, once the mod is applied. */
static int apply_overrides(Mod *mod, int index)
{
    int i, count = Json_Count(mod->data);
    const char *stream = NULL;
    for (i = 0; i < count; i++) {
        const JsonValue *entry = Json_At(mod->data, i);
        const char *file = Json_String(Json_Member(entry, "file"), NULL);
        const JsonValue *replace = Json_Member(entry, "replace");
        const JsonValue *patch = Json_Member(entry, "patch");
        long lba_number = Json_Number(Json_Member(entry, "lba"), -1);
        int lba = lba_number >= 0 && lba_number <= INT_MAX ? (int)lba_number : -1;
        unsigned size = 0;
        int named = -1;
        if ((replace && Json_TypeOf(replace) != JSON_STRING) ||
            (patch && Json_TypeOf(patch) != JSON_ARRAY) || (!replace && !patch)) {
            note(mod, "a data entry needs a string \"replace\" or an array \"patch\"");
            goto failed;
        }
        if (file) {
            if (Memories_DiscOriginalFileInfo(file, &lba, &size) || lba < 0) {
                note(mod, "%s is not on the disc", file);
                goto failed;
            }
            if ((stream = streamed_file(file, lba, 0)) != NULL) goto streamed;
            named = disc_file(lba, size);
            if (named < 0) { note(mod, "cannot register disc file %s", file); goto failed; }
        } else if (lba < 0) {
            note(mod, "a data entry names neither \"file\" nor \"lba\"");
            goto failed;
        }
        if (Json_String(replace, NULL)) {
            long sector_number = size ? (long)(((uint64_t)size + SECTOR - 1) / SECTOR)
                : Json_Number(Json_Member(entry, "sectors"), 0);
            int sectors = sector_number > 0 && sector_number <= INT_MAX ? (int)sector_number : 0;
            if (sectors <= 0 || lba > INT_MAX - sectors) {
                note(mod, "\"replace\" at sector %d needs a \"sectors\" count", lba);
                goto failed;
            } else if (!file && (stream = streamed_file(NULL, lba, sectors)) != NULL) {
                goto streamed;
            } else if (!add_region(mod, index, named, lba, sectors, Json_String(replace, NULL))) {
                goto failed;
            }
        }
        if (patch) {
            int p, runs = Json_Count(patch);
            for (p = 0; p < runs; p++) {
                const JsonValue *run = Json_At(patch, p);
                long at = Json_Number(Json_Member(run, "at"), -1);
                const char *text = Json_String(Json_Member(run, "bytes"), NULL);
                unsigned char *bytes;
                int length;
                if (at < 0 || !text) {
                    note(mod, "a patch needs \"at\" and \"bytes\"");
                    goto failed;
                }
                length = read_bytes(text, &bytes);
                if (length < 0) {
                    note(mod, "\"bytes\": %s is not hexadecimal", text);
                    goto failed;
                }
                if (named >= 0) {
                    size = disc_files[named].size;
                    if (size < disc_files[named].retail_size) size = disc_files[named].retail_size;
                }
                if (!preparing_data && file && size && ((unsigned long)at > size || (unsigned long)length > size - (unsigned long)at)) {
                    note(mod, "a patch at 0x%lX reaches past %s", at, file);
                    free(bytes);
                    goto failed;
                }
                if (!file && at <= INT_MAX - length &&
                    (stream = streamed_file(NULL, lba + (int)(at / SECTOR),
                                            (int)((at % SECTOR + length + SECTOR - 1) / SECTOR))) != NULL) {
                    free(bytes);
                    goto streamed;
                }
                if (at > INT_MAX - length || at / SECTOR > INT_MAX - lba - (length + SECTOR - 1) / SECTOR ||
                    !add_patch(mod, index, named, (named < 0 ? lba : 0) + (int)(at / SECTOR), (int)(at % SECTOR), bytes, length)) {
                    free(bytes);
                    goto failed;
                }
                free(bytes);
            }
        }
    }
    for (i = 0; i < Json_Count(mod->models); i++)
        if (!add_model(mod, index, Json_At(mod->models, i), i)) goto failed;
    publish_overrides();
    if (region_count || patch_count) {
        say("%s: %d replaced regions, %d patched runs, sectors %d to %d",
            mod->id, region_count, patch_count, override_low, override_high);
    }
    if (Json_Count(mod->models)) {
        say("%s: %d models (%d in all), the battle's windows at sector %d",
            mod->id, Json_Count(mod->models), model_count, window_lba);
    }
    return 1;
streamed:
    note(mod, "%s is streamed XA/STR audio or video, which \"data\" cannot replace or patch; "
         "use \"audio\" for its sounds", stream);
failed:
    drop_overrides(index);
    if (!mod->status[0]) note(mod, "could not prepare data overrides");
    return 0;
}

static int validate_disc_patches(void)
{
    for (int i = 0; i < patch_count; i++) if (patches[i].file >= 0) {
        const Patch *patch = &patches[i];
        const DiscFile *file = &disc_files[patch->file];
        uint64_t end = (uint64_t)patch->lba * SECTOR + patch->offset + patch->length;
        unsigned size = file->size > file->retail_size ? file->size : file->retail_size;
        if (end > size) {
            int mod = patch->mod;
            note(&mods[mod], "a patch reaches past the final replacement (size %u)", size);
            mods[mod].failed = 1;
            mods[mod].data_prepared = 0;
            if (mods[mod].active) activate(mod, 0);
            else drop_overrides(mod);
            return 0;
        }
    }
    return 1;
}

int Mods_DiscSector(int lba, void *user_data)
{
    unsigned char *out = user_data;
    int changed = 0, i, named, relative = -1, physical = lba;
    if (!overrides_live) return 0;
    __asm__ volatile("" ::: "memory");
    if (lba < override_low || lba > override_high) return 0;
    if ((i = model_window(lba)) >= 0) {   /* a battle slot's model (Mods_ModelWindow) */
        memcpy(out, window_data[i] + (size_t)((lba - window_lba) % MODEL_RECORD_SECTORS) * SECTOR, SECTOR);
        return 1;
    }
    named = virtual_file(lba);
    if (named >= 0) {
        relative = lba - disc_files[named].lba;
        physical = relative < disc_files[named].retail_sectors ? disc_files[named].retail_lba + relative : -1;
    }
    for (i = 0; i < published_regions; i++) {
        const Region *region = &regions[i];
        size_t at, have;
        if (named >= 0 && region->file == named) at = (size_t)relative * SECTOR;
        else {
            if (physical < region->lba || physical - region->lba >= region->sectors) continue;
            at = (size_t)(physical - region->lba) * SECTOR;
        }
        have = at < region->image_size ? region->image_size - at : 0;
        if (have > SECTOR) have = SECTOR;
        if (have) memcpy(out, region->image + at, have);
        if (have < SECTOR) memset(out + have, 0, SECTOR - have);
        changed = 1;
    }
    for (i = 0; i < published_patches; i++) {
        const Patch *patch = &patches[i];
        if (patch->file >= 0) {
            const DiscFile *file = &disc_files[patch->file];
            if (named == patch->file) {
                if (patch->lba != relative) continue;
            } else if (physical < file->retail_lba || physical - file->retail_lba >= file->retail_sectors ||
                       patch->lba != physical - file->retail_lba) continue;
        } else if (patch->lba != physical) continue;
        memcpy(out + patches[i].offset, patches[i].bytes, (size_t)patches[i].length);
        changed = 1;
    }
    return changed;
}

/* --- the names a code mod binds to ----------------------------------- */

static int by_name(const void *key, const void *entry)
{
    return strcmp(key, ((const MemoriesModExport *)entry)->name);
}

void *Mods_Lookup(const char *name)
{
    const MemoriesModExport *found;
    union { void (*function)(void); void *pointer; } libc;
    if (!name) return NULL;
    libc.function = Mods_LibcLookup(name);
    if (libc.function) return libc.pointer;
    found = bsearch(name, Memories_ModExports, Memories_ModExportCount, sizeof(*Memories_ModExports), by_name);
    return found ? found->address : NULL;
}

int Mods_PrintExports(void)
{
    unsigned i;
    for (i = 0; i < Memories_ModExportCount; i++) {
        printf("%08lx %s\n", (unsigned long)(size_t)Memories_ModExports[i].address, Memories_ModExports[i].name);
    }
    return fflush(stdout) ? 1 : 0;
}

/* --- loading --------------------------------------------------------- */

static void *resolve(const char *name, void *context)
{
    (void)context;
    return Mods_Lookup(name);
}

/* The mod's functions, for crash and hang reports: "3d-monsters:draw_frame". */
static void register_symbols(Mod *mod)
{
    SymbolsEntry *entries = calloc(mod->object.symbol_count ? mod->object.symbol_count : 1, sizeof(*entries));
    char (*names)[72] = calloc(mod->object.symbol_count ? mod->object.symbol_count : 1, sizeof(*names));
    size_t i, count = 0;
    if (entries && names) {
        for (i = 0; i < mod->object.symbol_count; i++) {
            const struct ObjectSymbol *symbol = &mod->object.symbols[i];
            if (!symbol->function) continue;
            snprintf(names[count], sizeof(names[count]), "%s:%s", mod->id, symbol->name);
            entries[count].address = symbol->address;
            entries[count].size = symbol->size;
            entries[count].name = names[count];
            count++;
        }
        Symbols_Add(entries, count);
    }
    free(entries);
    free(names);
}

/* The mod's object file, read whole and handed to the loader. */
static void *load_object(Mod *mod, const char *path)
{
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
    char error[STATUS_MAX];
    if (ObjectLoader_LoadPath(path, &mod->object, error, sizeof(error))) {
        note(mod, "%s %s", mod->library, error);
        return NULL;
    }
    mod->code_hash = mod->object.hash;
    return ObjectLoader_Symbol(&mod->object, "MemoriesModInit");
#else
    FILE *file = fopen(path, "rb");
    unsigned char *data = NULL;
    long size;
    char error[STATUS_MAX];
    void *entry;
    if (!file) {
        note(mod, "cannot read %s", mod->library);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || (unsigned long)size > OBJECT_MAX ||
        fseek(file, 0, SEEK_SET) || !(data = malloc(size ? (size_t)size : 1)) ||
        fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        note(mod, "cannot read %s", mod->library);
        return NULL;
    }
    fclose(file);
    if (ObjectLoader_Load(data, (size_t)size, resolve, mod, &mod->object, error, sizeof(error))) {
        free(data);
        note(mod, "%s %s", mod->library, error);
        return NULL;
    }
    mod->code_hash = mod->object.hash;
    free(data);
    entry = ObjectLoader_Symbol(&mod->object, "MemoriesModInit");
    if (!entry) {
        note(mod, "%s has no MemoriesModInit", mod->library);
        return NULL;
    }
    register_symbols(mod);
    return entry;
#endif
}

#if !defined(MEMORIES_NO_CODE_MODS) && !(defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED))
/* Whether the mod's object for this game is there to load. */
static int library_present(const Mod *mod)
{
    char path[PATH_MAX_];
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, mod->library) >= (int)sizeof(path)) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

#if defined(__ANDROID__) && defined(MEMORIES_TRANSLATED)
/* Whether the mod has the object the fixed-memory Android game loaded
 * (<library>.aarch64.o where this game's is <library>.aarch64-translated.o). */
static int old_android_object(const Mod *mod)
{
    static const char ours[] = "." OBJECT_LOADER_TARGET ".o";
    char path[PATH_MAX_];
    size_t length = strlen(mod->library);
    FILE *file;
    if (length <= sizeof(ours) - 1 || strcmp(mod->library + length - (sizeof(ours) - 1), ours)) return 0;
    if (snprintf(path, sizeof(path), "%s/%.*s.aarch64.o", mod->directory, (int)(length - (sizeof(ours) - 1)),
                 mod->library) >= (int)sizeof(path)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}
#endif
#endif

static int load_library(Mod *mod)
{
    char path[PATH_MAX_];
    MemoriesModEntry entry;
    union { void *pointer; int (*function)(const MemoriesModHost *, MemoriesMod *); } symbol;
    if (!mod->library[0]) return 1;   /* data only: nothing to load */
    if (mod->object.image || mod->object.native_handle) return 1;
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, mod->library) >= (int)sizeof(path)) return 0;
    symbol.pointer = load_object(mod, path);
    if (!symbol.pointer) return 0;
    entry = symbol.function;
    fill_host(mod);
    memset(&mod->hooks, 0, sizeof(mod->hooks));
    if (!entry(&mod->host, &mod->hooks)) {
        memset(&mod->hooks, 0, sizeof(mod->hooks));
        Mods_ClearHooks((int)(mod - mods));
        note(mod, "refused to start");
        return 0;
    }
    if (!mod->hooks.api || mod->hooks.api > MEMORIES_MOD_API) {
        /* 0 is a mod that never said which table it filled in. */
        if (!mod->hooks.api) note(mod, "MemoriesModInit did not set mod->api");
        else note(mod, "was built for mod API %u; this game has %u", mod->hooks.api, MEMORIES_MOD_API);
        memset(&mod->hooks, 0, sizeof(mod->hooks));
        Mods_ClearHooks((int)(mod - mods));
        return 0;
    }
    if (mod->hooks.name && *mod->hooks.name && !mod->name[0]) {
        copy_text(mod->name, sizeof(mod->name), mod->hooks.name);
    }
    mod->initialized = 1;
    say("%s: loaded %s", mod->id, mod->library);
    /* Always said (stderr, which an Android app sends to logcat): which code
     * ran is the first thing to know about a report. */
    fprintf(stderr, "memories-pc: mods: %s loaded its code (%s)\n", mod->id, mod->library);
    return 1;
}

/* Every key a manifest's top level may have (here, in manager.c and in
 * notes/modding.md and mod-api-3.md). Anything else is most likely a typo
 * that would leave the mod doing nothing, so it is a warning, not an error. */
static const char *const manifest_keys[] = {
    "id", "name", "version", "author", "description", "library", "libraries", "enabled", "restart", "legacy_setting",
    "data", "textures", "assets", "cards", "audio", "min_api", "game", "requires", "after", "conflicts", "priority",
    "settings", "fusions", "equips", "rituals", "drops", "decks", "duelists", "text", "font",
    "chest_overflow", "terrain_bonus", "trap_thresholds", "equip_bonus_default", "passwords", "starter",
    "starter_pools", "title", "menu", "limits", "guardian_stars", "attributes", "packs", "pack_shop",
    "card_text_colors", "card_layout", "palette_ramps", "ui", "tower", "card_types", "fields", "models",
};

/* How many letters to add, remove or change to turn one word into the
 * other, for "did you mean": 99 for words too long to bother with. */
static int distance(const char *a, const char *b)
{
    int row[33], i, j, length_a = (int)strlen(a), length_b = (int)strlen(b);
    if (length_a > 32 || length_b > 32) return 99;
    for (j = 0; j <= length_b; j++) row[j] = j;
    for (i = 1; i <= length_a; i++) {
        int diagonal = row[0];
        row[0] = i;
        for (j = 1; j <= length_b; j++) {
            int above = row[j], best = diagonal + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            if (above + 1 < best) best = above + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diagonal = above;
            row[j] = best;
        }
    }
    return row[length_b];
}

static void check_keys(Mod *mod, const JsonValue *root)
{
    static const char *const booleans[] = {"enabled", "restart"};
    const JsonValue *member;
    unsigned i;
    for (member = Json_At(root, 0); member; member = Json_Next(member)) {
        const char *name = Json_Name(member), *closest = NULL;
        int best = 3;   /* at most two letters off, to be a likely typo */
        for (i = 0; i < sizeof(manifest_keys) / sizeof(manifest_keys[0]); i++) {
            int apart;
            if (!strcmp(name, manifest_keys[i])) break;
            apart = distance(name, manifest_keys[i]);   /* letter case counts as no distance: "Name" */
            if (apart < best) { best = apart; closest = manifest_keys[i]; }
        }
        if (i < sizeof(manifest_keys) / sizeof(manifest_keys[0])) {
            /* Only the first of a key is read: two "fusions" lists lose the second. */
            if (Json_Member(root, name) != member) warn(mod, 1, "key '%s' appears twice; only the first is read", name);
            continue;
        }
        if (closest) warn(mod, 1, "unknown key '%s' (did you mean '%s'?)", name, closest);
        else warn(mod, 1, "unknown key '%s'", name);
    }
    for (i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++) {
        const JsonValue *value = Json_Member(root, booleans[i]);
        if (value && Json_TypeOf(value) != JSON_BOOL) warn(mod, 1, "\"%s\" should be true or false", booleans[i]);
    }
}

/* mod.json, read into the record. Returns 0 when it is not a mod at all. */
static int read_manifest(Mod *mod, const char *directory, const char *origin)
{
    char path[PATH_MAX_], error[128];
    const JsonValue *root;
    const char *text;
    if (snprintf(path, sizeof(path), "%s/mod.json", directory) >= (int)sizeof(path)) return 0;
    if (access(path, R_OK)) return 0;
    memset(mod, 0, sizeof(*mod));
    copy_text(mod->directory, sizeof(mod->directory), directory);
    mod->origin = origin;
    mod->manifest = Json_ParseFile(path, error, sizeof(error));
    root = Json_Root(mod->manifest);
    if (!root || Json_TypeOf(root) != JSON_OBJECT) {
        const char *slash = strrchr(directory, '/');
        copy_text(mod->id, sizeof(mod->id), slash ? slash + 1 : directory);
        copy_text(mod->name, sizeof(mod->name), mod->id);
        mod->broken = 1;
        note(mod, "mod.json %s", root ? "is not an object" : error);
        return 1;
    }
    text = Json_String(Json_Member(root, "id"), NULL);
    if (!text || !*text) {
        const char *slash = strrchr(directory, '/');
        text = slash ? slash + 1 : directory;
    }
    if (strlen(text) >= sizeof(mod->id) || !*text || strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(text)) {
        copy_text(mod->id, sizeof(mod->id), strrchr(directory, '/') + 1);
        mod->broken = 1;
        note(mod, "id must be 1-63 letters, digits, hyphens or underscores");
    } else copy_text(mod->id, sizeof(mod->id), text);
    copy_text(mod->name, sizeof(mod->name), Json_String(Json_Member(root, "name"), mod->id));
    /* The object for this game's target (object_loader.h): "libraries"
     * names it outright, else it comes from "library", as it is written (or
     * with ".o") for the 32-bit games, whose one object serves Linux and
     * Windows, and as <library>.<target>.o for the others (its ".o" taken
     * off first). A mod with "libraries" and no "library" is a code mod
     * too: without this target's entry it looks for <id>.<target>.o. */
    {
        const JsonValue *libraries = Json_Member(root, "libraries");
        const char *named = Json_String(Json_Member(libraries, OBJECT_LOADER_TARGET), NULL);
        text = Json_String(Json_Member(root, "library"), NULL);
        if (named && *named) text = named;
        else if ((!text || !*text) && Manifest_HasCode(root)) text = mod->id;
        if (text && *text) {
            if (!Paths_Contained(text)) {
                mod->broken = 1;
                note(mod, "\"library\": %s is outside the mod", text);
            } else if (named && *named) {
                copy_text(mod->library, sizeof(mod->library), text);
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
            } else if (strchr(text, '.')) {
                /* The macOS game links a dylib (build_mod.py --target macos):
                 * "x.o" means x.dylib; a name with another extension is kept. */
                size_t length = strlen(text);
                if (length >= 2 && !strcmp(text + length - 2, ".o"))
                    snprintf(mod->library, sizeof(mod->library), "%.*s.dylib", (int)(length - 2), text);
                else
                    copy_text(mod->library, sizeof(mod->library), text);
            } else {
                snprintf(mod->library, sizeof(mod->library), "%s.dylib", text);
#else
            } else if (strcmp(OBJECT_LOADER_TARGET, "i386")) {
                size_t stem = strlen(text);
                if (stem > 2 && !strcmp(text + stem - 2, ".o")) stem -= 2;
                snprintf(mod->library, sizeof(mod->library), "%.*s.%s.o", (int)stem, text, OBJECT_LOADER_TARGET);
            } else if (strchr(text, '.')) {
                copy_text(mod->library, sizeof(mod->library), text);
            } else {
                snprintf(mod->library, sizeof(mod->library), "%s.o", text);   /* one object for both 32-bit systems */
#endif
            }
        }
    }
    mod->restart = Json_Bool(Json_Member(root, "restart"), 0);
    mod->default_enabled = Json_Bool(Json_Member(root, "enabled"), 0);
    text = Json_String(Json_Member(root, "textures"), NULL);
    if (text && *text) {
        if (!Paths_Contained(text)) {
            mod->broken = 1;
            note(mod, "\"textures\": %s is outside the mod", text);
        } else if (snprintf(mod->textures, sizeof(mod->textures), "%s/%s", directory, text) >= (int)sizeof(mod->textures)) {
            mod->broken = 1;
            note(mod, "\"textures\": %s is too long", text);
        }
    }
    mod->assets = Json_Member(root, "assets");
    if (mod->assets && Json_TypeOf(mod->assets) == JSON_STRING) {
        /* "assets": "<directory>" ships the pictures instead of listing
         * them; each PNG's path under it is the name it replaces. */
        text = Json_String(mod->assets, "");
        mod->assets = NULL;
        if (!*text) {
            mod->broken = 1;
            note(mod, "\"assets\" names no directory");
        } else if (!Paths_Contained(text)) {
            mod->broken = 1;
            note(mod, "\"assets\": %s is outside the mod", text);
        } else if (snprintf(mod->asset_dir, sizeof(mod->asset_dir), "%s/%s", directory, text) >=
                   (int)sizeof(mod->asset_dir)) {
            mod->broken = 1;
            note(mod, "\"assets\": %s is too long", text);
        }
    } else if (mod->assets && Json_TypeOf(mod->assets) != JSON_OBJECT) {
        mod->broken = 1;
        note(mod, "\"assets\" is not an object or the name of a directory");
    }
    mod->data = Json_Member(root, "data");
    if (mod->data && Json_TypeOf(mod->data) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"data\" is not an array");
    }
    mod->audio = Json_Member(root, "audio");
    if (mod->audio && Json_TypeOf(mod->audio) != JSON_OBJECT) {
        mod->broken = 1;
        note(mod, "\"audio\" is not an object");
    }
    mod->cards = Json_Member(root, "cards");
    if (mod->cards && Json_TypeOf(mod->cards) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"cards\" is not an array");
    }
    mod->models = Json_Member(root, "models");
    if (mod->models && Json_TypeOf(mod->models) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"models\" is not an array");
    }
    /* Data overrides change what the game loaded on its way up, so they are
     * only whole while the game starts with them in place; the cards a mod
     * adds are counted once, when the game starts. */
    if (Json_Count(mod->data)) mod->restart = Json_Bool(Json_Member(root, "restart"), 1);
    for (int i = 0; i < Json_Count(mod->data); i++) {
        const JsonValue *entry = Json_At(mod->data, i);
        if (Json_Member(entry, "file") && Json_Member(entry, "replace")) mod->restart = 1;
    }
    if (Json_Count(mod->cards) || Json_Count(mod->models)) mod->restart = 1;
    {   /* So are the rule tables (src/pc/cards/tables.c), the starter decks
         * (src/pc/cards/starter.c) and a translation (src/pc/text): all are
         * read once, at startup. */
        static const char *const tables[] = {"fusions", "equips", "rituals", "drops", "decks", "duelists",
                                             "text", "font", "terrain_bonus", "trap_thresholds",
                                             "chest_overflow", "passwords", "starter", "starter_pools",
                                             "limits", "guardian_stars", "attributes", "packs", "pack_shop",
                                             "palette_ramps", "tower", "fields"};
        for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
            const JsonValue *value = Json_Member(root, tables[t]);
            /* "text": "text.txt" is one file named as a string. */
            if (Json_Count(value) || *Json_String(value, "")) mod->restart = 1;
        }
        if (Json_Member(root, "equip_bonus_default")) mod->restart = 1;   /* a number */
    }
    {   /* A roster need not be in the manifest at all: four folders beside it,
         * a file to a duelist (pc/free_duel/duelists.h). They are read once at
         * startup like the keys above, so a mod that carries only folders needs
         * a restart just the same -- and, having no key, would otherwise look
         * like a mod that changes nothing. */
        static const char *const folders[] = {"duelists", "decks", "drops", "portraits"};
        for (size_t f = 0; f < sizeof(folders) / sizeof(folders[0]); f++) {
            char folder[PATH_MAX_];
            if (snprintf(folder, sizeof(folder), "%s/%s", mod->directory, folders[f]) >= (int)sizeof(folder))
                continue;
            if (!access(folder, R_OK)) mod->restart = 1;
        }
    }
    {   /* The key this mod's choice was stored under before it was a mod. */
        const char *legacy = Json_String(Json_Member(root, "legacy_setting"), NULL);
        char key[256];
        int fallback = mod->default_enabled;
        if (legacy && *legacy) fallback = Settings_GetNamed(legacy, fallback);
        if (setting_key(key, sizeof(key), mod->id, NULL)) fallback = Settings_GetNamed(key, fallback);
        mod->enabled = environment_choice(mod->id, fallback) != 0;
    }
    check_keys(mod, root);
    return 1;
}

static int by_id(const char *id)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (!strcmp(mods[i].id, id)) return i;
    }
    return -1;
}

static int scan(const char *root, const char *origin)
{
    char path[PATH_MAX_];
    char **names = NULL;
    int count = 0, room = 0, i;
    unsigned char here[MODS_MAX] = {0};   /* the mods this directory has given so far */
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (!directory) return -1;   /* errno is opendir's */
    /* Read the names first and sort them: readdir's order is the file
     * system's, and the window should not shuffle between launches. */
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        if (count == room) {
            char **grown = realloc(names, (size_t)(room = room ? room * 2 : 16) * sizeof(*names));
            if (!grown) break;
            names = grown;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(directory);
    for (i = 1; i < count; i++) {   /* an insertion sort: a handful of names */
        char *name = names[i];
        int at = i;
        while (at > 0 && strcmp(names[at - 1], name) > 0) { names[at] = names[at - 1]; at--; }
        names[at] = name;
    }
    for (i = 0; i < count; i++) {
        Mod candidate;
        int existing;
        if (snprintf(path, sizeof(path), "%s/%s", root, names[i]) >= (int)sizeof(path)) continue;
        if (!read_manifest(&candidate, path, origin)) continue;
        existing = by_id(candidate.id);
        if (existing >= 0 && here[existing]) {
            /* Two folders of one directory with one id: the first, in the
             * sorted order, is kept, whatever order the disk lists them. */
            warn(&mods[existing], 1, "%s was left out: same id as %s", path, mods[existing].directory);
            Json_Free(candidate.manifest);
        } else if (existing >= 0) {
            /* A later root wins: the player's mods folder over the release's
             * own mods, the user directory's over both. */
            say("%s in %s replaces the one in %s", candidate.id, path, mods[existing].directory);
            Json_Free(mods[existing].manifest);
            candidate.enabled = mods[existing].enabled;
            mods[existing] = candidate;
            here[existing] = 1;
        } else if (mod_count < MODS_MAX) {
            here[mod_count] = 1;
            mods[mod_count++] = candidate;
        } else {
            Json_Free(candidate.manifest);
            fprintf(stderr, "memories-pc: more than %d mods; %s was skipped\n", MODS_MAX, candidate.id);
        }
    }
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
    return 0;
}

/* --- The player's own mods folder ---------------------------------------
 * One more folder the game reads, between the release's mods/ and the user
 * directory's: named in mods-folder.txt (user directory), or by
 * MEMORIES_MODS_FOLDER, which wins over the file. The game only reads it:
 * imports, the HD pack, Remove and a mod's data stay in the user directory. */

#define FOLDER_FILE "mods-folder.txt"
static char folder_problem[PATH_MAX_ + 200];

#if defined(_WIN32) || defined(__APPLE__)   /* the file system does not tell case */
#define SAME_CHAR(a, b) (tolower((unsigned char)(a)) == tolower((unsigned char)(b)))
#else
#define SAME_CHAR(a, b) ((a) == (b))
#endif

/* A folder as the file or the environment writes it: no byte order mark, no
 * spaces or line end around it, no double quotes around it (Explorer's "Copy
 * as path" adds them), '/' for '\' on Windows, no trailing separator (but a
 * root's). */
static void tidy_folder(char *text)
{
    size_t length, start = 0;
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) start = 3;
    while (text[start] == ' ' || text[start] == '\t') start++;
    length = strlen(text + start);
    memmove(text, text + start, length + 1);
    while (length && strchr(" \t\r\n", text[length - 1])) text[--length] = '\0';
    if (length >= 2 && text[0] == '"' && text[length - 1] == '"') {
        memmove(text, text + 1, length - 2);
        text[length -= 2] = '\0';
        while (length && strchr(" \t\r\n", text[length - 1])) text[--length] = '\0';
    }
#ifdef _WIN32
    for (start = 0; start < length; start++) if (text[start] == '\\') text[start] = '/';
#endif
    while (length > 1 && text[length - 1] == '/' && !(length == 3 && text[1] == ':')) text[--length] = '\0';
}

/* Whether `path` is `root` or lies under it (both canon_folder's). */
static int folder_inside(const char *path, const char *root)
{
    size_t length = strlen(root), i;
    if (!length) return 0;
    for (i = 0; i < length; i++) if (!SAME_CHAR(path[i], root[i])) return 0;
    return !path[length] || path[length] == '/' || root[length - 1] == '/';
}

/* "." and ".." parts and doubled separators taken out of a tidy, absolute path. */
static void flatten_folder(char *path)
{
    char out[PATH_MAX_];
    size_t used = 0, root = 0;
    const char *at;
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':') root = 2;
#endif
    memcpy(out, path, root);
    used = root;
    at = path + root;
    if (*at == '/') out[used++] = '/';
    root = used;
    for (;;) {
        size_t length;
        while (*at == '/') at++;
        length = strcspn(at, "/");
        if (!length) break;
        if (length == 1 && at[0] == '.') {
            /* this folder */
        } else if (length == 2 && at[0] == '.' && at[1] == '.') {
            while (used > root && out[used - 1] == '/') used--;
            while (used > root && out[used - 1] != '/') used--;
        } else {
            if (used > root && out[used - 1] != '/') out[used++] = '/';
            memcpy(out + used, at, length);
            used += length;
        }
        at += length;
    }
    while (used > root + 1 && out[used - 1] == '/') used--;
    out[used] = '\0';
    memcpy(path, out, used + 1);
}

#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetFullPathNameW(const wchar_t *, unsigned long, wchar_t *, wchar_t **);
__declspec(dllimport) unsigned long __stdcall GetLongPathNameW(const wchar_t *, wchar_t *, unsigned long);
#endif

/* The folder `path` names, spelled one way: absolute, without "." and ".."
 * (from the current directory if it is not absolute), and, where it exists,
 * with its links followed (realpath). On Windows: without the prefix of a
 * "\\?\" or "\\.\" path, with "\\?\UNC\" as "\\", and, where it exists, with
 * its long names (GetLongPathNameW, for an 8.3 short one); a junction or a
 * link is not followed there, nor is a difference in the case of a non-ASCII
 * letter. For comparing two folders. -1 if it does not fit or is not a name. */
static int canon_folder(const char *path, char *out, size_t size)
{
    char work[PATH_MAX_];
#ifdef _WIN32
    wchar_t *wide, full[PATH_MAX_], longer[PATH_MAX_];
    char plain[PATH_MAX_], *narrow;
    unsigned long length;
    if (snprintf(plain, sizeof(plain), "%s", path) >= (int)sizeof(plain)) return -1;
    tidy_folder(plain);   /* '/' for '\' */
    if (!strncmp(plain, "//?/UNC/", 8)) memmove(plain + 2, plain + 8, strlen(plain + 8) + 1);
    else if (!strncmp(plain, "//?/", 4) || !strncmp(plain, "//./", 4)) memmove(plain, plain + 4, strlen(plain + 4) + 1);
    if (!(wide = Memories_Utf8ToWide(plain))) return -1;
    length = GetFullPathNameW(wide, PATH_MAX_, full, NULL);
    free(wide);
    if (!length || length >= PATH_MAX_) return -1;
    length = GetLongPathNameW(full, longer, PATH_MAX_);
    if (length && length < PATH_MAX_) memcpy(full, longer, ((size_t)length + 1) * sizeof(wchar_t));
    if (!(narrow = Memories_WideToUtf8(full))) return -1;
    length = (unsigned long)snprintf(work, sizeof(work), "%s", narrow);
    free(narrow);
    if (length >= sizeof(work)) return -1;
#else
    char whole[PATH_MAX_];
    size_t cut;
    if (path[0] == '/') {
        if (snprintf(whole, sizeof(whole), "%s", path) >= (int)sizeof(whole)) return -1;
    } else {
        char here[PATH_MAX_];
        if (!getcwd(here, sizeof(here)) || snprintf(whole, sizeof(whole), "%s/%s", here, path) >= (int)sizeof(whole)) return -1;
    }
    /* The longest part that exists, with its links followed, and the rest as it is. */
    for (cut = strlen(whole);; ) {
        char head[PATH_MAX_], *real;
        memcpy(head, whole, cut);
        head[cut] = '\0';
        real = realpath(cut ? head : "/", NULL);
        if (real) {
            int fits = snprintf(work, sizeof(work), "%s/%s", real, whole + cut) < (int)sizeof(work);
            free(real);
            if (!fits) return -1;
            break;
        }
        while (cut > 0 && whole[cut - 1] != '/') cut--;
        while (cut > 0 && whole[cut - 1] == '/') cut--;
    }
#endif
    tidy_folder(work);
    flatten_folder(work);
    return (size_t)snprintf(out, size, "%s", work) >= size ? -1 : 0;
}

/* 1 when the game reads or writes `path` already, as one of its own folders (0
 * when not, -1 when it cannot be told: the name does not fit or is not text):
 * the release's mods/, MEMORIES_MODS_DIR, the user directory (its mods/, the
 * downloads and everything else the game keeps there) or a folder in one.
 * `path` is as the game would open it (absolute_folder's). Links are followed
 * where the folder exists. */
static int folder_is_own(const char *path)
{
    char own[PATH_MAX_], root[PATH_MAX_], canon[PATH_MAX_];
    const char *named = getenv("MEMORIES_MODS_DIR");
    if (canon_folder(path, canon, sizeof(canon))) return -1;
    if (!Paths_Program(own, sizeof(own), "mods") && !canon_folder(own, root, sizeof(root)) && folder_inside(canon, root))
        return 1;
    if (named && *named && snprintf(own, sizeof(own), "%s", named) < (int)sizeof(own)) {
        tidy_folder(own);
        if (!canon_folder(own, root, sizeof(root)) && folder_inside(canon, root)) return 1;
    }
    if (!canon_folder(Paths_UserDir(), root, sizeof(root)) && folder_inside(canon, root)) return 1;
    return 0;
}

/* A tidy folder as an absolute one: a path that is not absolute is from the
 * program directory (a portable pack says "mods-extra"). -1 if it does not fit. */
static int absolute_folder(const char *line, char *path, size_t size)
{
    int absolute = line[0] == '/';
#ifdef _WIN32
    if (isalpha((unsigned char)line[0]) && line[1] == ':') absolute = 1;
#endif
    if (snprintf(path, size, absolute ? "%s%s" : "%s/%s", absolute ? "" : Paths_ProgramDir(), line) >= (int)size)
        return -1;
    tidy_folder(path);
    return 0;
}

int Mods_Folder(char *out, size_t size)
{
    char line[PATH_MAX_], path[PATH_MAX_];
    const char *named = getenv("MEMORIES_MODS_FOLDER");
    line[0] = '\0';
    if (named) {   /* set, even to nothing: the file is not read */
        if (snprintf(line, sizeof(line), "%s", named) >= (int)sizeof(line)) return -1;
        tidy_folder(line);
    } else {
        FILE *file;
        if (Paths_User(path, sizeof(path), FOLDER_FILE) || !(file = fopen(path, "rb"))) return -1;
        /* The first line that says anything; one too long for `line` is no folder. */
        while (!line[0] && fgets(line, sizeof(line), file)) {
            if (!strchr(line, '\n') && strlen(line) == sizeof(line) - 1) { line[0] = '\0'; break; }
            tidy_folder(line);
        }
        fclose(file);
    }
    if (!line[0] || absolute_folder(line, path, sizeof(path))) return -1;
    if ((size_t)snprintf(out, size, "%s", path) >= size) return -1;
    return 0;
}

/* A choice saved (or cleared) is not what this run does while MEMORIES_MODS_FOLDER
 * or MEMORIES_MODS_DIR is set: `why` says so, and the call still succeeds (0). */
static int overridden(char *why, size_t why_size, int cleared)
{
    const char *dir = getenv("MEMORIES_MODS_DIR");
    const char *did = cleared ? "Cleared" : "Saved";
    if (dir && *dir)
        snprintf(why, why_size, "%s, but MEMORIES_MODS_DIR is set, so no other mods folder is read until it is unset.", did);
    else if (getenv("MEMORIES_MODS_FOLDER"))
        snprintf(why, why_size, "%s, but MEMORIES_MODS_FOLDER is set, so it is used %s until it is unset.", did,
                 cleared ? "all the same" : "instead");
    return 0;
}

int Mods_SetFolder(const char *path, char *why, size_t why_size)
{
    char saved[PATH_MAX_], temporary[PATH_MAX_], text[PATH_MAX_], resolved[PATH_MAX_], reason[1200];
    FILE *file;
    size_t length;
    int written, own;
    if (why_size) why[0] = '\0';
    if (snprintf(text, sizeof(text), "%s", path ? path : "") >= (int)sizeof(text)) {
        snprintf(why, why_size, "That folder's name is too long.");
        return -1;
    }
    tidy_folder(text);
    if (strpbrk(text, "\r\n")) {
        snprintf(why, why_size, "A folder's name cannot hold a line break.");
        return -1;
    }
    if (isalpha((unsigned char)text[0]) && text[1] == ':' && text[2] != '/' && text[2] != '\\') {
        snprintf(why, why_size, "Give the folder's full path, such as C:\\Mods.");   /* "C:" and "C:foo" are the drive's current folder */
        return -1;
    }
    if (text[0] && absolute_folder(text, resolved, sizeof(resolved))) {
        snprintf(why, why_size, "That folder's name is too long.");
        return -1;
    }
    if (text[0] && (own = folder_is_own(resolved))) {
        if (own < 0)
            snprintf(why, why_size, "That folder's name cannot be used as a path (it is too long, or not valid text).");
        else
            snprintf(why, why_size, "That is one of the game's own folders (the release's mods, or your user folder "
                     "with the mods, saves and downloads in it), which the game reads or writes already. Choose another.");
        return -1;
    }
    Paths_WriteBegin();
    if (Paths_User(saved, sizeof(saved), FOLDER_FILE) || Paths_User(temporary, sizeof(temporary), FOLDER_FILE ".tmp")) {
        snprintf(saved, sizeof(saved), "%s/" FOLDER_FILE, Paths_UserDir());
        snprintf(why, why_size, "Could not save your mods folder to %s", Paths_WriteError(reason, sizeof(reason), saved));
        return -1;
    }
    if (!text[0]) {   /* no folder of the player's: the file goes */
        if (remove(saved) && errno != ENOENT) {
            snprintf(why, why_size, "Could not stop using the mods folder: %s",
                     Paths_WriteError(reason, sizeof(reason), saved));
            return -1;
        }
        return overridden(why, why_size, 1);
    }
    length = strlen(text);
    if (!(file = fopen(temporary, "wb"))) {
        snprintf(why, why_size, "Could not save your mods folder to %s", Paths_WriteError(reason, sizeof(reason), saved));
        return -1;
    }
    written = fwrite(text, 1, length, file) == length && fputc('\n', file) != EOF;
    if (fclose(file)) written = 0;
    if (!written || rename(temporary, saved)) {
        Paths_WriteError(reason, sizeof(reason), saved); /* before remove() changes the reason */
        remove(temporary);
        snprintf(why, why_size, "Could not save your mods folder to %s", reason);
        return -1;
    }
    return overridden(why, why_size, 0);
}

const char *Mods_FolderProblem(void) { return folder_problem; }

/* The scan of the player's folder, between the release's mods and the user
 * directory's. One that cannot be read is left out, and only
 * Mods_FolderProblem and the log say so. */
static void scan_folder(int ignored)
{
    char path[PATH_MAX_];
    folder_problem[0] = '\0';
    if (Mods_Folder(path, sizeof(path))) return;
    if (ignored) {
        snprintf(folder_problem, sizeof(folder_problem), "MEMORIES_MODS_DIR is set, so the mods folder %s is not read.",
                 path);
    } else if (folder_is_own(path)) {
        snprintf(folder_problem, sizeof(folder_problem),
                 folder_is_own(path) < 0 ? "%s cannot be used as a path (it is too long, or not valid text)."
                                         : "%s is one of the game's own folders, which it reads or writes already.",
                 path);
    } else if (scan(path, "folder")) {
        int error = errno;
        if (error == ENOENT) snprintf(folder_problem, sizeof(folder_problem), "%s does not exist.", path);
        else if (error == ENOTDIR) snprintf(folder_problem, sizeof(folder_problem), "%s is not a folder.", path);
        else snprintf(folder_problem, sizeof(folder_problem), "%s could not be read: %s.", path, strerror(error));
    }
    if (folder_problem[0] && !ignored) fprintf(stderr, "memories-pc: mods folder: %s\n", folder_problem);
}

/* Turn a mod on or off for real: its library, its overrides, its hook. */
static int (*texture_pack_load)(const char *directory, unsigned rank, int (*part)(const char *, void *),
                                void *context, char *problems, size_t size);
static int (*assets_load)(const char *, const JsonValue *, unsigned,
                          int (*)(const char *, void *), void *, char *, size_t);
static int (*asset_folder_load)(const char *, unsigned, int (*)(const char *, void *), void *, char *, size_t);

void Mods_SetAssets(int (*load)(const char *, const JsonValue *, unsigned,
                                int (*)(const char *, void *), void *, char *, size_t),
                    int (*folder)(const char *, unsigned, int (*)(const char *, void *), void *, char *, size_t))
{
    assets_load = load;
    asset_folder_load = folder;
}

static void (*texture_pack_unload)(void);
static unsigned texture_rank;   /* the highest rank a loaded pack has */
static int (*audio_load)(int, const char *, const char *, const JsonValue *, char *, size_t);
static void (*audio_unload)(int);

void Mods_SetAudio(int (*load)(int mod, const char *id, const char *directory, const struct JsonValue *audio,
                               char *error, size_t size),
                   void (*unload)(int mod))
{
    audio_load = load;
    audio_unload = unload;
}

void Mods_SetAudioClips(int (*add)(int mod, const char *id, const int16_t *samples, size_t frames, int channels,
                                   unsigned rate),
                        int (*play)(int mod, int handle, int volume, int pan), void (*release)(int mod, int handle))
{
    clip_add = add;
    clip_play = play;
    clip_free = release;
}

void Mods_SetTexturePack(int (*load)(const char *directory, unsigned rank,
                                     int (*part)(const char *setting, void *context), void *context,
                                     char *problems, size_t size),
                         void (*unload)(void))
{
    texture_pack_load = load;
    texture_pack_unload = unload;
}

/* An entry's "setting" (a pack's image, a "text" or "font" file): the value
 * of that declared setting of the mod, as the mod was applied with it
 * (Mods_RuntimeOption), in `value`; 0 when the mod declares none of that key. */
static int setting_value(int index, const char *setting, int *value)
{
    int i;
    for (i = 0; i < Mods_OptionCount(index); i++) {
        if (!strcmp(setting, Json_String(Json_Member(Mods_Option(index, i), "key"), ""))) {
            *value = Mods_RuntimeOption(index, i);
            return 1;
        }
    }
    return 0;
}

/* Whether the pack's part is on, -1 when the mod declares no such setting. */
static int texture_part(const char *setting, void *context)
{
    int value;
    return setting_value((int)((Mod *)context - mods), setting, &value) ? value != 0 : -1;
}

/* An entry's "setting", with "value" for one choice of a "choice" setting:
 * 0 when that declared setting leaves it out. A setting the mod lacks, or a
 * "value" that is not a number, is warned of and the entry used. `key` and
 * `name` say where the entry is. */
static int entry_used(int index, const JsonValue *entry, const char *key, const char *name)
{
    const JsonValue *setting = Json_TypeOf(entry) == JSON_OBJECT ? Json_Member(entry, "setting") : NULL;
    const JsonValue *only = setting ? Json_Member(entry, "value") : NULL;
    const char *wanted = Json_String(setting, "");
    int current = 0;
    if (!setting) return 1;
    if (!*wanted || !setting_value(index, wanted, &current)) {
        warn(&mods[index], 0, "\"%s\": %s names a setting the mod does not declare (%s); used", key, name,
             *wanted ? wanted : "not a key");
    } else if (only && Json_TypeOf(only) != JSON_NUMBER) {
        warn(&mods[index], 0, "\"%s\": %s's \"value\" is not a number; used", key, name);
    } else if (only ? current != (int)Json_Number(only, 0) : !current) {
        say("%s: \"%s\": %s left out, setting %s is %d", Mods_Id(index), key, name, wanted, current);
        return 0;
    }
    return 1;
}

int Mods_EntryUsed(const char *id, const JsonValue *entry, const char *where)
{
    int index = by_id(id);
    return index < 0 || entry_used(index, entry, where, "the entry");
}

int Mods_File(int index, const char *key, int entry_index, char *path, size_t size, const char **name)
{
    const JsonValue *value = Json_Member(Mods_Manifest(index), key);
    const JsonValue *entry = Json_TypeOf(value) == JSON_ARRAY ? Json_At(value, entry_index) : entry_index ? NULL : value;
    int object = Json_TypeOf(entry) == JSON_OBJECT;
    const char *file = Json_String(object ? Json_Member(entry, "file") : entry, NULL);
    if (!entry) return 0;
    path[0] = '\0';
    *name = file ? file : "";
    if (!entry_used(index, entry, key, *name)) return 1;
    if (!file || !*file || !Paths_Contained(file) ||
        snprintf(path, size, "%s/%s", Mods_Directory(index), file) >= (int)size) {
        Mods_Note(Mods_Id(index), "\"%s\": %s is not a file in the mod", key, *name);
        path[0] = '\0';
    }
    return 1;
}

/* The texture packs of the active mods, with `with` about to be one and
 * without `without`, in the mods' load order (Mods_Order): a pack's rank
 * follows its place there, so where two packs read the same words the same
 * way the later one wins, whichever the player applied first. A pack that
 * comes last, as each does while the game starts, goes on top of those
 * loaded; otherwise they are all loaded again. Returns what loading `with`
 * returned (-1 it could not load), its problems in `problems`. */
static int has_images(const Mod *mod) { return mod->textures[0] || mod->assets || mod->asset_dir[0]; }

static int load_mod_images(Mod *mod, char *problems, size_t size)
{
    int loaded = 0, got;
    char error[STATUS_MAX] = "";
    if (problems && size) problems[0] = '\0';
    if (mod->textures[0]) {
        if (!texture_pack_load) return -1;
        loaded = texture_pack_load(mod->textures, ++texture_rank, texture_part, mod, problems, size);
        if (loaded < 0) return loaded;
    }
    if (mod->assets || mod->asset_dir[0]) {
        if (!assets_load || !asset_folder_load) {
            if (problems && size) snprintf(problems, size, "this build has no named asset replacements");
            return -1;
        }
        /* Direct images override the same mod's pack; the next mod still wins. */
        got = mod->asset_dir[0]
            ? asset_folder_load(mod->asset_dir, ++texture_rank, texture_part, mod, error, sizeof(error))
            : assets_load(mod->directory, mod->assets, ++texture_rank, texture_part, mod, error, sizeof(error));
        if (problems && size && error[0]) {
            size_t used = strlen(problems);
            if (used < size) snprintf(problems + used, size - used, "%s%s", used ? "; " : "", error);
        }
        if (got < 0) return got;
        loaded += got;
    }
    return loaded;
}

static int load_texture_packs(int with, int without, char *problems, size_t size)
{
    int wanted[MODS_MAX], order[MODS_MAX], i, n, loaded = with < 0 ? 0 : -1;
    char ignored[STATUS_MAX];
    for (i = 0; i < mod_count; i++)
        wanted[i] = i != without && has_images(&mods[i]) && (mods[i].active || i == with);
    n = Mods_Order(wanted, order, ignored, sizeof(ignored));
    if (n < 0) for (n = 0; order[n] >= 0; n++) continue;   /* a cycle: the packs that could be placed */
    if (with >= 0 && n > 0 && order[n - 1] == with) {
        return load_mod_images(&mods[with], problems, size);
    }
    texture_pack_unload();
    texture_rank = 0;
    for (i = 0; i < n; i++) {
        int got = load_mod_images(&mods[order[i]], order[i] == with ? problems : NULL, size);
        if (order[i] == with) loaded = got;
    }
    return loaded;
}

static void activate_once(int index, int on);

/* Function hooks follow what is applied, however activation ended. */
static void activate(int index, int on)
{
    activate_once(index, on);
    Hooks_Relink();
}

static void activate_once(int index, int on)
{
    Mod *mod = &mods[index];
    if (on == mod->active) return;
    change_count++;   /* before it can fail: a rollback is a change too */
    if (on) {
        int option_count = Mods_OptionCount(index);
        if (option_count && !mod->runtime_options) {
            mod->runtime_options = malloc((size_t)option_count * sizeof(int));
            if (!mod->runtime_options) {
                mod->failed = 1; note(mod, "out of memory for settings"); drop_overrides(index); return;
            }
        }
#ifdef MEMORIES_NO_CODE_MODS
        /* A game built without code mods (none is now: the arm64 game links
         * them since Mod SDK M2). The mod stays off with the reason beside it,
         * and its choice and the other mods' Apply are left alone: it is not
         * a broken mod. */
        if (!mod->broken && mod->library[0]) {
            note(mod, "is a code mod, which this game does not load yet");
            drop_overrides(index);
            return;
        }
#else
        /* A 64-bit game loads the mod's object for its own target
         * (<library>.x86_64-windows.o). A mod made before there was one has
         * only the 32-bit object: it stays off with the reason beside it, as
         * above, and is not a broken mod. The macOS game, which has no 32-bit
         * one to send the player to, reports a missing dylib as broken. On a
         * phone there is no other game to point to either: the note says
         * which object is missing. Both say how one is made (notes/modding.md,
         * "Building one"). */
#if !(defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED))
        if (strcmp(OBJECT_LOADER_TARGET, "i386") && !mod->broken && mod->library[0] && !mod->object.image &&
            !mod->object.native_handle && !library_present(mod)) {
#if defined(__ANDROID__) && defined(MEMORIES_TRANSLATED)
            /* A mod made for the fixed-memory game has <library>.aarch64.o:
             * the same code mod, which needs building again for this one. */
            note(mod, old_android_object(mod) ? "was built for the fixed-memory Android game: ask the mod's author to "
                                                "rebuild it (build_mod.py --target aarch64-translated); it has no %s"
                                              : "needs an Android build of this mod: it has no %s (ask the mod's "
                                                "author to rebuild it with build_mod.py --target aarch64-translated)",
                 mod->library);
#elif defined(__ANDROID__)
            note(mod, "needs an Android build of this mod: it has no %s (rebuild it with build_mod.py using clang)",
                 mod->library);
#else
            note(mod, "needs a 64-bit build of this mod (%s): the one it has is for the 32-bit game, which is the one "
                      "to play it with, or rebuild it with build_mod.py using clang", mod->library);
#endif
            drop_overrides(index);
            return;
        }
#endif
#endif
        /* A mod that cannot load keeps the player's choice and its reason:
         * the window shows both, and removing it still works. */
        if (mod->broken || !load_library(mod)) {
            mod->broken = 1;
            drop_overrides(index);
            return;
        }
        if (!mod->data_prepared) copy_text(mod->status, sizeof(mod->status), mod->warnings);
        if (!mod->data_prepared && !apply_overrides(mod, index)) { mod->failed = 1; return; }
        mod->data_prepared = 1;
        if (has_images(mod)) {
            char problems[STATUS_MAX] = "";
            if (!texture_pack_unload || load_texture_packs(index, -1, problems, sizeof(problems)) < 0) {
                mod->failed = 1;
                note(mod, "texture pack could not load%s%s", problems[0] ? ": " : "", problems);
                drop_overrides(index);
                if (texture_pack_unload) load_texture_packs(-1, index, NULL, 0);
                return;
            }
            if (problems[0]) warn(mod, 0, "texture pack: %s", problems);
        }
        if (mod->audio) {
            /* Replacement sounds are decoded now; one that will not decode
             * is skipped with its reason beside the mod, and the rest play. */
            char error[STATUS_MAX];
            int added = audio_load ? audio_load(index, mod->id, mod->directory, mod->audio, error, sizeof(error)) : 0;
            if (!audio_load) warn(mod, 0, "this build has no audio replacement");
            else if (error[0]) warn(mod, 0, "%s", error);
            if (added > 0) say("%s: %d sounds replaced", mod->id, added);
        }
        for (int option = 0; option < option_count; option++) mod->runtime_options[option] = Mods_OptionValue(index, option);
        mod->sequence = ++activation_sequence;
        mod->failed = 0;
        mod->active = 1;
        if (mod->hooks.applied) mod->hooks.applied(1);
    } else {
        drop_overrides(index);
        /* The manifest's sounds, and those its code added (host->sound_add). */
        if ((mod->audio || mod->sounds) && audio_unload) audio_unload(index);
        mod->sounds = 0;
        if (has_images(mod) && texture_pack_unload) {
            /* The packs add up: the others' come back without this one's. */
            load_texture_packs(-1, index, NULL, 0);
        }
        mod->active = 0;
        if (mod->hooks.applied) mod->hooks.applied(0);
    }
}

int Mods_InstallDirectory(char *out, size_t size)
{
    const char *named = getenv("MEMORIES_MODS_DIR");
    if (named && *named) {
        if ((size_t)snprintf(out, size, "%s", named) >= size) return -1;
    } else if (Paths_User(out, size, "mods")) {
        return -1;
    }
    return Paths_MakeDirs(out);
}

/* A mod folder that came while the game runs (Import mod..., import.h).
 * Never put in place before: borrowed strings live until exit, so an old
 * manifest it replaces stays allocated. */
static Mod *at(int index);
static int untouched(const Mod *mod)
{
    return !mod->active && !mod->initialized && !mod->data_prepared && !mod->runtime_options && !mod->sequence &&
           !mod->object.image && !mod->object.native_handle;
}
int Mods_InUse(int index) { return at(index) && !untouched(&mods[index]); }
int Mods_Discover(const char *directory, int *later)
{
    Mod candidate;
    char error[160], key[256];
    int index;
    *later = 0;
    if (!read_manifest(&candidate, directory, "installed")) return -1;
    index = by_id(candidate.id);
    if (index < 0) {
        if (mod_count >= MODS_MAX) {
            Json_Free(candidate.manifest);
            fprintf(stderr, "memories-pc: more than %d mods; %s was skipped\n", MODS_MAX, candidate.id);
            return -1;
        }
        /* New: off until the player turns it on, at this launch and the next. */
        candidate.enabled = 0;
        if (setting_key(key, sizeof(key), candidate.id, NULL)) Settings_SetNamed(key, 0);
        index = mod_count++;
        mods[index] = candidate;
    } else if (untouched(&mods[index])) {
        /* The player's choice is the setting, which read_manifest read. */
        mods[index] = candidate;
    } else {
        /* In place (or once was): its object, data and pictures are the old
         * files' until the game starts again, so a change to it waits for
         * that launch too. */
        Json_Free(candidate.manifest);
        mods[index].restart = 1;
        note(&mods[index], "its files were replaced: the new ones are used from the next launch");
        *later = 1;
        Mods_OverlapsForget();
        return index;
    }
    if (!Mods_CheckManifest(index, error, sizeof(error))) { mods[index].broken = 1; note(&mods[index], "%s", error); }
    Mods_OverlapsForget();
    say("%s: found in %s", mods[index].id, directory);
    return index;
}

int Mods_Forget(int index)
{
    char key[256];
    if (!at(index) || !untouched(&mods[index])) return -1;
    /* Off, in the settings too: a mod with this id imported later starts
     * off, as any new one does. Its order and option values stay, so an
     * updated version comes back with the player's choices. The slot stays
     * (events and state are keyed by index); the list leaves it out. */
    if (setting_key(key, sizeof(key), mods[index].id, NULL)) Settings_SetNamed(key, 0);
    mods[index].enabled = 0;
    mods[index].failed = 0;
    mods[index].broken = 1;
    mods[index].removed = 1;
    mods[index].status[0] = '\0';
    mods[index].warnings[0] = '\0';
    Mods_OverlapsForget();
    say("%s: removed", mods[index].id);
    return 0;
}
int Mods_Removed(int index) { return at(index) && mods[index].removed; }

void Mods_Load(void)
{
    const char *all = getenv("MEMORIES_MODS");
    char path[PATH_MAX_];
    int i, enabled[MODS_MAX], order[MODS_MAX], count, first = !scanned;
    char error[160];
    if (!scanned) {
        const char *named = getenv("MEMORIES_MODS_DIR");
        scanned = 1;
        if (named && *named) {
            scan(named, "installed");
            scan_folder(1);
        } else {
            if (!Paths_Program(path, sizeof(path), "mods")) scan(path, "shipped");
            scan_folder(0);
            if (!Paths_User(path, sizeof(path), "mods")) scan(path, "installed");
        }
        for (i = 0; i < mod_count; i++) {
            if (!Mods_CheckManifest(i, error, sizeof(error))) { mods[i].broken = 1; note(&mods[i], "%s", error); }
        }
        say("%d mods found", mod_count);
    }
    for (i = 0; i < mod_count; i++) {
        /* The settings are the choice, here and after a settings reload.
         * A mod that wants a restart is still put in place at startup: it
         * is only a live change it cannot take. */
        char key[256];
        int want;
        want = mods[i].enabled;
        if (setting_key(key, sizeof(key), mods[i].id, NULL)) want = Settings_GetNamed(key, want);
        want = environment_choice(mods[i].id, want) != 0;
        if (all && (!strcmp(all, "0") || !strcmp(all, "off"))) want = 0;
        mods[i].enabled = want;
        enabled[i] = want;
    }
    for (i = 0; i < mod_count; i++) if (enabled[i] && !Mods_Compatible(i, enabled, error, sizeof(error))) {
        note(&mods[i], "%s", error); enabled[i] = 0;
    }
    count = Mods_Order(enabled, order, error, sizeof(error));
    if (count < 0) {
        /* Only the mods in the cycle, or waiting on it, stay off. */
        int placed[MODS_MAX] = {0};
        for (count = 0; order[count] >= 0; count++) placed[order[count]] = 1;
        for (i = 0; i < mod_count; i++) if (enabled[i] && !placed[i]) { note(&mods[i], "%s", error); enabled[i] = 0; }
    }
    if (first) {
        preparing_data = 1;
        for (i = 0; i < count; i++) {
            int current = order[i];
            if (!enabled[current] || mods[current].broken) continue;
            if (!apply_overrides(&mods[current], current)) mods[current].failed = 1;
            else mods[current].data_prepared = 1;
        }
        preparing_data = 0;
        for (;;) {
            int changed = 0;
            while (!validate_disc_patches()) changed = 1;
            /* Failed preparations and their dependents cannot contribute data. */
            for (i = 0; i < count; i++) {
                int current = order[i];
                if (!enabled[current]) continue;
                if (mods[current].failed || mods[current].broken) enabled[current] = 0;
                if (enabled[current] && !Mods_Compatible(current, enabled, error, sizeof(error))) {
                    note(&mods[current], "%s", error);
                    mods[current].failed = 1;
                    enabled[current] = 0;
                }
                if (!enabled[current]) { drop_overrides(current); changed = 1; }
            }
            if (!changed) break;
        }
        disc_layout_frozen = 1;
    }
    /* After the first load (a settings reload) a mod that wants a restart is
     * only recorded, as Mods_SetEnabled does, and a live one that requires
     * it waits for the same restart. */
    {   /* Applied last, removed first (as Mods_Apply does). */
        int newest[MODS_MAX], j;
        for (i = 0; i < mod_count; i++) {
            for (j = i; j > 0 && mods[newest[j - 1]].sequence < mods[i].sequence; j--) newest[j] = newest[j - 1];
            newest[j] = i;
        }
        for (j = 0; j < mod_count; j++) {
            i = newest[j];
            if (!enabled[i] && (first || !mods[i].restart)) activate(i, 0);
        }
    }
    for (i = 0; i < count; i++) {
        int current = order[i], j, active[MODS_MAX];
        if (!enabled[current] || (first && mods[current].failed)) continue;
        if (!first && mods[current].restart) continue;
        if (!first && !mods[current].active && (j = Mods_WaitsForRestart(current, enabled)) >= 0) {
            note(&mods[current], "waits for a restart: %s, which it requires, is applied at the next launch", mods[j].name);
            continue;
        }
        for (j = 0; j < mod_count; j++) active[j] = mods[j].active;
        active[current] = 1;
        if (!Mods_Compatible(current, active, error, sizeof(error))) {
            note(&mods[current], "%s", error); drop_overrides(current); continue;
        }
        activate(current, 1);
    }
    /* A code/texture initializer can still fail after data preparation.
     * Recheck tail patches and dependencies against the surviving files. */
    for (;;) {
        int changed = 0, active[MODS_MAX];
        while (!validate_disc_patches()) changed = 1;
        for (i = 0; i < mod_count; i++) active[i] = mods[i].active;
        for (i = 0; i < count; i++) {
            int current = order[i];
            if (active[current] && !Mods_Compatible(current, active, error, sizeof(error))) {
                note(&mods[current], "%s", error);
                activate(current, 0);
                mods[current].failed = 1;
                active[current] = 0;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    /* What the startup-only readers (rule tables, translation) see: the mods
     * active once the game has started, in load order. A settings reload
     * leaves it alone, as those readers do not run again. */
    if (first) {
        loaded_count = 0;
        for (i = 0; i < count; i++) if (mods[order[i]].active) loaded[loaded_count++] = order[i];
    }
}

int Mods_LoadedCount(void) { return loaded_count; }
int Mods_Loaded(int index) { return index >= 0 && index < loaded_count ? loaded[index] : -1; }

void Mods_Shutdown(void)
{
    int i;
    /* Every mod's event callbacks go before any shutdown hook runs, and the
     * frame and reset hooks stop: the process is on its way out, and a hook
     * may free what the callbacks use. The caller stops the clock first. */
    shut_down = 1;
    for (i = 0; i < mod_count; i++) Mods_ClearHooks(i);
    for (i = 0; i < mod_count; i++) {
        if (mods[i].initialized && mods[i].hooks.shutdown) mods[i].hooks.shutdown();
    }
}

int Mods_Count(void) { return mod_count; }

static Mod *at(int index)
{
    return index >= 0 && index < mod_count ? &mods[index] : NULL;
}

const char *Mods_Id(int mod) { return at(mod) ? mods[mod].id : ""; }
const char *Mods_Name(int mod) { return at(mod) ? mods[mod].name : ""; }
const char *Mods_Status(int mod) { return at(mod) ? mods[mod].status : ""; }
int Mods_Enabled(int mod) { return at(mod) ? mods[mod].enabled : 0; }
int Mods_RequiresRestart(int mod) { return at(mod) ? mods[mod].restart : 0; }

void Mods_SetEnabled(int mod, int enabled)
{
    char key[256];
    if (!at(mod)) return;
    enabled = enabled != 0;
    /* Unchanged, nothing is written: the choice stored stays, even when an
     * environment variable overrides it for this run. */
    if (mods[mod].enabled == enabled) return;
    if (setting_key(key, sizeof(key), mods[mod].id, NULL)) Settings_SetNamed(key, enabled);
    mods[mod].enabled = enabled;
    /* A mod that could not go in place (a replacement file missing, say) is
     * tried again when the player next applies it; one that cannot load at
     * all stays broken. */
    if (!enabled) mods[mod].failed = 0;
    /* A mod that asks for a restart is only recorded here; the next launch
     * is what puts it in place (the mods window offers the restart). So is
     * a live one that requires a mod still waiting for that launch. */
    if (mods[mod].restart) return;
    if (enabled) {
        int i, dep, wanted[MODS_MAX];
        for (i = 0; i < mod_count; i++) wanted[i] = mods[i].enabled;
        if ((dep = Mods_WaitsForRestart(mod, wanted)) >= 0) {
            note(&mods[mod], "waits for a restart: %s, which it requires, is applied at the next launch", mods[dep].name);
            return;
        }
    } else if (!mods[mod].active && !Mods_Failed(mod)) {
        /* whatever it was waiting for, it no longer is */
        copy_text(mods[mod].status, sizeof(mods[mod].status), mods[mod].warnings);
    }
    activate(mod, enabled);
}

void Mods_VisitCards(void (*visit)(const char *id, const char *directory, const struct JsonValue *cards, void *context),
                     void *context)
{
    int i;
    /* In load order, as every other table reads the mods (Mods_Loaded):
     * where two replace one card the later one wins, and the copies take
     * their ids in that order. Cards_Build runs once, after the first load. */
    for (i = 0; i < loaded_count; i++) {
        const Mod *mod = &mods[loaded[i]];
        if (mod->active && Json_Count(mod->cards)) visit(mod->id, mod->directory, mod->cards, context);
    }
}

void Mods_VisitManifests(void (*visit)(const char *id, const char *directory,
                                       const struct JsonValue *manifest, void *context), void *context)
{
    int i;
    for (i = 0; i < loaded_count; i++) {
        const Mod *mod = &mods[loaded[i]];
        if (mod->active) visit(mod->id, mod->directory, Json_Root(mod->manifest), context);
    }
}

int Mods_Setting(const char *id, const char *key, int fallback)
{
    char name[256];
    const char *text;
    if (by_id(id) < 0 || !key || !*key) return fallback;
    text = environment_name(name, sizeof(name), id, key) ? getenv(name) : NULL;
    if (text && *text) return (int)strtol(text, NULL, 0);
    if (!setting_key(name, sizeof(name), id, key)) return fallback;
    return Settings_GetNamed(name, fallback);
}

void Mods_Note(const char *id, const char *format, ...)
{
    char message[STATUS_MAX];
    va_list arguments;
    size_t length;
    int i = by_id(id);
    if (i < 0) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: mod %s: %s\n", mods[i].id, message);
    /* After the notes before it, as warn() adds a warning, unless it is one
     * of them already (a reader run again). */
    if (strstr(mods[i].status, message)) return;
    length = strlen(mods[i].status);
    snprintf(mods[i].status + length, sizeof(mods[i].status) - length, "%s%s", length ? "; " : "", message);
    Menu_TextTrim(mods[i].status);
}

void Mods_DrawFrame(void)
{
    int i;
    if (shut_down) return;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.frame) mods[i].hooks.frame();
    }
}

void Mods_Reset(void)
{
    int i;
    if (shut_down) return;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.reset) mods[i].hooks.reset();
    }
}

const JsonValue *Mods_Manifest(int index) { return at(index) ? Json_Root(mods[index].manifest) : NULL; }
const char *Mods_Metadata(int index, const char *key) { return Json_String(Json_Member(Mods_Manifest(index), key), ""); }
const char *Mods_Directory(int index) { return at(index) ? mods[index].directory : ""; }
const char *Mods_Origin(int index) { return at(index) ? mods[index].origin : ""; }
int Mods_Active(int index) { return at(index) && mods[index].active; }
int Mods_Failed(int index) { return at(index) && (mods[index].broken || mods[index].failed); }
int Mods_HasCode(int index) { return at(index) && Manifest_HasCode(Mods_Manifest(index)); }

unsigned Mods_CodeHash(int index) { return at(index) ? mods[index].code_hash : 0; }

unsigned Mods_Sequence(int index) { return at(index) ? mods[index].sequence : 0; }

unsigned Mods_Changes(void) { return change_count; }

void Mods_OptionChanged(int index, int option)
{
    const JsonValue *spec = Mods_Option(index, option);
    if (!at(index) || !mods[index].active || !has_images(&mods[index]) || !texture_pack_unload) return;
    /* A setting that waits for a restart keeps the value it was applied with (Mods_RuntimeOption). */
    if (mods[index].restart || Json_Bool(Json_Member(spec, "restart"), 0)) return;
    load_texture_packs(-1, -1, NULL, 0);
}

int Mods_RuntimeOption(int index, int option) {
    const JsonValue *spec = Mods_Option(index, option);
    if (at(index) && mods[index].active && mods[index].runtime_options &&
        (mods[index].restart || Json_Bool(Json_Member(spec, "restart"), 0))) return mods[index].runtime_options[option];
    return Mods_OptionValue(index, option);
}
