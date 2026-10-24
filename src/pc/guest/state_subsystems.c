/* Shared subsystem payloads and compatibility checks for every state backend. */
#define _GNU_SOURCE
#include "pc/tower/tower.h"
#include "state.h"
#include "state_io.h"
#include "state_subsystems.h"
#include "state_remap.h"
#include "pc/platform/settings.h"
#include "pc/platform/paths.h"
#include "pc/mods/mods.h"
#include "pc/mods/events.h"
#include "image.h"
#include "retail_image.h"
#include "pc/audio/spu.h"
#include "pc/audio/replace.h"
#include "pc/compat/gte.h"
#include "pc/rng.h"
#include "pc/render/soft_gpu.h"
#include "pc/render/texture_dump.h"
#include "pc/saves/deck_menu.h"
#include "pc/cards/pack_shop.h"
#include "pc/cards/fields.h"
#include "pc/text/language.h"
#include "pc/text/text.h"
#include "pc/platform/menu.h"
#include "pc/platform/title_screen.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/compat/signal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/mman.h"
#include <sys/stat.h>
#include <unistd.h>
#include "pc/compat/posix.h"

typedef struct { MemoriesState *state; int valid; } ModStateCheck;
static void mod_state_tag(char *tag, size_t size, int owner)
{
    int ordinal = 0;
    /* Rank by stable identity, independent of discovery and activation order. */
    for (int i = 0; i < Mods_Count(); i++)
        if (Mods_Active(i) && strcmp(Mods_Id(i), Mods_Id(owner)) < 0)
            ordinal++;
    snprintf(tag, size, "mod:%d", ordinal);
}
static void mod_state_visit(int owner, void *data, size_t size, unsigned version, void *context)
{
    MemoriesState *state = context;
    char tag[16];
    MemoriesStateField fields[] = {{&version, sizeof(version)}, {data, size}};
    mod_state_tag(tag, sizeof(tag), owner);
    Memories_StateChunk(state, tag, fields, 2);
}
static void mod_state_check(int owner, void *data, size_t size, unsigned version, void *context)
{
    ModStateCheck *check = context;
    char tag[16]; size_t have = 0; unsigned saved = 0;
    const uint8_t *chunk;
    (void)data;
    mod_state_tag(tag, sizeof(tag), owner);
    chunk = Memories_StateFindChunk(check->state, tag, &have);
    if (chunk && have >= sizeof(saved)) memcpy(&saved, chunk, sizeof(saved));
    if (!chunk || have != size + sizeof(version) || saved != version) check->valid = 0;
}
int Memories_StateCompatibleMods(MemoriesState *state)
{
    size_t size = 0;
    const uint8_t *chunk = Memories_StateFindChunk(state, "mod-set", &size);
    unsigned saved = 0, current = Mods_Signature();
    ModStateCheck check = {state, 1};
    if (chunk && size == sizeof(saved)) memcpy(&saved, chunk, size);
    /* Old vanilla states remain usable; old modded states have no way to
     * establish card identity or native callback compatibility. */
    if ((!chunk && current != 2166136261u) || (chunk && (size != sizeof(saved) || saved != current))) return 0;
    Mods_VisitState(mod_state_check, &check);
    return check.valid;
}

#define LANGUAGE_CODE_SIZE 16
#define LANGUAGE_CHUNK_VERSION 1u
typedef struct {
    char code[LANGUAGE_CODE_SIZE];
    uint32_t version;
    uint32_t base, used, crc; /* TextLayout */
} LanguageChunk;
/* Written as it lies in memory, in the machine's byte order like the rest
 * of the state (which loads only on the system that saved it): 32 bytes,
 * no padding between the fields. */
_Static_assert(sizeof(LanguageChunk) == LANGUAGE_CODE_SIZE + 4 * sizeof(uint32_t), "LanguageChunk is padded");

/* The state's chunk: 1 with the layout, 0 with the code only, -1 with no
 * chunk (English US). */
