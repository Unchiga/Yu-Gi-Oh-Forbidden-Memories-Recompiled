#include "pc/compat/fs.h"
#include "image.h"
#include "state.h"
#include "pc/platform/platform.h"
#include "pc/platform/loading.h"
#include "pc/debug/log.h"
#include "pc/debug/symbols.h"
#include "pc/debug/crash.h"
#include "pc/debug/monitor.h"
#include "pc/debug/crash_test.h"
#include "pc/platform/settings.h"
#include "pc/platform/paths.h"
#include "pc/mods/mods.h"
#include "pc/mods/exports.h"
#include "pc/platform/game_files.h"
#include "pc/platform/update_check.h"
#include "pc/cards/cards.h"
#include "pc/text/entry_layout.h"
#include "pc/text/language.h"
#include "pc/text/text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <errno.h>
#endif

/* MEMORIES_RESTART_ENV="NAME=value;NAME=;...": what a scripted check changes
 * in the environment the restarted game starts with (an empty value removes
 * the name), so that it can drive the game past a restart instead of the
 * new game replaying the old one's MEMORIES_INPUT. Applied once, by the
 * game that restarts; on Windows the monitor starts the game again with its
 * own environment, so it needs MEMORIES_NO_MONITOR=1 there. */
static void restart_environment(void)
{
    const char *script = getenv("MEMORIES_RESTART_ENV");
    size_t length;
    char *copy, *item, *next;
    if (!script || !*script) return;
    length = strlen(script);
    copy = malloc(length + 1);
    if (!copy) return;
    memcpy(copy, script, length + 1);
    unsetenv("MEMORIES_RESTART_ENV");
    for (item = copy; item; item = next) {
        char *equals;
        next = strchr(item, ';');
        if (next) *next++ = '\0';
        equals = strchr(item, '=');
        if (!equals || equals == item) continue;
        *equals = '\0';
        if (equals[1]) setenv(item, equals + 1, 1);
        else unsetenv(item);
    }
    free(copy);
}

#ifdef _WIN32
#include "pc/platform/win32.h"
#include <fcntl.h>

int Platform_RestartGame(void)
{
    unsetenv("MEMORIES_CONTROL"); /* no client follows a restart (MEMORIES_RESTART_ENV may name one) */
    restart_environment();
    return Win32_Restart();
}
#else
#include <signal.h>
#include <sys/time.h>
#endif

static char **launch_argv;

#if !defined(_WIN32) && !defined(__ANDROID__) /* android.c: an app cannot re-execute itself */
int Platform_RestartGame(void)
{
 #ifdef __APPLE__
    uint32_t capacity = 1024;
    char *path = malloc(capacity);
    if (!path) return -1;
    if (_NSGetExecutablePath(path, &capacity)) {
        free(path); path = malloc(capacity);
        if (!path) return -1;
        if (_NSGetExecutablePath(path, &capacity)) { free(path); errno = ENAMETOOLONG; return -1; }
    }
    char *executable = realpath(path, NULL);
    free(path);
    if (!executable) return -1; /* restart only a verified absolute pathname */
 #endif
    struct itimerval stopped = {0}, previous;
    struct sigaction ignored = {0}, old_action;
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGALRM, &ignored, &old_action);
    setitimer(ITIMER_REAL, &stopped, &previous);
    unsetenv("MEMORIES_CONTROL"); /* no client follows a restart (MEMORIES_RESTART_ENV may name one) */
    restart_environment();
    /* A restart must boot the game, not auto-load an old launch state. */
    unsetenv("MEMORIES_LOAD_STATE");
 #ifdef __APPLE__
    execv(executable, launch_argv);
    int saved_error = errno; free(executable); errno = saved_error;
 #else
    execv("/proc/self/exe", launch_argv);
 #endif
    perror("memories-pc: restart");
    sigaction(SIGALRM, &old_action, NULL);
    setitimer(ITIMER_REAL, &previous, NULL);
    return -1;
}
#endif

extern int Main_Init(void);

/* The game, once the port has its startup picture of the game data (a save
 * state's words the game never changed are told by it): a PAL language's
 * text entries come after it, so a state keeps its own (entry_layout.h). */
static int start_game(void)
{
    TextEntries_Start();
    return Main_Init();
}

/* The game's executable: the file named on the command line (development),
 * or SLUS_014.11 read out of the player's disc image. */
static int load_game(const char *named)
{
    char why[768];
    const char *disc;
    unsigned char *data;
    size_t size = 0;
    int result;
    if (named) return Memories_GuestLoadExe(named);
    result = GameFiles_Setup(why, sizeof(why));
    if (!result) return 1; /* Cancelling setup is a normal exit. */
    disc = result > 0 ? GameFiles_Disc(why, sizeof(why)) : NULL;
    if (!disc) {
        Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", why);
        return -1;
    }
    data = GameFiles_ReadExecutable(disc, &size);
    if (!data) {
        snprintf(why, sizeof(why), "Could not read the game's executable from %s.", disc);
        Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", why);
        return -1;
    }
    result = Memories_GuestLoadExeData(data, size, disc);
    free(data);
    return result;
}

