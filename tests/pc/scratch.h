#ifndef MEMORIES_TESTS_SCRATCH_H
#define MEMORIES_TESTS_SCRATCH_H
/* Scratch files and folders for the tests, in the system's temporary folder:
 * TMPDIR, else TEMP or TMP (Windows), else /tmp. A plain "/tmp" is \tmp on
 * the current drive on Windows, which a fresh machine or a CI runner does not
 * have (Wine maps it, which hid this).
 *
 * scratch_dir() makes memories-<kind>-p<pid>-XXXXXX and removes it again
 * when the test exits or fails an assert() (and on SIGINT/SIGTERM off
 * Windows). What a test cannot remove itself (killed by a timeout, or a file
 * it still holds open, which Windows will not delete) is swept later: by the
 * next scratch_dir() of the same kind, and by pc_scratch_sweep, which CTest
 * runs after the tests (scratch_sweep_test.c). The sweep takes only a kind
 * in scratch_kinds() whose process is gone (a PID it cannot read counts as
 * alive), and the untagged memories-<kind>-XXXXXX of an earlier build once
 * it is a day old. A new kind goes into scratch_kinds().
 *
 * Removal never goes through a link: a symbolic link, junction or other
 * reparse point is unlinked, or left where it is if that fails, and never
 * entered (scratch_rules_test.c). */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include "pc/compat/fs.h" /* UTF-8 opendir/stat/rmdir/remove/getenv/mkdtemp */
/* Declared here rather than from <windows.h>, whose types clash with the
 * game's headers; the declarations match it, so a test may include both. */
__declspec(dllimport) unsigned long __stdcall GetFileAttributesW(const wchar_t *);
__declspec(dllimport) int __stdcall RemoveDirectoryW(const wchar_t *);
__declspec(dllimport) int __stdcall DeleteFileW(const wchar_t *);
__declspec(dllimport) void *__stdcall OpenProcess(unsigned long, int, unsigned long);
__declspec(dllimport) int __stdcall GetExitCodeProcess(void *, unsigned long *);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define SCRATCH_MAX 512
#define SCRATCH_ROOTS 16
#define SCRATCH_LEGACY_AGE (24 * 60 * 60)

static inline const char *scratch_base(void)
{
    const char *names[] = {"TMPDIR", "TEMP", "TMP"}, *base = NULL;
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]) && (!base || !*base); i++) base = getenv(names[i]);
    return base && *base ? base : "/tmp";
}

/* A mkstemp/mkdtemp template, for a test that manages the name itself. */
static inline void scratch_template(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s-XXXXXX", scratch_base(), name);
}

/* Every kind the tests make, the only ones the global sweep touches. */
static inline const char *const *scratch_kinds(size_t *count)
{
    static const char *const kinds[] = {
        "memories-audio", "memories-backend", "memories-card-identities", "memories-card-layout-art", "memories-card-order", "memories-controls",
        "memories-decks", "memories-disc", "memories-fs", "memories-lifecycle", "memories-log",
        "memories-manager", "memories-mod-window", "memories-mods", "memories-packs", "memories-report-folder", "memories-rom", "memories-runtime",
        "memories-save-menu", "memories-save-slots", "memories-scratch-rules", "memories-settings",
        "memories-state", "memories-texture-pack", "memories-tower", "memories-user-dir", "memories-window"};
    *count = sizeof(kinds) / sizeof(kinds[0]);
    return kinds;
}

static inline unsigned long scratch_pid(void)
{
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

/* Nonzero unless process `pid` is known to be gone. */
static inline int scratch_alive(unsigned long pid)
{
#ifdef _WIN32
    void *process;
    unsigned long code;
    int alive;
    if (!pid) return 1;
    process = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, 0, pid);
    /* Only "no such process" counts as gone; a refusal means it is there. */
    if (!process) return GetLastError() != 87 /* ERROR_INVALID_PARAMETER */;
    alive = !GetExitCodeProcess(process, &code) || code == 259 /* STILL_ACTIVE */;
    CloseHandle(process);
    return alive;
#else
    if (!pid || pid > 0x7FFFFFFFul) return 1; /* not a process of ours: leave it */
    return !kill((pid_t)pid, 0) || errno == EPERM;
#endif
}

enum { SCRATCH_NONE, SCRATCH_FILE, SCRATCH_FOLDER, SCRATCH_LINK };