static int state_language(const MemoriesState *state, LanguageChunk *saved)
{
    size_t size = 0;
    const uint8_t *chunk = Memories_StateFindChunk(state, "language", &size);
    memset(saved, 0, sizeof(*saved));
    if (!chunk || size < LANGUAGE_CODE_SIZE) {
        snprintf(saved->code, sizeof(saved->code), "%s", Language_Code(LANGUAGE_US));
        return -1;
    }
    memcpy(saved->code, chunk, LANGUAGE_CODE_SIZE - 1);
    if (size < sizeof(*saved)) return 0;
    memcpy(&saved->version, chunk + LANGUAGE_CODE_SIZE, sizeof(*saved) - LANGUAGE_CODE_SIZE);
    return saved->version >= 1;
}

static const char *language_label(const char *code)
{
    int language;
    for (language = 0; language < LANGUAGE_COUNT; language++) {
        if (!strcmp(code, Language_Code(language))) return Language_Label(language);
    }
    return code;
}

int Memories_StateCompatibleLanguage(const MemoriesState *state, const char *path, char *why, size_t why_size)
{
    LanguageChunk saved;
    int found = state_language(state, &saved);
    TextLayout now = Text_Layout();
    const char *name = path, *at;
    for (at = path; *at; at++) {
        if (*at == '/' || *at == '\\') name = at + 1; /* MEMORIES_LOAD_STATE may give a Windows path */
    }
    if (strcmp(saved.code, Language_Code(Language_Current()))) {
        snprintf(why, why_size, "%s was made with the game in %s%s, and the game is in %s now. Choose %s in Game > Language, "
               "restart, and load it again.", name, language_label(saved.code),
               found < 0 ? " (it is older than the language setting)" : "", Language_Label(Language_Current()),
               language_label(saved.code));
        return 0;
    }
    if (found < 1) {
        /* No layout to check: only the game's own text (English US, no
         * translation mod) is where it was. */
        if (!now.used) return 1;
        snprintf(why, why_size, "%s is older than this version's text check, and the game has text of its own compiled now (a "
               "language or a translation mod), which the state cannot be checked against. Loading it could show "
               "the wrong lines or crash.", name);
        return 0;
    }
    if (saved.used && !saved.base) {
        snprintf(why, why_size, "%s was made while the game's text could not be placed at its fixed address, so the text it "
               "points into is gone.", name);
        return 0;
    }
    if (saved.base != now.base || saved.used != now.used || saved.crc != now.crc) {
        snprintf(why, why_size, "%s was made with other game text: a language pack, a translation mod's text or this version's "
               "own words changed since (%u bytes at 0x%08X, CRC %08X then; %u bytes at 0x%08X, CRC %08X now). "
               "Loading it would show the wrong lines or crash.",
               name, (unsigned)saved.used, (unsigned)saved.base, (unsigned)saved.crc, now.used, now.base, now.crc);
        return 0;
    }
    return 1;
}

/* Where VRAM's words came from on the disc (texture_dump.h), which a
 * texture pack goes by. Without them a loaded state showed retail textures
 * wherever the pack could not find its pictures again by their bytes alone:
 * a page the game had drawn into since its upload (the duel's card frames,
 * digits and panels). As runs (TextureDump_TagRuns), a few thousand pairs
 * where the tags themselves are 2 MiB. */
