/* Native-stack entry and save-state requests for translated ARM64 game code. */
#include "pc/compat/fs.h"
#include "pc/platform/paths.h"
#include "state.h"
#include "types.h"
#include "game/build_deck_transition_state.h"
#include "pc/debug/crash.h"
#include "pc/guest/translated_runtime.h"
#ifdef MEMORIES_TRANSLATED
#include "translated_state_backend.h"
#include "state_io.h"
#endif
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

MemoriesStateEntry Memories_StateEntry; /* compatibility storage; never captured */
static volatile sig_atomic_t requested;
static volatile sig_atomic_t requested_slot = 1;
static int startup_checked;
extern int Memories_VSync(int mode);
extern BuildDeckTransitionState gBuildDeck_aWorkspace[2];

static void unsupported(const char *operation)
{
    Crash_ReportSoft("native save states unavailable", operation);
    errno = ENOTSUP;
}

#ifndef MEMORIES_TRANSLATED
int VSync(int mode)
{
    /* The real native SDK implementation presents, services timers/input and
     * reaches Memories_StatePoint. No x86 register capture is attempted. */
    return Memories_VSync(mode);
}

#endif

int Memories_StateRunGame(int (*entry)(void))
{
    if (!entry) {
        errno = EINVAL;
        return 1;
    }
#ifdef MEMORIES_TRANSLATED
    return Memories_NativeStateRunGame(entry);
#else
    return entry();
#endif
}

void Memories_StateRequest(int what, int slot)
{
#ifdef MEMORIES_TRANSLATED
    Memories_NativeStateRequest(what, slot);
    return;
#endif
    /* Async-signal-safe: diagnostic work stays at the next frame boundary. */
    if (slot > 0) requested_slot = slot;
    requested = what;
}

