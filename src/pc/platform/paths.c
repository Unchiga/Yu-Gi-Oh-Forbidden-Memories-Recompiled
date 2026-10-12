/* The two roots the port reads and writes through (paths.h). Both are
 * resolved once and cached: the program directory from the running executable, the
 * user directory from MEMORIES_USER_DIR, else portable.txt beside the
 * executable (portable mode), else the platform's own convention for a
 * game's files. */
#define _POSIX_C_SOURCE 200809L
#include "paths.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <shlobj.h> /* SHGetFolderPathW */
#endif
#include "pc/compat/posix.h" /* mkdir, and readlink of /proc/self/exe, on Windows */
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define PATH_MAX_ 1024
#define APP_NAME "YFM Re-Decomp"
#define OLD_APP_NAME "YFM ReDecomp" /* what builds before 2026-09-22 called it */

static char user_dir[PATH_MAX_];
static char program_dir[PATH_MAX_];

/* A folder made, or already there. When it is not, errno and the last error
 * are mkdir's (Paths_WriteError), not those of the check after it. */
static int make_dir(const char *path)
{
    int error;
    struct stat info;
#ifdef _WIN32
    DWORD code;
    unsigned long dos;
#endif
    if (!mkdir(path, 0777)) return 0;
    error = errno;
#ifdef _WIN32
    code = GetLastError();
    dos = _doserrno;
#endif
    /* A folder, not a file in its way (Windows' access() passes any file for X_OK). */
    if (!stat(path, &info) && S_ISDIR(info.st_mode)) return 0;
    errno = error;
#ifdef _WIN32
    _doserrno = dos;
    SetLastError(code);
#endif
    return -1;
}

int Paths_MakeDirs(const char *path)
{
    char work[PATH_MAX_];
    size_t length = strlen(path), i, first = 1;
    if (length >= sizeof(work)) return -1;
    memcpy(work, path, length + 1);
    while (length > 1 && work[length - 1] == '/') work[--length] = '\0';
#ifdef __SWITCH__
    {
        /* "sdmc:/": a device's root is no folder to make */
        const char *colon = strchr(work, ':');
        if (colon && colon[1] == '/') first = (size_t)(colon - work) + 2;
    }
#endif
    for (i = first; i < length; i++) {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (make_dir(work)) return -1;
        work[i] = '/';
    }
    if (make_dir(work)) return -1;
    return 0;
}

/* Everything above the last separator of a path. */
static void directory_of(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash) *slash = '\0';
    else path[0] = '\0';
}

#ifdef __APPLE__
/* A packaged macOS app keeps immutable game data in Contents/Resources.
 * Only use that layout when the executable really belongs to a conventional
 * .app bundle; command-line builds and ad-hoc directory layouts stay beside
 * their executable. */
static int darwin_bundle_resources(char *directory, size_t size)
{
    static const char executable_directory[] = "/Contents/MacOS";
    char bundle[PATH_MAX_], info[PATH_MAX_], resources[PATH_MAX_];
    struct stat info_stat, resources_stat;
    size_t length = strlen(directory), suffix = sizeof(executable_directory) - 1;

    if (length <= suffix || strcmp(directory + length - suffix, executable_directory)) return 0;
    length -= suffix;
    if (length < 4 || memcmp(directory + length - 4, ".app", 4)) return 0;
    if (length >= sizeof(bundle)) return 0;
    memcpy(bundle, directory, length);
    bundle[length] = '\0';

    if (snprintf(info, sizeof(info), "%s/Contents/Info.plist", bundle) >= (int)sizeof(info) ||
        snprintf(resources, sizeof(resources), "%s/Contents/Resources", bundle) >= (int)sizeof(resources) ||
        stat(info, &info_stat) || !S_ISREG(info_stat.st_mode) ||
        stat(resources, &resources_stat) || !S_ISDIR(resources_stat.st_mode) ||
        strlen(resources) >= size)
        return 0;
    memcpy(directory, resources, strlen(resources) + 1);
    return 1;
}
#endif