static void texture_tags(MemoriesState *state)
{
    const size_t max = (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT;
    uint32_t count = 0, *runs = NULL;
    MemoriesStateField counted = {&count, sizeof(count)}, data;
    if (!Memories_StateLoading(state)) {
        if (!TextureDump_Tags || !(runs = malloc(max * 2 * sizeof(*runs)))) return;
        count = (uint32_t)TextureDump_TagRuns(runs, max);
        data.data = runs;
        data.size = (size_t)count * 2 * sizeof(*runs);
        Memories_StateChunk(state, "vram-tags", &counted, 1);
        Memories_StateChunk(state, "vram-tag-runs", &data, 1);
        free(runs);
        return;
    }
    if (TextureDump_Tags && Memories_StateChunk(state, "vram-tags", &counted, 1) && count && count <= max &&
        (runs = malloc((size_t)count * 2 * sizeof(*runs)))) {
        data.data = runs;
        data.size = (size_t)count * 2 * sizeof(*runs);
        if (!Memories_StateChunk(state, "vram-tag-runs", &data, 1)) count = 0;
    } else {
        count = 0;
    }
    TextureDump_TagsLoaded(runs, count); /* VRAM restored: the rest the pack finds again */
    free(runs);
}

void Memories_StateSubsystems(MemoriesState *state)
{
#ifdef MEMORIES_TRANSLATED
    extern void GuestRuntime_JumpState(MemoriesState *);
    extern void Memories_MipsState(MemoriesState *);
    GuestRuntime_JumpState(state);
    Memories_MipsState(state);
#endif
    unsigned signature = Mods_Signature();
    MemoriesStateField mod_set = {&signature, sizeof(signature)};
    MemoriesStateField gpu[2], gte[1];
    LanguageChunk language;
    MemoriesStateField language_field = {&language, sizeof(language)};
    if (!Memories_StateLoading(state)) {
        TextLayout layout = Text_Layout();
        Memories_StateChunk(state, "mod-set", &mod_set, 1);
        memset(&language, 0, sizeof(language));
        snprintf(language.code, sizeof(language.code), "%s", Language_Code(Language_Current()));
        language.version = LANGUAGE_CHUNK_VERSION;
        language.base = layout.base;
        language.used = layout.used;
        language.crc = layout.crc;
        Memories_StateChunk(state, "language", &language_field, 1);
    }
    Mods_VisitState(mod_state_visit, state);
    unsigned gte_size;
    gpu[0].data = SoftGpu_StateData(0, &gpu[0].size);
    gpu[1].data = SoftGpu_StateData(1, &gpu[1].size);
    gte[0].data = Gte_StateData(&gte_size);
    gte[0].size = gte_size;
    if (Memories_StateChunk(state, "soft_gpu", gpu, 2) || !Memories_StateLoading(state)) {
        texture_tags(state);
        if (Memories_StateLoading(state)) SoftGpu_PictureFromVram();
    }
    Memories_StateChunk(state, "gte", gte, 1);
    {
        /* The game's random seed (src/pc/rng.c, which `rand` and `srand`
         * are renamed to): native, so in no game section, yet the deck's
         * shuffle, the CPU's choices and a pack's cards all follow it. On
         * the console it sits in RAM, which a state holds. Without it a
         * state loaded in a running game drew other numbers than the game
         * that saved it. */
        MemoriesStateField seed = {&gRand_dwSeed, sizeof(gRand_dwSeed)};
        Memories_StateChunk(state, "rng", &seed, 1);
    }
    {
        /* The mods' rand seed (mod_libc.c): the same kind of number, one
         * sequence for every mod, which a mod's choices follow. */
        unsigned size;
        MemoriesStateField seed;
        seed.data = Mods_RandSeed(&size);
        seed.size = size;
        Memories_StateChunk(state, "mod-rng", &seed, 1);
    }
#if defined(_WIN32) && defined(__x86_64__) && !defined(MEMORIES_TRANSLATED)
    Memories_StateJumps(state);
#endif
    RetailImage_State(state);
    Spu_State(state);
    LibSpu_State(state);
    LibDs_State(state);
    LibEtc_State(state);
    LibGpu_State(state);
    LibGte_State(state);
    LibPress_State(state);
    LibMcrd_State(state);
    SaveMenu_State(state);
    if (!Memories_StateLoading(state)) DeckMenu_ShopState(state);
    if (!Memories_StateLoading(state)) PackShop_State(state);
    Fields_State(state);
    Tower_State(state); /* the other towers' progress lines; the rest is game data */
    TitleJump_State(state);
    TitleScreen_State(state);
    Platform_State(state);
    DeckMenu_State(state); /* the decks' draft, kept with the save */
}