void Memories_StatePoint(unsigned presented_frames)
{
    int what = requested;
    if (getenv("MEMORIES_TRACE_GAMEPLAY") && presented_frames % 120 == 0 && GuestRuntime_IsBound()) {
        MemoriesMemory *memory = GuestRuntime_Memory();
        const u8 *mode = Memories_Resolve(memory, 0x8009b26c, 1, 1);
        const u8 *scene = Memories_Resolve(memory, 0x8009b27a, 1, 1);
        const u8 *location = Memories_Resolve(memory, 0x8016960c, 1, 1);
        const u8 *substate = Memories_Resolve(memory, 0x8009b26e, 1, 1);
        const u8 *deck = Memories_Resolve(memory, 0x801d0200, 80, 2);
        unsigned deck_count = 0;
        for (unsigned i = 0; i < 40; i++) if (deck && (deck[2*i] || deck[2*i+1])) deck_count++;
        fprintf(stderr, "gameplay frame=%u mode=%02x sub=%02x scene=%u location=%u deck=%u\n",
                presented_frames, mode ? *mode : 0, substate ? *substate : 0,
                scene ? *scene : 0, location ? *location : 0, deck_count);
        const u8 *quantities = Memories_Resolve(memory, 0x801d0250, 722, 1);
        const u8 *chips = Memories_Resolve(memory, 0x801d07e0, 4, 4);
        unsigned copies = 0;
        for (unsigned i = 0; i < 722; i++) copies += quantities[i];
        fprintf(stderr, "inventory chest=%u starchips=%u\n", copies, Memories_ReadLE32(chips));
        if (mode && (((*mode & 0x1f) == 3 && substate && *substate == 0x80) ||
                     (*mode & 0x1f) == 7)) {
            {
                BuildDeckTransitionState *ws = &gBuildDeck_aWorkspace[0];
                fprintf(stderr, "deckmenu state=%x next=%x pane=%u deck=%d chest=%d effect=%u pointer=%x expected=%x\n",
                    ws->state, ws->next_state, ws->pane_index, ws->deck_total, ws->chest_total,
                    *(u8 *)Memories_Resolve(memory, 0x8009b254, 1, 1),
                    Memories_ReadLE32(Memories_Resolve(memory, 0x8009b2fc, 4, 4)),
                    GuestRuntime_EncodePointer(ws));
                for (unsigned pane = 0; pane < 2; ++pane) {
                    CardList *list = &ws->lists[pane];
                    fprintf(stderr, "decklist pane=%u sort=%d mode=%u first=%d target=%d cursor=%d card=%u\n",
                        pane, list->sort_choice, list->sort_mode, list->first,
                        list->first_target, list->cursor,
                        list->entries[list->first + list->cursor].id);
                }
            }
        }
        if (mode && (*mode & 0x1f) == 3 && substate && *substate == 0x81) {
            const u8 *phase = Memories_Resolve(memory, 0x8009b23a, 2, 2);
            const u8 *player_lp = Memories_Resolve(memory, 0x800ea004, 2, 2);
            const u8 *opponent_lp = Memories_Resolve(memory, 0x800ea024, 2, 2);
            fprintf(stderr, "duel phase=%x action=%x side=%u LP=%u/%u\n",
                phase[0] | phase[1] << 8,
                *(u8 *)Memories_Resolve(memory, 0x8009b174, 1, 1),
                *(u8 *)Memories_Resolve(memory, 0x8009b1d5, 1, 1),
                player_lp[0] | player_lp[1] << 8, opponent_lp[0] | opponent_lp[1] << 8);
            u32 cursor_token = Memories_ReadLE32(Memories_Resolve(memory, 0x8009b1b4, 4, 4));
            if (cursor_token) {
                const u8 *cursor = GuestRuntime_ResolveData((void *)(uintptr_t)cursor_token, 0x1a);
                fprintf(stderr, "duel cursor=%x col=%d row=%d status=%x hand-slot=%d\n", cursor_token,
                    (signed char)cursor[15], (signed char)cursor[16], cursor[25],
                    (signed char)cursor[14]);
            }
            const u8 *rank = Memories_Resolve(memory, 0x800e9ff0, 64, 4);
            fprintf(stderr, "duel counters turns=%u/%u fusions=%u/%u equips=%u/%u magic=%u/%u traps=%u/%u hand=",
                rank[1], rank[33], rank[8], rank[40], rank[9], rank[41],
                rank[5], rank[37], rank[6], rank[38]);
            for (unsigned i = 0; i < 5; i++) {
                const u8 *order = Memories_Resolve(memory, 0x800907cc, 10, 1);
                unsigned index = (order[i] & 0x7f) + ((order[i] & 0x80) ? 15 : 0);
                /* rank.hand contains deck indices, not field-record indices.
                 * The presentation order maps each hand slot to its record. */
                const u8 *record = (signed char)rank[26+i] >= 0 && index < 30 ?
                    Memories_Resolve(memory, 0x801a7ad8 + 28*index, 28, 4) : NULL;
                fprintf(stderr, "%s%u", i ? "," : "", record ? record[12] | record[13]<<8 : 0);
            }
            fputc('\n', stderr);
            fprintf(stderr, "duel field=");
            for (unsigned i = 5; i < 10; i++) {
                const u8 *record = Memories_Resolve(memory, 0x801a7ad8 + 28*i, 28, 4);
                unsigned flags = record[22] | record[23]<<8;
                if (!(flags & 0x8000)) continue;
                fprintf(stderr, " %u:%u/%u/%u/%d/%d", i,
                    record[12] | record[13]<<8, record[14] | record[15]<<8,
                    record[16] | record[17]<<8,
                    (s16)(record[18] | record[19]<<8), (s16)(record[20] | record[21]<<8));
            }
            fputc('\n', stderr);
            for (unsigned zone = 0; zone < 3; zone++) {
                unsigned first = zone == 0 ? 10 : zone == 1 ? 20 : 25;
                fprintf(stderr, "duel %s=", zone == 0 ? "backrow" :
                        zone == 1 ? "opponent" : "opponent-backrow");
                for (unsigned i = first; i < first + 5; i++) {
                    const u8 *record = Memories_Resolve(memory, 0x801a7ad8 + 28*i, 28, 4);
                    unsigned flags = record[22] | record[23]<<8;
                    if (!(flags & 0x8000)) continue;
                    fprintf(stderr, " %u:%u/%x", i, record[12] | record[13]<<8, flags);
                }
                fputc('\n', stderr);
            }
        }
    }
#ifdef MEMORIES_TRANSLATED
    Memories_NativeStatePoint(presented_frames);
    return;
#endif
    (void)requested_slot;
    requested = 0;
    if (!startup_checked) {
        const char *load = getenv("MEMORIES_LOAD_STATE");
        const char *save = getenv("MEMORIES_SAVE_STATE");
        const char *autosave = getenv("MEMORIES_AUTOSAVE");
        startup_checked = 1;
        if ((load && *load) || (save && *save) || (autosave && *autosave))
            unsupported("save/load state environment requests are unsupported by the native translated backend");
    }
    if (what == 1)
        unsupported("state save rejected: native stack and callbacks cannot yet be serialized");
    else if (what == 2)
        unsupported("state load rejected: i386 saved stacks cannot be restored on arm64");
    else if (what)
        unsupported("unrecognized save-state request");
}