const char *Paths_ProgramDir(void)
{
    const char *named = getenv("MEMORIES_PROGRAM_DIR");
    if (program_dir[0]) return program_dir;
    if (named && *named && strlen(named) < sizeof(program_dir)) {
        snprintf(program_dir, sizeof(program_dir), "%s", named);
        return program_dir;
    }
#ifdef __APPLE__
    uint32_t capacity = sizeof(program_dir);
    char *executable = malloc(capacity);
    if (executable && _NSGetExecutablePath(executable, &capacity)) {
        free(executable);
        executable = malloc(capacity);
        if (executable && _NSGetExecutablePath(executable, &capacity)) { free(executable); executable = NULL; }
    }
    if (executable) {
        char *canonical = realpath(executable, NULL);
        const char *path = canonical ? canonical : executable;
        if (strlen(path) < sizeof(program_dir)) {
            memcpy(program_dir, path, strlen(path) + 1);
            directory_of(program_dir);
            darwin_bundle_resources(program_dir, sizeof(program_dir));
        }
        free(canonical); free(executable);
    }
#elif defined(__SWITCH__)
    /* Homebrew lives in a folder of its own on the SD card: the .nro, the
     * game/ and mods/ folders the player fills, and user/ (Paths_UserDir). */
    snprintf(program_dir, sizeof(program_dir), "%s", PATHS_SWITCH_DIR);
#else
    ssize_t length;
    length = readlink("/proc/self/exe", program_dir, sizeof(program_dir) - 1);
    if (length > 0 && (size_t)length < sizeof(program_dir)) {
        program_dir[length] = '\0';
        directory_of(program_dir);
    }
 #endif
    if (!program_dir[0]) snprintf(program_dir, sizeof(program_dir), ".");
    return program_dir;
}

/* Portable mode: a file named portable.txt beside the executable (what it
 * holds does not matter) keeps the player's files beside it too, in a
 * folder of their own -- mods/ beside the executable is the release's. */
static int portable(char *out, size_t size)
{
    char marker[PATH_MAX_];
    int n;
    if (Paths_Program(marker, sizeof(marker), "portable.txt") || access(marker, F_OK)) return 0;
    n = snprintf(out, size, "%s/user", Paths_ProgramDir());
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return 0;
    }
    fprintf(stderr, "memories-pc: %s found; the player's files go to %s\n", marker, out);
    return 1;
}

/* Whether a folder holds the player's saves: a save slot, or the memory
 * card image the builds before save slots kept. */
static int has_saves(const char *dir)
{
    char path[PATH_MAX_];
    int slot;
    for (slot = 1; slot <= 10; slot++) { /* SAVE_SLOT_COUNT */
        snprintf(path, sizeof(path), "%s/saves/slot%02d.sav", dir, slot);
        if (!access(path, F_OK)) return 1;
    }
    snprintf(path, sizeof(path), "%s/memcard1.mcd", dir);
    if (!access(path, F_OK)) return 1;
    snprintf(path, sizeof(path), "%s/memcard2.mcd", dir);
    return !access(path, F_OK);
}

/* saves/ beside the game, where every build has kept the player's files when
 * it could not make its own folder (Paths_UserDir), when it holds saves:
 * the working directory's, as those builds named it, else the program
 * directory's. NULL when neither does. */
static const char *legacy_saves(void)
{
    static char program[PATH_MAX_];
    if (has_saves("saves")) return "saves";
    if (snprintf(program, sizeof(program), "%s/saves", Paths_ProgramDir()) < (int)sizeof(program) &&
        has_saves(program))
        return program;
    return NULL;
}

/* `from` moved to `to` only when nothing is there: never over a file, not
 * even one that appeared since it was looked for. */
static int move_new(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *a = Memories_Utf8ToWide(from), *b = Memories_Utf8ToWide(to);
    int moved = a && b && MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH); /* no MOVEFILE_REPLACE_EXISTING */
    free(a);
    free(b);
    return moved ? 0 : -1;
#else
    if (!link(from, to)) return remove(from), 0;
    if (errno == EEXIST) return -1;
    /* A file system without links (FAT): checked, then renamed. */
    return access(to, F_OK) && !rename(from, to) ? 0 : -1;
#endif
}

/* One file, unless `to` is already there, which is never replaced: written
 * beside it and moved in, so a copy cut short is never taken for a save.
 * 0 when it is there afterwards. */
static int copy_file(const char *from, const char *to)
{
    char partial[PATH_MAX_ + 16], buffer[65536];
    FILE *in, *out;
    size_t got;
    int failed = 0;
    if (!access(to, F_OK)) return 0;
    if (snprintf(partial, sizeof(partial), "%s.copying", to) >= (int)sizeof(partial)) return -1;
    if (!(in = fopen(from, "rb"))) return -1;
    if (!(out = fopen(partial, "wb"))) {
        fclose(in);
        return -1;
    }
    while (!failed && (got = fread(buffer, 1, sizeof(buffer), in)) > 0) failed = fwrite(buffer, 1, got, out) != got;
    failed |= ferror(in);
    fclose(in);
    failed |= fclose(out) != 0;
    if (failed || move_new(partial, to)) {
        remove(partial);
        return access(to, F_OK) ? -1 : 0; /* someone else's file there is fine */
    }
    return 0;
}