/* What is at `path`, without following a link (lstat). */
static inline int scratch_entry(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = Memories_Utf8ToWide(path);
    unsigned long attributes = wide ? GetFileAttributesW(wide) : 0xFFFFFFFFul;
    free(wide);
    if (attributes == 0xFFFFFFFFul) return SCRATCH_NONE;
    if (attributes & 0x400 /* FILE_ATTRIBUTE_REPARSE_POINT */) return SCRATCH_LINK;
    return attributes & 0x10 /* FILE_ATTRIBUTE_DIRECTORY */ ? SCRATCH_FOLDER : SCRATCH_FILE;
#else
    struct stat info;
    if (lstat(path, &info)) return SCRATCH_NONE;
    if (S_ISLNK(info.st_mode)) return SCRATCH_LINK;
    return S_ISDIR(info.st_mode) ? SCRATCH_FOLDER : SCRATCH_FILE;
#endif
}

/* Remove the link itself; its target is not touched. If that fails, it stays. */
static inline void scratch_unlink(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = Memories_Utf8ToWide(path);
    unsigned long attributes = wide ? GetFileAttributesW(wide) : 0xFFFFFFFFul;
    if (attributes != 0xFFFFFFFFul) {
        if (attributes & 0x10) RemoveDirectoryW(wide);
        else DeleteFileW(wide);
    }
    free(wide);
#else
    unlink(path);
#endif
}

/* Remove a file or a whole tree. A missing path is fine. */
static inline void scratch_remove(const char *path, int depth)
{
    DIR *dir;
    struct dirent *entry;
    char child[2 * SCRATCH_MAX];
    switch (scratch_entry(path)) {
    case SCRATCH_NONE: return;
    case SCRATCH_LINK: scratch_unlink(path); return;
    case SCRATCH_FILE: remove(path); return;
    default: break;
    }
    if (!rmdir(path) || depth > 32) return;
    dir = opendir(path);
    if (!dir) return;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) continue;
        scratch_remove(child, depth + 1);
    }
    closedir(dir);
    rmdir(path);
}

typedef struct {
    char roots[SCRATCH_ROOTS][SCRATCH_MAX];
    char base[SCRATCH_MAX]; /* read once: no getenv in a signal handler */
    int count, hooked;
    unsigned long owner; /* the process that made them: a fork's child leaves them be */
} ScratchState;

static inline ScratchState *scratch_state(void)
{
    static ScratchState state;
    return &state;
}

static inline void scratch_cleanup(void)
{
    ScratchState *state = scratch_state();
    int i, moved = 0;
    if (state->owner != scratch_pid()) return;
    for (i = state->count - 1; i >= 0; i--) {
        scratch_remove(state->roots[i], 0);
        if (scratch_entry(state->roots[i]) != SCRATCH_NONE && !moved) {
            /* The test may still be inside it (game_files_test chdirs in),
             * and Windows will not remove the current folder. */
            moved = 1;
#ifdef _WIN32
            {
                wchar_t *wide = Memories_Utf8ToWide(state->base);
                if (wide) _wchdir(wide);
                free(wide);
            }
#else
            if (chdir(state->base)) {}
#endif
            scratch_remove(state->roots[i], 0);
        }
    }
    state->count = 0;
}

static inline void scratch_signal(int number)
{
#ifndef _WIN32
    alarm(10); /* an abort from inside malloc would deadlock opendir: give up */
#endif
    scratch_cleanup();
    signal(number, SIG_DFL);
    raise(number);
}

/* Six letters or digits, and the end: mkdtemp's suffix. */
static inline int scratch_suffix(const char *text)
{
    int i;
    for (i = 0; i < 6; i++) if (!isalnum((unsigned char)text[i])) return 0;
    return !text[6];
}

/* <kind>-p<pid>-XXXXXX for exactly this kind. A PID that does not fit
 * reads as 0, which counts as alive. */
static inline int scratch_tagged(const char *entry, const char *kind, unsigned long *pid)
{
    size_t length = strlen(kind), digits = 0;
    unsigned long long value = 0;
    const char *at = entry + length;
    if (!length || strncmp(entry, kind, length) || at[0] != '-' || at[1] != 'p') return 0;
    for (at += 2; isdigit((unsigned char)*at); at++, digits++)
        if (digits < 11) value = value * 10 + (unsigned long long)(*at - '0');
    if (!digits || *at != '-' || !scratch_suffix(at + 1)) return 0;
    *pid = digits > 10 || value > 0xFFFFFFFFull ? 0 : (unsigned long)value;
    return 1;
}

