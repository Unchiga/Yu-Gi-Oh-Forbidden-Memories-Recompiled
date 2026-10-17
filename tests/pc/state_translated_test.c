#define _POSIX_C_SOURCE 200809L   /* mkdtemp, rmdir */
#include "pc/guest/state.h"
#include "game/build_deck_transition_state.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int entered, vsync_calls, diagnostics;
/* The program folder Memories_SymbolTablePath reads buildid from (the
 * platform's paths.c is not linked into this unit). */
static const char *program_dir;
const char *Paths_ProgramDir(void)
{
    return program_dir;
}
const unsigned Memories_GameFingerprint = 0x13572468u;
/* Optional gameplay tracing reads the real layout; this unit does not run
 * the deck menu or bind a game image. */
BuildDeckTransitionState gBuildDeck_aWorkspace[2];
extern int VSync(int mode);
/* Observing test doubles for the SDK/frame service and diagnostic sink.
 * No fake game routines are linked into the shipping backend. */
int Memories_VSync(int mode)
{
    assert(mode == 0);
    ++vsync_calls;
    Memories_StatePoint((unsigned)vsync_calls);
    return 263;
}
void Crash_ReportSoft(const char *kind, const char *detail)
{
    assert(strcmp(kind, "native save states unavailable") == 0);
    assert(detail && *detail);
    ++diagnostics;
}
static int game_entry(void)
{
    ++entered;
    assert(VSync(0) == 263);
    return 37;
}
int main(void)
{
    char path[32] = "not-cleared", folder[] = "/tmp/memories-state-XXXXXX", file[64], table[96], expected[96];
    FILE *buildid;
    assert(!Memories_StateStartupDone());
    assert(Memories_StateBuildId() == Memories_GameFingerprint);
    assert(Memories_StateRunGame(game_entry) == 37);
    assert(entered == 1 && vsync_calls == 1 && diagnostics == 0);
    assert(Memories_StateStartupDone());
    assert(Memories_StateRunGame(NULL) == 1 && errno == EINVAL);
    Memories_StateRequest(1, 2);
    assert(diagnostics == 0);
    assert(VSync(0) == 263 && diagnostics == 1 && errno == ENOTSUP);
    assert(VSync(0) == 263 && diagnostics == 1);
    Memories_StateRequest(2, 2);
    assert(VSync(0) == 263 && diagnostics == 2 && errno == ENOTSUP);
    assert(Memories_LastStateSlot() == 0);
    assert(Memories_StateLoading(NULL) == 0);
    assert(Memories_StateChunk(NULL, "test", NULL, 0) == 0 && errno == EINVAL);
    assert(diagnostics == 2);
    Memories_StateRemapRange(NULL, 1, 2, 4);
    assert(diagnostics == 3 && errno == ENOTSUP);
    /* symbols/<buildid>.txt beside the game: none without a buildid. */
    assert(Memories_SymbolTablePath(path, sizeof(path)) == -1 && path[0] == '\0');
    assert(mkdtemp(folder));
    program_dir = folder;
    strcpy(path, "not-cleared");
    assert(Memories_SymbolTablePath(path, sizeof(path)) == -1 && path[0] == '\0');
    snprintf(file, sizeof(file), "%s/buildid", folder);
    assert((buildid = fopen(file, "w")) && fputs("1a2b3c\n", buildid) >= 0 && fclose(buildid) == 0);
    assert(Memories_SymbolTablePath(table, sizeof(table)) == 0);
    snprintf(expected, sizeof(expected), "%s/symbols/001a2b3c.txt", folder);
    assert(strcmp(table, expected) == 0);
    assert(Memories_SymbolTablePath(path, 8) == -1);   /* too small for the path */
    assert(remove(file) == 0 && rmdir(folder) == 0);
    program_dir = NULL;
    assert(Memories_StateSaveHere("unused.state") == -1 && errno == ENOTSUP);
    assert(Memories_StateLoadHere("unused.state", path, sizeof(path)) == -1 && errno == ENOTSUP);
    assert(strstr(path, "unsupported") && diagnostics == 5);
    assert(Memories_StateLoadHere("unused.state", NULL, 0) == -1 && errno == ENOTSUP);
    puts("Native game entry/VSync delegation and explicit state rejection passed");
    return 0;
}