/* Everything in `from` that `to` lacks, folders and all; at the top, not
 * the crash reports or the cache, which are the game's own. 0 when it all
 * arrived. The originals stay. */
static int copy_tree(const char *from, const char *to, int top)
{
    DIR *folder;
    struct dirent *item;
    int failed = 0;
    if (Paths_MakeDirs(to) || !(folder = opendir(from))) return -1;
    while ((item = readdir(folder)) != NULL) {
        char source[PATH_MAX_], destination[PATH_MAX_];
        struct stat info;
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (top && (!strcmp(item->d_name, "reports") || !strcmp(item->d_name, "cache"))) continue;
        if (snprintf(source, sizeof(source), "%s/%s", from, item->d_name) >= (int)sizeof(source) ||
            snprintf(destination, sizeof(destination), "%s/%s", to, item->d_name) >= (int)sizeof(destination) ||
            stat(source, &info)) {
            failed = 1;
            continue;
        }
        if (S_ISDIR(info.st_mode)) failed |= copy_tree(source, destination, 0) != 0;
        else failed |= copy_file(source, destination) != 0;
    }
    closedir(folder);
    return failed ? -1 : 0;
}

const char *Paths_UserDir(void)
{
    const char *named = getenv("MEMORIES_USER_DIR");
    if (user_dir[0]) return user_dir;
    if (named && *named) {
        snprintf(user_dir, sizeof(user_dir), "%s", named);
    } else if (portable(user_dir, sizeof(user_dir))) {
        /* user/ beside the executable */
#ifdef __SWITCH__
    } else if (snprintf(user_dir, sizeof(user_dir), "%s/user", Paths_ProgramDir()) > 0) {
        /* the Switch is portable mode always: there is no other folder */
#endif
    } else {
        char root[PATH_MAX_ - 32] = ""; /* room for the folder name after it */
#ifdef _WIN32
        /* Documents\My Games is where Windows games have put their files
         * since the Games for Windows era; the player can find and back it
         * up without being told where to look. Ask the shell where Documents
         * is, since OneDrive and the folder's Location tab both move it. */
        wchar_t documents[MAX_PATH];
        char *utf8 = NULL;
        const char *profile = getenv("USERPROFILE");
        if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, documents) == S_OK)
            utf8 = Memories_WideToUtf8(documents);
        if (utf8) {
            snprintf(root, sizeof(root), "%s/My Games", utf8);
            free(utf8);
        }
        else
            snprintf(root, sizeof(root), "%s/Documents/My Games", profile && *profile ? profile : ".");
#elif defined(__APPLE__)
        const char *home = getenv("HOME");
        if (home && *home) snprintf(root, sizeof(root), "%s/Library/Application Support", home);
#else
        const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg == '/') snprintf(root, sizeof(root), "%s", xdg);
        else if (home && *home) snprintf(root, sizeof(root), "%s/.local/share", home);
#endif
        if (root[0]) {
            char old[PATH_MAX_];
            snprintf(user_dir, sizeof(user_dir), "%s/" APP_NAME, root);
            snprintf(old, sizeof(old), "%s/" OLD_APP_NAME, root);
            /* Bring the old folder along under the new name, once. */
            if (access(user_dir, F_OK) && !access(old, F_OK) && !rename(old, user_dir))
                fprintf(stderr, "memories-pc: moved %s to %s\n", old, user_dir);
            /* A player whose files went beside the game (an antivirus or
             * Controlled folder access kept this folder from being made)
             * has them copied here once this folder is there to take them;
             * the originals stay. When they cannot be, the game goes on
             * with them beside it: a folder without saves never wins over
             * one with them. */
            if (!has_saves(user_dir)) {
                const char *legacy = legacy_saves();
                if (legacy && !copy_tree(legacy, user_dir, 1) && has_saves(user_dir)) {
                    fprintf(stderr, "memories-pc: copied the saves in %s to %s\n", legacy, user_dir);
                } else if (legacy) {
                    fprintf(stderr, "memories-pc: cannot copy the saves in %s to %s; using them there\n", legacy,
                            user_dir);
                    snprintf(user_dir, sizeof(user_dir), "%s", legacy);
                }
            }
        } else {
            snprintf(user_dir, sizeof(user_dir), "saves");
        }
    }
    /* A root that cannot be made is not worth carrying: fall back beside the
     * game, which is where the port kept everything before. Not on Android,
     * where "beside the game" is no folder the app can write, and where the
     * folder was made before the game started (android.c): one that is
     * gone now stays the folder, and the writes into it fail there. */