/* The settings and the applied mods, for crash reports (monitor.h); again
 * whenever a setting changes. */
static void note_settings(SettingId changed, int value)
{
    char text[900];
    size_t used = 0;
    int id;
    (void)changed;
    (void)value;
    text[0] = '\0';
    for (id = 0; id < SET_COUNT && used < sizeof(text); id++) {
        const char *key = Settings_Key((SettingId)id);
        if (!key) continue; /* a retired setting (settings.c): no key, not saved */
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s=%d", used ? " " : "", key,
                                 Settings_Get((SettingId)id));
    }
    Monitor_Fact("settings", "%s", text);
    used = 0;
    text[0] = '\0';
    for (id = 0; id < Mods_Count() && used < sizeof(text); id++) {
        if (Mods_Enabled(id)) used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s", used ? " " : "", Mods_Id(id));
    }
    Monitor_Fact("mods", "%s", used ? text : "none");
}

/* GCC's MIPS `main` prologue hook; nothing to construct natively. */
void Psx___main(void)
{
}

/* Paths_WatchUserDir: the last write under the user folder, for the facts. */
static void user_dir_fact(int writable, const char *why)
{
    if (writable) Monitor_Fact("user dir", "%s; writable: yes", Paths_UserDir());
    else Monitor_Fact("user dir", "%s; writable: no: %s", Paths_UserDir(), why);
}

static const LoadingSink loading_sink = {Platform_ShowLoading, Platform_EndLoading, NULL, NULL};

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* main's narrow argv is lossy under a legacy Windows code page. */
    argv = Memories_Argv(&argc);
    if (!argv) return 1;
#endif
    const char *exe = argc > 1 ? argv[1] : NULL;
    launch_argv = argv;
    if (getenv("MEMORIES_MOD_EXPORTS")) {
        return Mods_PrintExports();   /* tools/pc/check_mod_exports.py */
    }
#ifdef _WIN32
    _set_fmode(_O_BINARY); /* disc images and states: no newline translation */
#endif
    if (getenv("MEMORIES_EXPORT_LANGUAGES")) {
        return Language_Export(getenv("MEMORIES_EXPORT_LANGUAGES"));   /* tools/pc/export_languages.py */
    }
    {
        /* This process may become the crash monitor, with the game its child. */
        int status;
        if (Monitor_Main(argc, argv, &status)) return status;
    }
    Log_Init();
    Symbols_Load();
    Mods_SetFunctionNames(Symbols_Lookup);   /* the functions code mods hook, in the Mods window */
    Crash_Init();
    Monitor_NoteSystem();
    /* Whether the player's files can be saved at all (an antivirus or
     * Controlled folder access blocking Documents), in every report and in
     * Help > System info: known from the game's own writes, never from a
     * test file, which such guards notify about or take for ransomware. */
    Monitor_Fact("user dir", "%s; writable: not tried yet", Paths_UserDir());
    Paths_WatchUserDir(user_dir_fact);
    CrashTest_Init();
    /* Guest globals are linked at fixed addresses: map before touching any. */
    if (Memories_GuestMap() != 0) {
        char why[1000];
        if (Platform_GuestMemoryHelp(why, sizeof(why))) Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", why);
        return 1;
    }
    {
        int loaded = load_game(exe);
        if (loaded) return loaded > 0 ? 0 : 1;
    }
    if (Memories_ModulesInit() != 0) {
        return 1;
    }
    if (Platform_Open("Yu-Gi-Oh! Forbidden Memories") != 0) {
        return 1;
    }
    Loading_Begin(&loading_sink);   /* a start that drags shows what it is doing (loading.h) */
    Update_Start(); /* in the background; a newer release shows a notice */
    /* The mods are applied by now (Platform_Open reads the settings), and
     * the executable is in place: the cards they add come after its own. */
    Loading_Step("Text and fonts", 0, 0);
    Text_Build();   /* the mods' translations and fonts, which the cards' names may use */
    Cards_Build();
    Loading_Step("Sorting cards", 0, 0);
    Text_SortCards();
    Loading_Step("Starting the game", 0, 0);
    Loading_Finish();   /* the picture the game draws next replaces it */
    note_settings(SET_COUNT, 0);
    Settings_Observe(note_settings);
#ifndef _WIN32 /* the Windows runtime has no line buffering, and takes no size 0 */
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    /* The game runs on a stack at a fixed address; see state.h. */
    return Memories_StateRunGame(start_game);
}