int Memories_LastStateSlot(void)
{
#ifdef MEMORIES_TRANSLATED
    return Memories_NativeStateLastSlot();
#else
    return 0;
#endif
}

int Memories_StateSaveHere(const char *path)
{
#ifdef MEMORIES_TRANSLATED
    return Memories_NativeStateSave(path);
#endif
    (void)path;
    unsupported("save states are unsupported on macOS ARM64; use the game's normal saves");
    return -1;
}

int Memories_StateLoadHere(const char *path, char *why, size_t why_size)
{
#ifdef MEMORIES_TRANSLATED
    return Memories_NativeStateLoad(path, why, why_size);
#endif
    (void)path;
    if (why && why_size)
        snprintf(why, why_size, "save states are unsupported on macOS ARM64; use the game's normal saves");
    unsupported("state load rejected: i386 saved stacks cannot be restored on arm64");
    return -1;
}

int Memories_StateStartupDone(void)
{
#ifdef MEMORIES_TRANSLATED
    return Memories_NativeStateStartupDone();
#else
    return startup_checked;
#endif
}

uint32_t Memories_StateBuildId(void)
{
    extern const unsigned Memories_GameFingerprint;
    return Memories_GameFingerprint;
}

void Memories_StateRemapRange(MemoriesState *state, uint32_t from,
                             uint32_t to, uint32_t size)
{
#ifdef MEMORIES_TRANSLATED
    Memories_NativeStateRemapRange(state, from, to, size);
    return;
#endif
    (void)state; (void)from; (void)to; (void)size;
    unsupported("saved-memory remapping is unsupported by the native translated backend");
}

int Memories_SymbolTablePath(char *out, size_t size)
{
    /* symbols/<buildid>.txt beside the game, as state.c's: in the
     * translated build (libgame.so, linked at 0) its addresses are offsets
     * into the image, which crash reports add the load address to
     * (debug/symbols.c) and save states carry across builds with
     * (translated_state_symbols.c). macOS ships none. */
    const char *directory = Paths_ProgramDir();
    char path[640], text[32] = "";
    unsigned long id = 0;
    FILE *file;
    if (out && size) out[0] = '\0';
    if (!directory || snprintf(path, sizeof(path), "%s/buildid", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    if (fgets(text, sizeof(text), file)) id = strtoul(text, NULL, 16);
    fclose(file);
    if (!id || !out || snprintf(out, size, "%s/symbols/%08lx.txt", directory, id) >= (int)size) return -1;
    return 0;
}