#ifdef __ANDROID__
    if (Paths_MakeDirs(user_dir)) fprintf(stderr, "memories-pc: cannot create %s: %s\n", user_dir, strerror(errno));
#else
    if (Paths_MakeDirs(user_dir)) {
        fprintf(stderr, "memories-pc: cannot create %s; using ./saves\n", user_dir);
        snprintf(user_dir, sizeof(user_dir), "saves");
        Paths_MakeDirs(user_dir);
    }
#endif
    return user_dir;
}

static int join(char *out, size_t size, const char *root, const char *relative, int create)
{
    int n = snprintf(out, size, "%s/%s", root, relative);
    if (n < 0 || (size_t)n >= size) return -1;
    if (create) {
        char parent[PATH_MAX_];
        if ((size_t)n >= sizeof(parent)) return -1;
        memcpy(parent, out, (size_t)n + 1);
        directory_of(parent);
        if (parent[0] && Paths_MakeDirs(parent)) return -1;
    }
    return 0;
}

int Paths_User(char *out, size_t size, const char *relative)
{
    return join(out, size, Paths_UserDir(), relative, 1);
}

int Paths_Program(char *out, size_t size, const char *relative)
{
    return join(out, size, Paths_ProgramDir(), relative, 0);
}

int Paths_Contained(const char *relative)
{
    const char *at = relative;
    if (!relative || !*relative || *relative == '/' || *relative == '\\') return 0;
    if (relative[0] && relative[1] == ':') return 0; /* a drive letter */
    for (;;) {
        size_t length = strcspn(at, "/");
        if (strchr(at, '\\')) return 0;
        if (!length) return 0; /* an empty component: "a//b" or a trailing slash */
        if (length == 1 && at[0] == '.') return 0;
        if (length == 2 && at[0] == '.' && at[1] == '.') return 0;
        if (!at[length]) return 1;
        at += length + 1;
    }
}

static void carry(const char *from, const char *relative)
{
    char destination[PATH_MAX_];
    char buffer[65536];
    FILE *in, *out;
    size_t got;
    if (access(from, R_OK)) return;
    if (Paths_User(destination, sizeof(destination), relative)) return;
    if (!access(destination, F_OK)) return; /* already carried, or newer */
    in = fopen(from, "rb");
    if (!in) return;
    out = fopen(destination, "wb");
    if (!out) { fclose(in); return; }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) break;
    }
    fclose(in);
    if (fclose(out)) remove(destination);
    else fprintf(stderr, "memories-pc: carried %s to %s\n", from, destination);
}

void Paths_MigrateLegacySaves(void)
{
    static const char *files[] = {"settings.txt", "controls.txt", "memcard1.mcd", "memcard2.mcd"};
    char legacy[PATH_MAX_];
    unsigned i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(legacy, sizeof(legacy), "saves/%s", files[i]);
        carry(legacy, files[i]);
    }
}

void Paths_WriteBegin(void)
{
    errno = 0;
#ifdef _WIN32
    _doserrno = 0;
    SetLastError(0);
#endif
}

#ifdef _WIN32
/* Whether a full path lies in the Documents folder, where an antivirus's
 * ransomware protection and Windows' Controlled folder access guard it. */
static int in_documents(const wchar_t *path)
{
    wchar_t documents[MAX_PATH];
    size_t n;
    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, documents) != S_OK) return 0;
    n = wcslen(documents);
    while (n && documents[n - 1] == L'\\') n--;
    return n && !_wcsnicmp(path, documents, n) && (path[n] == L'\\' || !path[n]);
}

/* The executable's file name, which the player allows in those settings. */
static void program_name(char *out, size_t size)
{
    wchar_t module[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, module, MAX_PATH);
    const wchar_t *base = module;
    char *utf8;
    snprintf(out, size, "memories-pc.exe");
    if (!length || length >= MAX_PATH) return;
    if (wcsrchr(module, L'\\')) base = wcsrchr(module, L'\\') + 1;
    utf8 = Memories_WideToUtf8(base);
    if (utf8 && *utf8) snprintf(out, size, "%s", utf8);
    free(utf8);
}
#endif

/* What the writes to the user directory have shown so far (paths.h,
 * Paths_WatchUserDir): -1 before any, then the last one's outcome. */