/* <kind>-XXXXXX, as builds before the process tag named them. */
static inline int scratch_legacy(const char *entry, const char *kind)
{
    size_t length = strlen(kind);
    return length && !strncmp(entry, kind, length) && entry[length] == '-' && scratch_suffix(entry + length + 1);
}

static inline int scratch_registered(const char *path)
{
    ScratchState *state = scratch_state();
    int i;
    for (i = 0; i < state->count; i++) if (!strcmp(state->roots[i], path)) return 1;
    return 0;
}

/* Is this entry of the temporary folder a leftover of `kind`? */
static inline int scratch_stale(const char *entry, const char *path, const char *kind, time_t now)
{
    unsigned long pid;
    struct stat info;
    if (scratch_tagged(entry, kind, &pid)) return !scratch_registered(path) && !scratch_alive(pid);
    return scratch_legacy(entry, kind) && !stat(path, &info) && now - info.st_mtime > SCRATCH_LEGACY_AGE;
}

/* Remove what earlier runs left in the temporary folder: every known kind
 * when `name` is NULL, else that kind only. Each removal is written to
 * `report` when given. Returns how many were removed. */
static inline int scratch_sweep(const char *name, FILE *report)
{
    const char *base = scratch_base();
    char path[2 * SCRATCH_MAX];
    DIR *dir = opendir(base);
    struct dirent *entry;
    time_t now = time(NULL);
    size_t count, i;
    const char *const *kinds = scratch_kinds(&count);
    int removed = 0;
    if (!dir) return 0;
    while ((entry = readdir(dir))) {
        int stale = 0;
        if (strncmp(entry->d_name, "memories-", 9)) continue;
        if (snprintf(path, sizeof(path), "%s/%s", base, entry->d_name) >= (int)sizeof(path)) continue;
        if (name) stale = scratch_stale(entry->d_name, path, name, now);
        for (i = 0; !name && !stale && i < count; i++) stale = scratch_stale(entry->d_name, path, kinds[i], now);
        if (!stale) continue;
#ifndef _WIN32
        /* Only our own: in a shared /tmp another user could swap an entry
         * of theirs for a link between the checks and the removal. */
        {
            struct stat own;
            if (lstat(path, &own) || own.st_uid != geteuid()) continue;
        }
#endif
        scratch_remove(path, 0);
        if (scratch_entry(path) == SCRATCH_NONE) {
            removed++;
            if (report) fprintf(report, "scratch sweep: removed %s\n", path);
        } else if (report) {
            fprintf(report, "scratch sweep: could not remove all of %s\n", path);
        }
    }
    closedir(dir);
    return removed;
}

/* mkdtemp in the temporary folder, removed again however the test ends.
 * Returns `out`, or NULL. */
static inline char *scratch_dir(char *out, size_t size, const char *name)
{
    ScratchState *state = scratch_state();
    scratch_sweep(name, NULL);
    if (state->count >= SCRATCH_ROOTS || !*name) return NULL;
    if (snprintf(out, size, "%s/%s-p%lu-XXXXXX", scratch_base(), name, scratch_pid()) >= (int)size) return NULL;
    if (strlen(out) >= SCRATCH_MAX || !mkdtemp(out)) return NULL;
    strcpy(state->roots[state->count++], out);
    if (!state->hooked) {
#ifdef _WIN32
        /* Windows never raises SIGTERM, and runs a SIGINT handler on another
         * thread while the test goes on; what those leave, the sweep takes. */
        const int numbers[] = {SIGABRT};
#else
        const int numbers[] = {SIGABRT, SIGINT, SIGTERM};
#endif
        size_t i;
        state->hooked = 1;
        state->owner = scratch_pid();
        snprintf(state->base, sizeof(state->base), "%s", scratch_base());
        atexit(scratch_cleanup);
        /* Only where the test has no handler of its own. */
        for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
            void (*previous)(int) = signal(numbers[i], scratch_signal);
            if (previous != SIG_DFL && previous != SIG_ERR) signal(numbers[i], previous);
        }
    }
    return out;
}
#endif