static void (*user_dir_watch)(int writable, const char *why);
static int user_dir_writable = -1;
static char user_dir_why[1024];

/* Whether `path` names the user directory or something in it. */
static int in_user_dir(const char *path)
{
    const char *dir = Paths_UserDir();
    size_t n = dir ? strlen(dir) : 0;
    while (n && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    return n && path && !strncmp(path, dir, n) && (path[n] == '/' || path[n] == '\\' || !path[n]);
}

/* A write's outcome under the user directory, told to the watcher only when
 * it is news (a moved window saves the settings again and again). */
static void note_user_dir(const char *path, int writable, const char *why)
{
    if (!in_user_dir(path)) return;
    if (!why) why = "";
    if (writable == user_dir_writable && !strcmp(why, user_dir_why)) return;
    user_dir_writable = writable;
    snprintf(user_dir_why, sizeof(user_dir_why), "%s", why);
    if (user_dir_watch) user_dir_watch(writable, user_dir_why);
}

void Paths_WatchUserDir(void (*watch)(int writable, const char *why)) { user_dir_watch = watch; }

void Paths_WriteDone(const char *path)
{
    int error = errno;
#ifdef _WIN32
    DWORD last = GetLastError();
#endif
    note_user_dir(path, 1, NULL);
#ifdef _WIN32
    SetLastError(last);
#endif
    errno = error;
}

/* Paths_WriteError and Paths_WriteReason: the reason, after the path when
 * `with_path` is set. */
static const char *describe(char *out, size_t size, const char *path, int with_path)
{
    int error = errno;
    char reason[512] = "", tail[1024], *shown = NULL, program[128] = "";
    size_t length;
    int hint = 0;
#ifdef _WIN32
    /* What the system said: the last error of the call that failed, or the
     * one the C runtime kept (_wfopen, fwrite and fclose go through it).
     * ERROR_ALREADY_EXISTS is what a successful CREATE_ALWAYS over an old
     * partial file leaves behind, not a failure. */
    DWORD last = GetLastError(), code = last;
    unsigned long dos = _doserrno;
    wchar_t *wide;
    if (!code || code == ERROR_ALREADY_EXISTS) code = (DWORD)dos;
    if (code == ERROR_ALREADY_EXISTS) code = 0;
    if (code) {
        wchar_t *message = NULL;
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, 0, (LPWSTR)&message, 0, NULL) && message) {
            char *utf8 = Memories_WideToUtf8(message);
            if (utf8) snprintf(reason, sizeof(reason), "%s", utf8);
            free(utf8);
        }
        if (message) LocalFree(message);
        if (!reason[0]) snprintf(reason, sizeof(reason), "Windows error %lu", (unsigned long)code);
    }
    /* The whole path, as Explorer shows it. */
    wide = Memories_Utf8ToWide(path);
    if (wide) {
        DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
        wchar_t *full = needed ? malloc((size_t)needed * sizeof(*full)) : NULL;
        if (full && GetFullPathNameW(wide, needed, full, NULL) < needed) {
            shown = Memories_WideToUtf8(full);
            hint = code == ERROR_ACCESS_DENIED && in_documents(full);
        }
        free(full);
        free(wide);
    }
    if (hint) program_name(program, sizeof(program));
    /* Left as found, so a second description of the same failure agrees. */
    SetLastError(last);
    _doserrno = dos;
#endif
    if (!reason[0]) snprintf(reason, sizeof(reason), "%s", error ? strerror(error) : "the system gave no reason");
    length = strlen(reason);
    while (length && (isspace((unsigned char)reason[length - 1]) || reason[length - 1] == '.')) reason[--length] = '\0';
    snprintf(tail, sizeof(tail), "%s%s%s%s", reason,
             hint ? " (an antivirus \"ransomware protection\" or Windows \"Controlled folder access\" may be blocking "
                    "the Documents folder; allow " : "",
             hint ? program : "", hint ? " there)." : ".");
    snprintf(out, size, "%s%s%s", with_path ? (shown ? shown : path) : "", with_path ? ": " : "", tail);
    free(shown);
    note_user_dir(path, 0, tail); /* the crash reports' "user dir" fact */
#ifdef _WIN32
    SetLastError(last); /* free() may have changed it */
#endif
    errno = error;
    return out;
}

const char *Paths_WriteError(char *out, size_t size, const char *path) { return describe(out, size, path, 1); }
const char *Paths_WriteReason(char *out, size_t size, const char *path) { return describe(out, size, path, 0); }
