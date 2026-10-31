/* Android: the thin layer the port needs there, as win32.c is Windows'
 * (notes/pc-build.md, "Android"). Everything else is the Linux code: the
 * guest image, the fault handler, the clock and the SDL window (sdl.c).
 *
 * The app is SDL's Java shell (org.libsdl.app.SDLActivity, packaged by
 * tools/pc/package_android.py). It loads libSDL3.so and libmain.so (the
 * loader, android_loader.c) and calls SDL_main on a thread of its own; that
 * loads this game, libgame.so, and runs Memories_AndroidMain, which sets up
 * what an app process differs in and runs the port's main.
 *
 * - The player's folder is the app's external files folder
 *   (/sdcard/Android/data/<package>/files): the disc image goes in its
 *   game/ folder (game_files.c), where the first run copies the one the
 *   player chooses (Platform_SelectDisc, below), showing the copy in a box;
 *   adb can put it there too. The picker is our own activity
 *   (DiscPicker.java), which writes the answer to a file in the internal
 *   files folder, so a choice made while the system ended this process is
 *   taken up by the next one instead of being lost.
 * - stdout and stderr, where the port reports, go nowhere in an app: they
 *   are forwarded to the system log (adb logcat -s memories).
 * - No crash monitor process: it re-executes the program, and an app
 *   process is the zygote's, not a program of its own. A restart
 *   (Platform_RestartGame: Apply & restart in Mods, Game > Language, the
 *   end of the credits) asks a small activity in a process of its own
 *   (Restart.java) to end this process and launch the game again.
 *   The game's own handler still writes crash reports, and the next
 *   launch offers to share or save one (android_report.c).
 * - No update check yet, and no desktop OpenGL (platform.h).
 * - The display's density as it is now (Android_Density, platform.h): the
 *   activity takes a density change itself (Display size in the system
 *   settings) and SDL's content scale keeps the starting one, so sdl.c
 *   reads it here, through JNI, to lay out again.
 * - The process ends with _exit (__wrap_exit, end_process), not through
 *   the system libraries' static destructors: the activity's threads
 *   still run.
 * - SDL_main itself is the loader's (android_loader.c, libmain.so), which
 *   loads this game, libgame.so, at the address it was linked at and calls
 *   Memories_AndroidMain.
 * - The build's own files (buildid, commit, symbols/: save states and crash
 *   reports read them beside the executable), the shipped mods and the
 *   language packs are APK assets under build/, unpacked into the app's
 *   internal files folder, program/, which becomes the program directory
 *   (MEMORIES_PROGRAM_DIR, paths.h). The player's own mods go in mods/ in
 *   the external files folder, as in the user directory on the desktop. */
#ifdef __ANDROID__
#include "pc/compat/fs.h"
#include "platform.h"
#include "paths.h"
#include "game_files.h"
#include "pc/guest/image.h"
#include <SDL3/SDL.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/ashmem.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <jni.h>
#include "pc/guest/state.h"
#include "pc/mods/hd_pack.h"
#include "pc/mods/import.h"
#include "pc/mods/mods.h"
#include "update.h"
#include "jni_guard.h" /* last: after SDL's own headers */

#define LOG_TAG "memories"

/* The release this build is (build_game32.py), "" for a development one. */
extern const char Memories_Version[];

#if __ANDROID_API__ < 30
int memories_memfd_create(const char *name, unsigned flags) /* android_compat.h */
{
    return (int)syscall(__NR_memfd_create, name, flags);
}
#endif

int memories_ashmem_create(const char *name, unsigned size) /* android_compat.h */
{
    char label[ASHMEM_NAME_LEN];
    int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    snprintf(label, sizeof(label), "%s", name);
    if (ioctl(fd, ASHMEM_SET_NAME, label) || ioctl(fd, ASHMEM_SET_SIZE, (size_t)size)) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv); /* src/pc/guest/main.c */

#ifndef MEMORIES_TRANSLATED
/* The game calls bzero (HOST_LIBC in build_game32.py); bionic has none.
 * The translated build sends it to GuestRuntime_bzero instead. */
void bzero(void *at, size_t size)
{
    memset(at, 0, size);
}
#endif

int Platform_HasDesktopGL(void)
{
    return 0; /* GLES only: sdl.c's GL renderer is desktop GL */
}

/* --- the display's density, as it is now (platform.h) ---------------- */

static int density_dpi;          /* the last read; 0 before the first */
static Uint64 density_read_at;   /* SDL_GetTicks of that read */

/* The activity's Resources.getDisplayMetrics().densityDpi, which Android
 * updates before it tells the activity of a change it takes itself; 0 when
 * it could not be read. */
static int read_density_dpi(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity, resources = NULL, metrics = NULL;
    jclass type;
    jmethodID method;
    jfieldID field;
    int dpi = 0;
    if (!env || !(activity = (jobject)SDL_GetAndroidActivity())) return 0;
    if ((type = (*env)->GetObjectClass(env, activity)) != NULL &&
        (method = (*env)->GetMethodID(env, type, "getResources", "()Landroid/content/res/Resources;")) != NULL)
        resources = (*env)->CallObjectMethod(env, activity, method);
    if (type) (*env)->DeleteLocalRef(env, type);
    if (resources && !(*env)->ExceptionCheck(env) && (type = (*env)->GetObjectClass(env, resources)) != NULL) {
        if ((method = (*env)->GetMethodID(env, type, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;")) != NULL)
            metrics = (*env)->CallObjectMethod(env, resources, method);
        (*env)->DeleteLocalRef(env, type);
    }
    if (metrics && !(*env)->ExceptionCheck(env) && (type = (*env)->GetObjectClass(env, metrics)) != NULL) {
        if ((field = (*env)->GetFieldID(env, type, "densityDpi", "I")) != NULL)
            dpi = (int)(*env)->GetIntField(env, metrics, field);
        (*env)->DeleteLocalRef(env, type);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        dpi = 0;
    }
    if (metrics) (*env)->DeleteLocalRef(env, metrics);
    if (resources) (*env)->DeleteLocalRef(env, resources);
    (*env)->DeleteLocalRef(env, activity);
    return dpi > 0 ? dpi : 0;
}

float Android_Density(void)
{
    return density_dpi > 0 ? (float)density_dpi / 160.0f : 0.0f;
}

int Android_DensityChanged(void)
{
    Uint64 now = SDL_GetTicks();
    int dpi, before = density_dpi;
    if (density_read_at && now - density_read_at < 1000) return 0;
    density_read_at = now ? now : 1;
    if (!(dpi = read_density_dpi()) || dpi == density_dpi) return 0;
    density_dpi = dpi;
    if (!before) return 0; /* the first read: what SDL started with */
    fprintf(stderr, "memories-pc: the display's density is now %d dpi (was %d): laying out again\n", dpi, before);
    return 1;
}

/* --- the disc image, through the system's file picker ----------------- */

/* SDL's file dialog: the Mods panel's .zip (below). */
typedef struct Picked {
    SDL_AtomicInt done;
    char uri[2048];
    int result; /* 1 chosen, 0 cancelled, -1 the picker failed */
} Picked;

static void SDLCALL picked(void *userdata, const char *const *files, int filter)
{
    Picked *pick = userdata;
    (void)filter;
    if (!files) pick->result = -1;
    else if (files[0] && strlen(files[0]) < sizeof(pick->uri)) {
        snprintf(pick->uri, sizeof(pick->uri), "%s", files[0]);
        pick->result = 1;
    } else if (files[0]) {
        fprintf(stderr, "memories-pc: the picker's answer is %u characters long: taken as no choice\n",
                (unsigned)strlen(files[0]));
    }
    SDL_SetAtomicInt(&pick->done, 1); /* on the Java thread: published last */
}

static int java_failed(JNIEnv *env);
static int java_ok(JNIEnv *env, const void *value);
static int start_app_activity(const char *name, int pid, const char *id, int flags);

#define DISC_TITLE "Yu-Gi-Oh! Forbidden Memories"

/* The chosen document, open: 1 when it holds the game's executable, as
 * game_files.c checks an image (SLUS_014.11 on a raw ISO 9660 image), before
 * anything is copied. *bytes is then the image's length by its volume
 * descriptor: the sectors it says the disc has, 2352 bytes each. */
static int is_the_disc(FILE *file, Sint64 *bytes)
{
    unsigned char sector[2048];
    unsigned lba, size;
    if (!GameFiles_ReadSector(file, 16, sector)) return 0;
    *bytes = (Sint64)(sector[80] | (unsigned)sector[81] << 8 | (unsigned)sector[82] << 16 |
                      (unsigned)sector[83] << 24) * 2352;
    return *bytes > 0 && GameFiles_FindFile(file, "SLUS_014.11", &lba, &size) && size > 2048 &&
           size < (4u << 20) && GameFiles_ReadSector(file, lba, sector) && !memcmp(sector, "PS-X EXE", 8);
}

/* DiscPicker.java, through JNI: the picker started, the copy's box shown
 * or taken away, the document's lasting read right given back (one, or all
 * but one). */
enum { DISC_START, DISC_PROGRESS, DISC_CLOSE, DISC_RELEASE, DISC_RELEASE_ALL };
typedef struct {
    int what;
    const char *text; /* DISC_START: the pick's id; DISC_PROGRESS: the box's
                       * line; DISC_RELEASE: the URI; DISC_RELEASE_ALL: the
                       * URI kept ("" for none) */
    int done, total;  /* DISC_PROGRESS: MB; total 0 for a moving bar */
    int ok;
} DiscCall;

static void disc_java(void *argument)
{
    DiscCall *call = argument;
    JNIEnv *env;
    jobject activity;
    jclass activity_class = NULL, loader_class = NULL, found = NULL;
    jmethodID get_loader = NULL, load = NULL, method = NULL;
    jobject loader = NULL;
    jstring name = NULL, title = NULL, text = NULL;
    int ready;
    if (call->what == DISC_START) {
        /* a task of its own, any earlier one of it cleared:
         * FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TASK */
        call->ok = start_app_activity("org.yfmredecomp.game.DiscPicker", 0, call->text, 0x10000000 | 0x00008000);
        return;
    }
    env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!activity) return;
    if ((*env)->PushLocalFrame(env, 16) < 0) {
        java_failed(env);
        (*env)->DeleteLocalRef(env, activity);
        return;
    }
    /* DiscPicker is the app's: through the activity's class loader, as
     * MessageBox (java_box). */
#define CHECKED(value) java_ok(env, (const void *)(value))
    ready = CHECKED(activity_class = (*env)->GetObjectClass(env, activity)) &&
            CHECKED(get_loader = (*env)->GetMethodID(env, activity_class, "getClassLoader",
                                                     "()Ljava/lang/ClassLoader;")) &&
            CHECKED(loader = (*env)->CallObjectMethod(env, activity, get_loader)) &&
            CHECKED(loader_class = (*env)->GetObjectClass(env, loader)) &&
            CHECKED(load = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")) &&
            CHECKED(name = (*env)->NewStringUTF(env, "org.yfmredecomp.game.DiscPicker")) &&
            CHECKED(found = (jclass)(*env)->CallObjectMethod(env, loader, load, name));
    if (ready && call->what == DISC_PROGRESS) {
        if (CHECKED(method = (*env)->GetStaticMethodID(env, found, "progress",
                                                       "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;II)V")) &&
            CHECKED(title = (*env)->NewStringUTF(env, DISC_TITLE)) &&
            CHECKED(text = (*env)->NewStringUTF(env, call->text))) {
            (*env)->CallStaticVoidMethod(env, found, method, activity, title, text, (jint)call->done, (jint)call->total);
            call->ok = !java_failed(env);
        }
    } else if (ready && call->what == DISC_CLOSE) {
        if (CHECKED(method = (*env)->GetStaticMethodID(env, found, "close", "(Landroid/app/Activity;)V"))) {
            (*env)->CallStaticVoidMethod(env, found, method, activity);
            call->ok = !java_failed(env);
        }
    } else if (ready && (call->what == DISC_RELEASE || call->what == DISC_RELEASE_ALL)) {
        if (CHECKED(method = (*env)->GetStaticMethodID(env, found,
                                                       call->what == DISC_RELEASE ? "release" : "releaseAll",
                                                       "(Landroid/content/Context;Ljava/lang/String;)V")) &&
            CHECKED(text = (*env)->NewStringUTF(env, call->text))) {
            (*env)->CallStaticVoidMethod(env, found, method, activity, text);
            call->ok = !java_failed(env);
        }
    }
#undef CHECKED
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, activity);
}

static int disc_call(int what, const char *text, int done, int total)
{
    DiscCall call;
    call.what = what;
    call.text = text ? text : "";
    call.done = done;
    call.total = total;
    call.ok = 0;
    Memories_OnHostStack(disc_java, &call); /* JNI: never from the game stack */
    return call.ok;
}

/* Copies the open document to `to` (through to.tmp): 1 when it was. `size`
 * is the document's length as its provider tells it (0 or less: not told),
 * `image` the image's length by its own volume descriptor (is_the_disc).
 * The box shows how far the copy is. */
static int copy_disc(FILE *from, Sint64 size, Sint64 image, const char *to, char *why, size_t why_size)
{
    char temporary[1100], line[160];
    static unsigned char buffer[1 << 20];
    struct statvfs space;
    char folder[1024], *slash;
    FILE *out;
    Sint64 done = 0, need = size > 0 ? size : image;
    int ok, complete;
    snprintf(folder, sizeof(folder), "%s", to);
    if ((slash = strrchr(folder, '/')) != NULL) *slash = '\0';
    snprintf(temporary, sizeof(temporary), "%s.tmp", to);
    /* A copy cut short before (the app closed, or ended by the system)
     * leaves its file: not counted against the free space, nor kept. */
    if (!remove(temporary)) fprintf(stderr, "memories-pc: removed %s, left by a copy that did not finish\n", temporary);
    if (!statvfs(folder, &space) && (Sint64)space.f_bavail * (Sint64)space.f_frsize < need + (16 << 20)) {
        snprintf(why, why_size, "There is not enough free space for the disc image: it needs %lld MB, and %lld MB "
                 "are free.", (long long)(need >> 20), (long long)((Sint64)space.f_bavail * space.f_frsize >> 20));
        return 0;
    }
    /* is_the_disc read it out of order: back to its start, which a document
     * that cannot seek refuses (rewind would not say). */
    if (fseek(from, 0, SEEK_SET)) {
        snprintf(why, why_size, "The file you chose could not be read from its start again (%s). "
                 "Copy it to the phone's Download folder and choose it there.", strerror(errno));
        return 0;
    }
    if (!(out = fopen(temporary, "wb"))) {
        snprintf(why, why_size, "Could not write the disc image to %s: %s", folder, strerror(errno));
        return 0;
    }
    snprintf(line, sizeof(line), "Copying the disc image into the game's folder: 0 of %lld MB",
             (long long)(need >> 20));
    disc_call(DISC_PROGRESS, line, 0, (int)(need >> 20));
    for (;;) {
        size_t got = fread(buffer, 1, sizeof(buffer), from);
        if (!got) break;
        if (fwrite(buffer, 1, got, out) != got) break;
        done += (Sint64)got;
        if (!(done & ((8 << 20) - 1))) {
            snprintf(line, sizeof(line), "Copying the disc image into the game's folder: %lld of %lld MB",
                     (long long)(done >> 20), (long long)(need >> 20));
            disc_call(DISC_PROGRESS, line, (int)(done >> 20), (int)(need >> 20));
        }
        if (!(done & ((64 << 20) - 1))) fprintf(stderr, "memories-pc: copying the disc image: %lld of %lld MB\n",
                                                 (long long)(done >> 20), (long long)(need >> 20));
    }
    /* All of it: the provider's length when it told one, else at least what
     * the image's own descriptor says it holds. */
    complete = size > 0 ? done == size : done >= image;
    ok = !ferror(from) && !ferror(out) && complete;
    if (fclose(out)) ok = 0;
    if (!ok || rename(temporary, to)) {
        if (ok)
            snprintf(why, why_size, "Could not copy the disc image into %s (%s).", folder, strerror(errno));
        else if (!complete && !ferror(from) && !ferror(out))
            snprintf(why, why_size, "Could not copy the disc image into %s: the file you chose ended after %lld of "
                     "%lld MB.", folder, (long long)(done >> 20), (long long)(need >> 20));
        else
            snprintf(why, why_size, "Could not copy the disc image into %s (a read or a write failed after %lld MB).",
                     folder, (long long)(done >> 20));
        remove(temporary);
        return 0;
    }
    fprintf(stderr, "memories-pc: disc image copied to %s (%lld MB)\n", to, (long long)(done >> 20));
    return 1;
}

/* DiscPicker's answer (DiscPicker.RECORD), in the internal files folder. */
enum { PICK_NONE, PICK_CHOSEN, PICK_CANCELLED, PICK_FAILED, PICK_QUIT };

static int pick_record(char *path, size_t size)
{
    const char *internal = SDL_GetAndroidInternalStoragePath();
    return internal && *internal && snprintf(path, size, "%s/disc-pick.txt", internal) < (int)size;
}

/* How old an answer left by an earlier process may be and still be taken up
 * at the start: one older than this is a choice made long ago (the copy it
 * was for was never made, or the image came another way), not taken unasked. */
#define PICK_FRESH_SECONDS (15 * 60)

/* The answer, taken (read, then removed): PICK_NONE while there is none,
 * else what it was, with the URI or the reason in *text (to free). Its first
 * line is what it was and the pick's id (DiscPicker's "id" extra): with
 * `expected`, an answer to another pick (a picker that a newer one cleared)
 * is dropped as none; without (the start), an answer of any pick is taken
 * if it is fresh (PICK_FRESH_SECONDS). */
static int take_pick(const char *record, const char *expected, char **text)
{
    enum { MOST = 1 << 16 }; /* a document's URI is far shorter */
    struct stat seen;
    FILE *file;
    char *data, *line, *id;
    size_t length;
    long long age = 0;
    int what = PICK_FAILED;
    *text = NULL;
    if (!expected && !stat(record, &seen)) age = (long long)time(NULL) - (long long)seen.st_mtime;
    if (!(file = fopen(record, "rb"))) return PICK_NONE;
    data = malloc(MOST + 1);
    length = data ? fread(data, 1, MOST, file) : 0;
    fclose(file);
    remove(record);
    if (!data) return PICK_FAILED;
    data[length] = '\0';
    if ((line = strchr(data, '\n')) != NULL) *line++ = '\0';
    else line = data + length;
    line[strcspn(line, "\r\n")] = '\0';
    if ((id = strchr(data, ' ')) != NULL) *id++ = '\0';
    else id = data + strlen(data);
    if (expected && strcmp(id, expected)) {
        fprintf(stderr, "memories-pc: an answer to an earlier pick (%s), not this one's (%s): dropped\n", id, expected);
        free(data);
        return PICK_NONE;
    }
    if (age > PICK_FRESH_SECONDS) {
        fprintf(stderr, "memories-pc: the picker's answer from before this start is %lld minutes old: dropped\n",
                age / 60);
        free(data);
        return PICK_NONE;
    }
    if (length == MOST) snprintf(data, MOST, "The chosen file's address is too long (more than 64 KB).");
    else {
        if (!strcmp(data, "chosen") && *line) what = PICK_CHOSEN;
        else if (!strcmp(data, "cancelled")) what = PICK_CANCELLED;
        memmove(data, line, strlen(line) + 1);
    }
    *text = data;
    return what;
}

/* The events so far, gone through: 1 when the app is asked to quit. */
static int drain_events(void)
{
    SDL_Event event;
    int quit = 0;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_EVENT_QUIT) quit = 1;
    return quit;
}

/* The game's activity going to the background and coming back, and the
 * app's end: SDL does not queue these, it gives them to event watchers only,
 * on the thread that pumps the events (this one). */
typedef struct {
    SDL_AtomicInt away, back, quit;
} PickWatch;

static bool SDLCALL pick_watch(void *userdata, SDL_Event *event)
{
    PickWatch *watch = userdata;
    if (event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        SDL_SetAtomicInt(&watch->away, 1);
        SDL_SetAtomicInt(&watch->back, 0);
    } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND && SDL_GetAtomicInt(&watch->away)) {
        SDL_SetAtomicInt(&watch->back, 1);
    } else if (event->type == SDL_EVENT_TERMINATING || event->type == SDL_EVENT_QUIT) {
        SDL_SetAtomicInt(&watch->quit, 1);
    }
    return true;
}

/* Waits for the picker's answer. SDL's event pump holds this thread while
 * the game's activity is in the background, so this goes on once it is in
 * front again: then the answer is there, since DiscPicker writes it before
 * it ends. The game's activity back in front without one (the player came
 * back to it from the recent apps or the launcher, not through the picker)
 * is the picker given up: PICK_NONE, the welcome again; so is a picker
 * whose task went away without a choice where the game's activity never
 * left the screen (a free-form window, split screen), which DiscPicker
 * answers "cancelled" as it ends. Only the answer to the pick `id` counts. */
static int wait_pick(const char *record, const char *id, char **text)
{
    PickWatch watch;
    Uint64 back_at = 0;
    int what = PICK_NONE;
    SDL_SetAtomicInt(&watch.away, 0);
    SDL_SetAtomicInt(&watch.back, 0);
    SDL_SetAtomicInt(&watch.quit, 0);
    if (!SDL_AddEventWatch(pick_watch, &watch))
        fprintf(stderr, "memories-pc: the picker's wait cannot see the app come back (%s)\n", SDL_GetError());
    for (;;) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_QUIT) SDL_SetAtomicInt(&watch.quit, 1);
        if (SDL_GetAtomicInt(&watch.quit)) {
            what = PICK_QUIT;
            break;
        }
        if ((what = take_pick(record, id, text)) != PICK_NONE) break;
        if (!SDL_GetAtomicInt(&watch.back)) back_at = 0;
        else if (!back_at) back_at = SDL_GetTicks();
        else if (SDL_GetTicks() - back_at > 2000) break; /* PICK_NONE */
        SDL_Delay(20);
    }
    SDL_RemoveEventWatch(pick_watch, &watch);
    return what;
}

/* The chosen document checked and copied into game/rpg-yfm.bin (`target`):
 * 1 when it was; 0 when not, the reason shown in a box. */
static int take_disc(const char *uri, char *target, size_t target_size, char *why, size_t why_size)
{
    SDL_IOStream *stream;
    FILE *file;
    Sint64 image = 0;
    char said[600];
    fprintf(stderr, "memories-pc: chosen: %s\n", uri);
    disc_call(DISC_PROGRESS, "Checking the file you chose...", 0, 0);
    stream = SDL_IOFromFile(uri, "rb");
    snprintf(said, sizeof(said), "%s", stream ? "" : SDL_GetError());
    file = stream ? SDL_GetPointerProperty(SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_STDIO_FILE_POINTER, NULL)
                  : NULL;
    if (!file || !is_the_disc(file, &image)) {
        if (!stream) {
            fprintf(stderr, "memories-pc: the chosen document could not be opened: %s\n", said);
            snprintf(why, why_size, "That file could not be opened.\n\nChoose it again, or copy it to the phone's "
                     "Download folder first and choose it there.\n\n(%s)", said);
        } else {
            snprintf(why, why_size, "That file could not be read as a Forbidden Memories (USA, SLUS-01411) disc.\n\n"
                     "Choose the raw .bin file from your .bin/.cue pair, rather than the .cue file.");
            SDL_CloseIO(stream);
        }
        disc_call(DISC_CLOSE, NULL, 0, 0);
        disc_call(DISC_RELEASE, uri, 0, 0);
        Platform_ShowError("Unable to use this ROM", why);
        return 0;
    }
    Paths_WriteBegin();
    if (Paths_User(target, target_size, "game/rpg-yfm.bin")) {
        char folder[1100], reason[1300];
        snprintf(folder, sizeof(folder), "%s/game", Paths_UserDir());
        Paths_WriteError(reason, sizeof(reason), folder); /* mkdir's reason (Paths_MakeDirs) */
        snprintf(why, why_size, "The game could not use its storage folder, so the disc image cannot be copied.\n\n"
                 "Try again, or close the game and open it again.\n\n%s", reason);
        SDL_CloseIO(stream);
        disc_call(DISC_CLOSE, NULL, 0, 0);
        disc_call(DISC_RELEASE, uri, 0, 0);
        Platform_ShowError(DISC_TITLE, why);
        return 0;
    }
    if (!copy_disc(file, SDL_GetIOSize(stream), image, target, why, why_size)) {
        SDL_CloseIO(stream);
        disc_call(DISC_CLOSE, NULL, 0, 0);
        disc_call(DISC_RELEASE, uri, 0, 0);
        Platform_ShowError("Unable to use this ROM", why);
        return 0;
    }
    SDL_CloseIO(stream);
    disc_call(DISC_CLOSE, NULL, 0, 0);
    disc_call(DISC_RELEASE, uri, 0, 0); /* the copy is the game's own now */
    return 1;
}

/* The welcome and the picker, again until a disc image is copied
 * (`target`, 1), the player quits (0) or setup cannot go on (-1, the reason
 * in `why`). `what` and `text` (taken, freed here) are an answer to take up
 * before the first welcome: PICK_CHOSEN and its URI, else PICK_NONE. */
static int select_disc(int what, char *text, const char *record, char *target, size_t target_size, char *why,
                       size_t why_size)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose disc image..."}
    };
    const SDL_MessageBoxData welcome = {
        SDL_MESSAGEBOX_INFORMATION, NULL, "Welcome to Forbidden Memories Recompiled",
        "Choose your copy of Yu-Gi-Oh! Forbidden Memories to begin.\n\n"
        "Select the .bin file of your USA disc (SLUS-01411), the .bin of a .bin/.cue pair.\n\n"
        "The game copies it into its own folder (about 500 MB), so you can remove the file you chose afterwards.",
        2, buttons, NULL
    };
    unsigned picks = 0;
    for (;; what = PICK_NONE, text = NULL) {
        if (what == PICK_NONE) {
            char id[64];
            int button = 0;
            if (!Android_ShowMessageBox(&welcome, &button)) {
                snprintf(why, why_size, "Could not open ROM setup: %s", SDL_GetError());
                return -1;
            }
            if (button != 1 || drain_events()) return 0;
            /* Each pick has its own id, so that only its answer counts: a
             * picker started earlier and cleared by this one says
             * "cancelled" as it ends, under its own. */
            snprintf(id, sizeof(id), "%d-%u-%llu", (int)getpid(), ++picks, (unsigned long long)SDL_GetTicksNS());
            remove(record);
            if (!disc_call(DISC_START, id, 0, 0)) {
                snprintf(why, why_size, "Could not open the file picker.");
                return -1;
            }
            what = wait_pick(record, id, &text);
        }
        if (what == PICK_QUIT) return 0;
        if (what == PICK_NONE) {
            fprintf(stderr, "memories-pc: back in the game without an answer from the picker: the welcome again\n");
        } else if (what == PICK_CANCELLED) { /* backed out of the picker, or its task closed */
            fprintf(stderr, "memories-pc: no disc image chosen (%s)\n", text ? text : "");
        } else if (what == PICK_FAILED) {
            fprintf(stderr, "memories-pc: the picker gave no document: %s\n", text ? text : "");
            snprintf(why, why_size, "The file picker gave back no file.\n\n%s", text ? text : "");
            Platform_ShowError("Unable to use this ROM", why);
        } else if (take_disc(text, target, target_size, why, why_size)) {
            free(text);
            return 1;
        }
        free(text);
    }
}

/* A start with the disc image copied already: an answer the picker left
 * (a picker given up and answered after all, an image put there another
 * way) and every lasting read right are left over, and go. */
static void forget_old_pick(const char *folder)
{
    char disc[1100], record[1024];
    struct stat seen;
    if (snprintf(disc, sizeof(disc), "%s/game/rpg-yfm.bin", folder) >= (int)sizeof(disc) || stat(disc, &seen)) return;
    if (pick_record(record, sizeof(record)) && !remove(record))
        fprintf(stderr, "memories-pc: the picker's answer left from before dropped: the disc image is there\n");
    disc_call(DISC_RELEASE_ALL, "", 0, 0);
}

/* First run (game_files.h): the picker offers the documents the system can
 * reach (Downloads, SD cards, cloud drives); the chosen image is checked
 * where it is and then copied into the app's game/ folder, since the right
 * to read a picked document does not outlast the app. A file that is not
 * the disc says so and the picker comes back, as on the desktop.
 *
 * The picker is DiscPicker.java's, which writes the answer to a file: an
 * answer there at the start is one the last process did not live to read
 * (the system ended it while the picker was in front), taken up at once,
 * without the welcome. The copy shows its progress in a box. */
int Platform_SelectDisc(char *path, size_t size, char *why, size_t why_size)
{
    char record[1024], target[1024];
    int result = 0;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        snprintf(why, why_size, "Could not open ROM setup: %s", SDL_GetError());
        return -1;
    }
    if (!pick_record(record, sizeof(record))) {
        snprintf(why, why_size, "Could not open ROM setup: Android gave the app no internal folder.");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -1;
    }
    /* A copy cut short (the app closed during it, or ended by the system)
     * leaves its file: its space back at once, whether or not the player
     * chooses again (copy_disc removes it too). */
    if (snprintf(target, sizeof(target), "%s/game/rpg-yfm.bin.tmp", Paths_UserDir()) < (int)sizeof(target) &&
        !remove(target))
        fprintf(stderr, "memories-pc: removed %s, left by a copy that did not finish\n", target);
    {
        /* An answer the last process did not live to read (the system ended
         * it while the picker was in front): a document is taken up at
         * once, without the welcome; anything else is dropped. The lasting
         * read rights of every other document go back (a copy cut short,
         * a pick never taken up). */
        char *text = NULL;
        int what = take_pick(record, NULL, &text);
        if (what != PICK_NONE)
            fprintf(stderr, "memories-pc: the picker's answer from before this start: %s\n",
                    what == PICK_CHOSEN ? "a document" : what == PICK_CANCELLED ? "none chosen" : "it failed");
        disc_call(DISC_RELEASE_ALL, what == PICK_CHOSEN ? text : "", 0, 0);
        if (what != PICK_CHOSEN) {
            free(text);
            text = NULL;
            what = PICK_NONE;
        }
        result = select_disc(what, text, record, target, sizeof(target), why, why_size);
    }
    if (result > 0) {
        if (strlen(target) >= size) {
            snprintf(why, why_size, "The ROM path is too long.");
            result = -1;
        } else {
            snprintf(path, size, "%s", target);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return result;
}

/* --- a mod's .zip, through the same picker (the Mods panel) ---------- */

/* Import mod... in the Mods panel (mods_window.c, ModsWindow_SetImport).
 * The picker returns while the game runs on: its answer comes on the Java
 * thread (picked, above) and the panel asks for it once per pump. The
 * chosen document is copied into the mods folder first, as the disc image
 * is: a cloud drive may hand over a stream that cannot seek, and the right
 * to read it does not outlast the app. */
static Picked mod_pick;
static int mod_picking;

int Platform_PickModZip(char *why, size_t why_size)
{
    /* Any document, as for the disc: a .zip's type is not the same with
     * every provider, and the file is checked by what it holds. */
    static const SDL_DialogFileFilter filters[] = {{"Mod (.zip)", "*"}};
    const char *test = getenv("MEMORIES_IMPORT_ZIP");
    (void)why;
    (void)why_size;
    if (mod_picking) return 0;
    memset(&mod_pick, 0, sizeof(mod_pick));
    mod_picking = 1;
    if (test && *test) {
        snprintf(mod_pick.uri, sizeof(mod_pick.uri), "%s", test);
        mod_pick.result = 1;
        SDL_SetAtomicInt(&mod_pick.done, 1);
        return 0;
    }
    SDL_ShowOpenFileDialog(picked, &mod_pick, NULL, filters, 1, NULL, false);
    return 0;
}

int Platform_PickedModZip(char *why, size_t why_size)
{
    if (!mod_picking || !SDL_GetAtomicInt(&mod_pick.done)) return 0;
    mod_picking = 0;
    if (mod_pick.result < 0) {
        snprintf(why, why_size, "Could not open the file picker: %s", SDL_GetError());
        return -2;
    }
    if (!mod_pick.result) return -1;
    fprintf(stderr, "memories-pc: mod chosen: %s\n", mod_pick.uri);
    return 1;
}

int Platform_FetchModZip(char *path, size_t size, char *why, size_t why_size)
{
    static unsigned char buffer[1 << 16];
    char folder[1024];
    SDL_IOStream *stream;
    FILE *out;
    Sint64 total = 0;
    size_t got;
    int ok = 1;
    if (Mods_InstallDirectory(folder, sizeof(folder)) ||
        snprintf(path, size, "%s/.incoming.zip", folder) >= (int)size) {
        snprintf(why, why_size, "Could not make the mods folder.");
        return 0;
    }
    Mods_ImportCleanup(folder); /* what an import cut short left */
    if (!(stream = SDL_IOFromFile(mod_pick.uri, "rb"))) {
        snprintf(why, why_size, "Could not read that file: %s", SDL_GetError());
        return 0;
    }
    if (!(out = fopen(path, "wb"))) {
        snprintf(why, why_size, "Could not write in the mods folder: %s.", strerror(errno));
        SDL_CloseIO(stream);
        return 0;
    }
    while ((got = SDL_ReadIO(stream, buffer, sizeof(buffer))) > 0) {
        if (!total && (got < 4 || memcmp(buffer, "PK", 2) || (buffer[2] != 3 && buffer[2] != 5))) {
            snprintf(why, why_size, "That file is not a .zip.");
            ok = 0;
            break;
        }
        total += (Sint64)got;
        if (total > (1 << 30)) {
            snprintf(why, why_size, "That file is too large for a mod (more than 1 GB).");
            ok = 0;
            break;
        }
        if (fwrite(buffer, 1, got, out) != got) {
            snprintf(why, why_size, "Could not copy the .zip into the mods folder: %s.", strerror(errno));
            ok = 0;
            break;
        }
    }
    if (ok && SDL_GetIOStatus(stream) == SDL_IO_STATUS_ERROR) {
        snprintf(why, why_size, "Could not read that file: %s", SDL_GetError());
        ok = 0;
    }
    if (ok && !total) {
        snprintf(why, why_size, "That file is empty.");
        ok = 0;
    }
    SDL_CloseIO(stream);
    if (fclose(out) && ok) {
        snprintf(why, why_size, "Could not copy the .zip into the mods folder: %s.", strerror(errno));
        ok = 0;
    }
    if (!ok) {
        remove(path);
        return 0;
    }
    fprintf(stderr, "memories-pc: mod .zip copied to %s (%lld bytes)\n", path, (long long)total);
    return 1;
}

#ifndef __LP64__
/* The game's memory sits at fixed addresses up to 0xB0800000 (image.c).
 * A 32-bit process has 4 GB to place them in on a 64-bit kernel (phones
 * that still run 32-bit apps), and only 3 GB on a 32-bit kernel, where the
 * system's libraries already fill the top of it before the game runs. The
 * failed mapping is what counts; the process's own stack, which the
 * kernel puts at the top of that space, only tells which it was: past
 * 0xC0000000 there is room for 4 GB. (uname is no help: a 32-bit process
 * on a 64-bit x86 kernel is told "i686".) */
static int four_gigabytes(void)
{
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned long start, end;
    int yes = -1;
    if (!maps) return -1;
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "[stack]") && sscanf(line, "%lx-%lx", &start, &end) == 2) {
            yes = end > 0xC0000000ul;
            break;
        }
    }
    fclose(maps);
    return yes;
}
#endif

/* printf onto a growing text (*text NULL at first; NULL again, and nothing
 * more added, once memory ran out). */
static void add_detail(char **text, size_t *used, size_t *room, const char *format, ...)
{
    va_list arguments;
    int length;
    for (;;) {
        if (!*text && *used) return; /* out of memory earlier */
        if (*text) {
            va_start(arguments, format);
            length = vsnprintf(*text + *used, *room - *used, format, arguments);
            va_end(arguments);
            if (length < 0) return;
            if ((size_t)length < *room - *used) {
                *used += (size_t)length;
                return;
            }
        }
        {
            size_t wanted = *room ? *room * 2 : 16384;
            char *grown = realloc(*text, wanted);
            if (!grown) {
                free(*text);
                *text = NULL;
                *used = 1;
                return;
            }
            if (!*text) grown[0] = '\0';
            *text = grown;
            *room = wanted;
        }
    }
}

int Platform_GuestMemoryHelp(char *why, size_t size)
{
    const char *failed = Memories_GuestMapError();
#ifndef __LP64__
    /* Only a 32-bit game (android-x86, for development) can meet a 32-bit
     * kernel; the app's arm64 game always has the 64-bit address space. */
    if (four_gigabytes() == 0) {
        fprintf(stderr, "memories-pc: this is a 32-bit kernel (3 GB for the app): the guest memory cannot be placed\n");
        snprintf(why, size, "This Android is 32-bit.\n\n"
                 "The game needs a 64-bit Android that can still run 32-bit apps: on a 32-bit system there is no "
                 "room for the memory the game runs in.");
        return 1;
    }
#endif
    {
        /* What the player sees (a screenshot of it is enough to go on) and
         * the report Share and Save hand out, with the process's mappings
         * below 4 GB, where the guest's fixed ranges are: who holds them
         * (an app's Java runtime, notes/pc-build.md "Android arm64") is the
         * whole question. */
        const char *occupants = Memories_GuestMapOccupants();
        char maker[PROP_VALUE_MAX], model[PROP_VALUE_MAX], release[PROP_VALUE_MAX], heap[PROP_VALUE_MAX];
        char limit[PROP_VALUE_MAX], uffd[PROP_VALUE_MAX], message[900], text[512], range[48], holder[200];
        char lead[300], facts[480];
        char version[sizeof(((struct utsname *)0)->release)];
        struct utsname kernel;
        int refuses = Memories_KernelRefusesTakenRange(), shown, start_skip = 0, name_at = 0, parsed;
        unsigned low = 0, high = 0;
        size_t used = 0, room = 0;
        char *details = NULL;
        FILE *maps;
#define PROPERTY(name, out) (out[0] = '\0', __system_property_get(name, out), out[0] ? out : "?")
        if (uname(&kernel)) {
            snprintf(kernel.release, sizeof(kernel.release), "?");
            snprintf(kernel.machine, sizeof(kernel.machine), "?");
        }
        /* Short: the system's box does not scroll in landscape, and its
         * buttons sit under the text. The kernel's version without its
         * build suffix; of the first holder ("80000000-80200000 rw-p
         * [anon:dalvik-free list large object space]") its range and name. */
        snprintf(version, sizeof(version), "%.*s", (int)strcspn(kernel.release, "-"), kernel.release);
        parsed = sscanf(failed, "the addresses from 0x%x to 0x%x", &low, &high) == 2;
        snprintf(range, sizeof(range), "0x%08X-0x%08X", low, high);
        snprintf(holder, sizeof(holder), "%.*s", (int)strcspn(occupants, "\n"), occupants);
        if (sscanf(holder, "%*x-%*x%n %*s %n", &start_skip, &name_at) >= 0 && start_skip > 0 && name_at > 0 &&
            (size_t)name_at < strlen(holder)) {
            const char *name = holder + name_at;
            size_t length = strlen(name);
            if (!strncmp(name, "[anon:", 6) && length > 7 && name[length - 1] == ']') name += 6, length -= 7;
            snprintf(text, sizeof(text), "%.*s %.*s", start_skip, holder, (int)length, name);
            snprintf(holder, sizeof(holder), "%s", text);
        }
        /* A range that something holds, or another failure (no shared
         * memory for the game's RAM, a refused mapping), told as it is. */
        if (parsed && strstr(failed, "already holds"))
            snprintf(lead, sizeof(lead), "%s already uses memory this game needs (%s).",
                     strstr(occupants, "dalvik") ? "Android's Java runtime" : "Something else in the app", range);
        else
            snprintf(lead, sizeof(lead), "The game could not set up its memory: %s.",
                     *failed ? failed : "the reason is in the report");
        snprintf(facts, sizeof(facts), "%s %s, Android %s, Linux %s, heap %s\nHeld by %s",
                 PROPERTY("ro.product.manufacturer", maker), PROPERTY("ro.product.model", model),
                 PROPERTY("ro.build.version.release", release), version, PROPERTY("dalvik.vm.heapsize", heap),
                 holder[0] ? holder : "nothing found there now");
        snprintf(message, sizeof(message), "%s\nPlease tap Share and send the report to us on Discord or GitHub.\n\n%s",
                 lead, facts);
        add_detail(&details, &used, &room, "failed step: %s\n", *failed ? failed : "(none recorded)");
        add_detail(&details, &used, &room, "kernel: %s %s; MAP_FIXED_NOREPLACE %s\n", kernel.release,
                   kernel.machine, refuses > 0 ? "known (4.17+)"
                                   : refuses == 0 ? "not known (before 4.17: taken as a hint)" : "not probed");
        add_detail(&details, &used, &room, "dalvik.vm.heapsize %s, heapgrowthlimit %s, enable_uffd_gc %s\n", heap,
                   PROPERTY("dalvik.vm.heapgrowthlimit", limit), PROPERTY("ro.dalvik.vm.enable_uffd_gc", uffd));
        add_detail(&details, &used, &room, "held by:\n%s", occupants[0] ? occupants : "(nothing found there now)\n");
        add_detail(&details, &used, &room, "\nthe process's mappings below 4 GB (/proc/self/maps):\n");
        if ((maps = fopen("/proc/self/maps", "r")) != NULL) {
            unsigned long long start;
            while (fgets(text, sizeof(text), maps))
                if (sscanf(text, "%llx-", &start) == 1 && start < 0x100000000ull)
                    add_detail(&details, &used, &room, "%s", text);
            fclose(maps);
        }
#undef PROPERTY
        shown = Android_OfferStartupReport("The game cannot start", message,
                                           details ? details : "(no memory for the details)");
        free(details);
        if (shown) return 0;
        /* No box with buttons (a scripted run, or none could be shown):
         * the caller's error box, whose text is all there is. */
        snprintf(why, size, "%s\nPlease send a screenshot of this to us on Discord or GitHub.\n\n%s", lead, facts);
    }
    return 1;
}

/* JNI from here on: a pending Java exception is cleared and is a failure. */
static int java_failed(JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return 1;
}

/* A JNI call's result, good when it is not NULL and no exception is
 * pending: the exception is looked for even when the call gave NULL (a
 * failed call does both), so none is left pending for the next call. */
static int java_ok(JNIEnv *env, const void *value)
{
    return !java_failed(env) && value != NULL;
}

/* Starts the app's activity `name` (Restart.java's, a process of its own,
 * with this process's id as the "pid" extra; DiscPicker.java's, with the
 * pick's id as the "id" extra) with the Intent flags `flags`: 1 when the
 * system took the request. pid 0 and id NULL put no extra. */
static int start_app_activity(const char *name, int pid, const char *id, int flags)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    jclass intent_class = NULL, activity_class = NULL;
    jmethodID make = NULL, set_class = NULL, put_int = NULL, put_text = NULL, add_flags = NULL, start = NULL;
    jobject intent = NULL;
    jstring class_name = NULL, key = NULL, id_key = NULL, id_text = NULL;
    int ok = 0;
    if (!activity) return 0;
    if ((*env)->PushLocalFrame(env, 16) < 0) {
        java_failed(env);
        (*env)->DeleteLocalRef(env, activity);
        return 0;
    }
    /* Each call is checked before the next: a JNI call with an exception
     * pending aborts the process under CheckJNI. */
#define CHECKED(value) java_ok(env, (const void *)(value))
    if (CHECKED(intent_class = (*env)->FindClass(env, "android/content/Intent")) &&
        CHECKED(make = (*env)->GetMethodID(env, intent_class, "<init>", "()V")) &&
        CHECKED(set_class = (*env)->GetMethodID(env, intent_class, "setClassName",
                                                "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;")) &&
        CHECKED(put_int = (*env)->GetMethodID(env, intent_class, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;")) &&
        CHECKED(put_text = (*env)->GetMethodID(env, intent_class, "putExtra",
                                               "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;")) &&
        CHECKED(add_flags = (*env)->GetMethodID(env, intent_class, "addFlags", "(I)Landroid/content/Intent;")) &&
        CHECKED(activity_class = (*env)->GetObjectClass(env, activity)) &&
        CHECKED(start = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V")) &&
        CHECKED(intent = (*env)->NewObject(env, intent_class, make)) &&
        CHECKED(class_name = (*env)->NewStringUTF(env, name)) &&
        CHECKED(key = (*env)->NewStringUTF(env, "pid")) &&
        CHECKED((*env)->CallObjectMethod(env, intent, set_class, activity, class_name)) &&
        (pid <= 0 || CHECKED((*env)->CallObjectMethod(env, intent, put_int, key, (jint)pid))) &&
        (!id || (CHECKED(id_key = (*env)->NewStringUTF(env, "id")) && CHECKED(id_text = (*env)->NewStringUTF(env, id)) &&
                 CHECKED((*env)->CallObjectMethod(env, intent, put_text, id_key, id_text)))) &&
        CHECKED((*env)->CallObjectMethod(env, intent, add_flags, (jint)flags))) {
        (*env)->CallVoidMethod(env, activity, start, intent);
        ok = !java_failed(env);
    }
#undef CHECKED
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

static void restart_on_host(void *result)
{
    if (!start_app_activity("org.yfmredecomp.game.Restart", (int)getpid(), NULL, 0x10000000)) { /* FLAG_ACTIVITY_NEW_TASK */
        fprintf(stderr, "memories-pc: restart: the restart activity did not start; close the app and open it again\n");
        *(int *)result = -1;
        return;
    }
    /* What the game keeps is on disk already (settings and mod choices are
     * saved before a restart is asked for, memory cards as they are
     * written); the activity ends this process from its own. */
    fprintf(stderr, "memories-pc: restarting: the game starts again in a new process\n");
    fflush(stdout);
    for (int i = 0; i < 1000; i++) /* 10 s */
        usleep(10000);
    fprintf(stderr, "memories-pc: restart: this process was not ended; close the app and open it again\n");
    *(int *)result = -1;
}

int Platform_RestartGame(void)
{
    int result = 0;
    Memories_OnHostStack(restart_on_host, &result); /* JNI: never from the game stack */
    return result;
}

/* --- message boxes, through MessageBox.java (platform.h) -------------- */

typedef struct {
    const SDL_MessageBoxData *data;
    int button;
    int shown;
} JavaBox;

/* The box in the system's alert (MessageBox.show): box->shown stays 0 when
 * it could not be, and SDL's is shown instead. */
static void java_box(void *argument)
{
    JavaBox *box = argument;
    const SDL_MessageBoxData *data = box->data;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    jclass activity_class = NULL, loader_class = NULL, string_class = NULL, found = NULL;
    jmethodID get_loader = NULL, load = NULL, show = NULL;
    jobject loader = NULL;
    jstring name = NULL, title = NULL, message = NULL;
    jobjectArray texts = NULL;
    jintArray ids = NULL, flags = NULL;
    jint id[3], flag[3];
    int i, ok;
    if (!activity) return;
    if (data->numbuttons < 1 || data->numbuttons > 3 || (*env)->PushLocalFrame(env, 24) < 0) {
        java_failed(env);
        (*env)->DeleteLocalRef(env, activity);
        return;
    }
    for (i = 0; i < data->numbuttons; i++) {
        id[i] = data->buttons[i].buttonID;
        flag[i] = (jint)data->buttons[i].flags;
    }
    /* MessageBox is the app's: a thread native code attached finds only the
     * system's classes with FindClass, so it comes through the activity's
     * class loader, as HdDownload does. */
#define CHECKED(value) java_ok(env, (const void *)(value))
    ok = CHECKED(activity_class = (*env)->GetObjectClass(env, activity)) &&
         CHECKED(get_loader = (*env)->GetMethodID(env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;")) &&
         CHECKED(loader = (*env)->CallObjectMethod(env, activity, get_loader)) &&
         CHECKED(loader_class = (*env)->GetObjectClass(env, loader)) &&
         CHECKED(load = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")) &&
         CHECKED(name = (*env)->NewStringUTF(env, "org.yfmredecomp.game.MessageBox")) &&
         CHECKED(found = (jclass)(*env)->CallObjectMethod(env, loader, load, name)) &&
         CHECKED(show = (*env)->GetStaticMethodID(env, found, "show",
                                                  "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;"
                                                  "[Ljava/lang/String;[I[I)I")) &&
         CHECKED(string_class = (*env)->FindClass(env, "java/lang/String")) &&
         CHECKED(title = (*env)->NewStringUTF(env, data->title ? data->title : "")) &&
         CHECKED(message = (*env)->NewStringUTF(env, data->message ? data->message : "")) &&
         CHECKED(texts = (*env)->NewObjectArray(env, data->numbuttons, string_class, NULL)) &&
         CHECKED(ids = (*env)->NewIntArray(env, data->numbuttons)) &&
         CHECKED(flags = (*env)->NewIntArray(env, data->numbuttons));
    for (i = 0; ok && i < data->numbuttons; i++) {
        jstring text = NULL;
        ok = CHECKED(text = (*env)->NewStringUTF(env, data->buttons[i].text ? data->buttons[i].text : ""));
        if (ok) {
            (*env)->SetObjectArrayElement(env, texts, i, text);
            ok = !java_failed(env);
        }
    }
#undef CHECKED
    if (ok) {
        (*env)->SetIntArrayRegion(env, ids, 0, data->numbuttons, id);
        (*env)->SetIntArrayRegion(env, flags, 0, data->numbuttons, flag);
        ok = !java_failed(env);
    }
    if (ok) {
        jint chosen = (*env)->CallStaticIntMethod(env, found, show, activity, title, message, texts, ids, flags);
        if (!java_failed(env) && chosen != -2) {
            box->button = (int)chosen;
            box->shown = 1;
        }
    } else {
        fprintf(stderr, "memories-pc: MessageBox.java could not show the box: SDL's instead\n");
    }
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, activity);
}

int Android_ShowMessageBox(const SDL_MessageBoxData *data, int *button)
{
    JavaBox box;
    box.data = data;
    box.button = -1;
    box.shown = 0;
    Memories_OnHostStack(java_box, &box); /* JNI: never from the game stack */
    if (!box.shown) return SDL_ShowMessageBox(data, button);
    /* as SDL_ShowMessageBox does: a key held when the box came up is let
     * go, not left down in SDL's state */
    SDL_ResetKeyboard();
    if (button) *button = box.button;
    return 1;
}

/* --- the HD pack's download (hd_pack.h), through Java ------------------- */

/* HdNet over HdDownload.java: HttpURLConnection for the question to
 * GitHub's API, the system's DownloadManager for the pack (it goes on
 * while the app is in the background, which an app's own connection does
 * not on Android 15). hd_pack.c calls these on its job's threads only,
 * threads of its own (pthread_create, their own stacks), never the game's:
 * SDL attaches each to the VM on its first JNI use and detaches it when it
 * ends. The game thread makes no JNI call for the download at all; it
 * reads the job's atomic counters. A thread that native code attached
 * finds only the system's classes with FindClass, so HdDownload comes
 * through the activity's class loader, once, with the application's
 * context (DownloadManager's). */
typedef struct {
    jobject self;     /* global: the HdDownload */
    jbyteArray bytes; /* global: what its read() fills */
} HdStream;

#define HD_BYTES (64 << 10)
static jclass hd_class;    /* global */
static jobject hd_context; /* global: the application */
static jmethodID hd_open, hd_read, hd_close, hd_fetch, hd_poll, hd_where, hd_stop, hd_forget, hd_sha256;
static jfieldID hd_error, hd_status;

/* A JNI call's result, good when no exception is pending (one that is, is
 * cleared): every call is checked, so none is left pending for the next
 * call (CheckJNI aborts) or for the thread's detach (which hands it to the
 * uncaught-exception handler, ending the app). */
static int hd_ok(JNIEnv *env, const void *value)
{
    return !java_failed(env) && value != NULL;
}

static JNIEnv *hd_java(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity, loader = NULL, found = NULL, context = NULL;
    jclass activity_class = NULL, loader_class = NULL;
    jmethodID get_loader = NULL, load = NULL, get_context = NULL;
    jstring name = NULL;
    if (!env || hd_class) return env;
    if (!(activity = (jobject)SDL_GetAndroidActivity())) return NULL;
    if ((*env)->PushLocalFrame(env, 16) < 0) {
        java_failed(env);
        (*env)->DeleteLocalRef(env, activity);
        return NULL;
    }
#define CHECKED(value) hd_ok(env, (const void *)(value))
#define METHOD(out, kind, method, signature) CHECKED(out = (*env)->kind(env, found, method, signature))
    if (CHECKED(activity_class = (*env)->GetObjectClass(env, activity)) &&
        CHECKED(get_loader = (*env)->GetMethodID(env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;")) &&
        CHECKED(get_context = (*env)->GetMethodID(env, activity_class, "getApplicationContext",
                                                  "()Landroid/content/Context;")) &&
        CHECKED(context = (*env)->CallObjectMethod(env, activity, get_context)) &&
        CHECKED(loader = (*env)->CallObjectMethod(env, activity, get_loader)) &&
        CHECKED(loader_class = (*env)->GetObjectClass(env, loader)) &&
        CHECKED(load = (*env)->GetMethodID(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")) &&
        CHECKED(name = (*env)->NewStringUTF(env, "org.yfmredecomp.game.HdDownload")) &&
        CHECKED(found = (*env)->CallObjectMethod(env, loader, load, name)) &&
        METHOD(hd_open, GetStaticMethodID, "open", "(Ljava/lang/String;)Lorg/yfmredecomp/game/HdDownload;") &&
        METHOD(hd_read, GetMethodID, "read", "([BI)I") && METHOD(hd_close, GetMethodID, "close", "()V") &&
        METHOD(hd_fetch, GetStaticMethodID, "fetch",
               "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J") &&
        METHOD(hd_poll, GetStaticMethodID, "poll", "(Landroid/content/Context;J)[J") &&
        METHOD(hd_where, GetStaticMethodID, "where", "(Landroid/content/Context;J)Ljava/lang/String;") &&
        METHOD(hd_stop, GetStaticMethodID, "stop", "(Landroid/content/Context;J)V") &&
        METHOD(hd_forget, GetStaticMethodID, "forget", "(Landroid/content/Context;)V") &&
        METHOD(hd_sha256, GetStaticMethodID, "sha256", "(Ljava/lang/String;)Ljava/lang/String;") &&
        CHECKED(hd_error = (*env)->GetFieldID(env, found, "error", "I")) &&
        CHECKED(hd_status = (*env)->GetFieldID(env, found, "status", "I")) &&
        CHECKED(hd_context = (*env)->NewGlobalRef(env, context)))
        hd_class = (jclass)(*env)->NewGlobalRef(env, found);
#undef METHOD
#undef CHECKED
    (*env)->PopLocalFrame(env, NULL);
    (*env)->DeleteLocalRef(env, activity);
    if (!hd_class) fprintf(stderr, "memories-pc: HD pack: HdDownload.java is not in the app\n");
    return hd_class ? env : NULL;
}

static void hd_drop(JNIEnv *env, HdStream *stream)
{
    if (stream->self) {
        (*env)->CallVoidMethod(env, stream->self, hd_close);
        java_failed(env);
        (*env)->DeleteGlobalRef(env, stream->self);
    }
    if (stream->bytes) (*env)->DeleteGlobalRef(env, stream->bytes);
    free(stream);
}

static int hd_net_open(const char *url, void **out, int *status)
{
    JNIEnv *env = hd_java();
    HdStream *stream;
    jstring text;
    jobject self;
    jbyteArray bytes;
    int error;
    *out = NULL;
    *status = 0;
    if (!env || !(stream = calloc(1, sizeof(*stream)))) return HD_NET_FAILED;
    if (!hd_ok(env, text = (*env)->NewStringUTF(env, url))) {
        free(stream);
        return HD_NET_FAILED;
    }
    self = (*env)->CallStaticObjectMethod(env, hd_class, hd_open, text);
    (*env)->DeleteLocalRef(env, text);
    if (!hd_ok(env, self)) {
        free(stream);
        return HD_NET_FAILED;
    }
    stream->self = (*env)->NewGlobalRef(env, self);
    (*env)->DeleteLocalRef(env, self);
    error = (*env)->GetIntField(env, stream->self, hd_error);
    *status = (*env)->GetIntField(env, stream->self, hd_status);
    if (!error && hd_ok(env, bytes = (*env)->NewByteArray(env, HD_BYTES))) {
        stream->bytes = (jbyteArray)(*env)->NewGlobalRef(env, bytes);
        (*env)->DeleteLocalRef(env, bytes);
    }
    if (error || !stream->bytes) {
        hd_drop(env, stream);
        return error ? error : HD_NET_FAILED;
    }
    *out = stream;
    return 0;
}

static long hd_net_read(void *data, unsigned char *buffer, size_t size)
{
    HdStream *stream = data;
    JNIEnv *env = hd_java();
    jint got;
    if (!env) return HD_NET_FAILED;
    got = (*env)->CallIntMethod(env, stream->self, hd_read, stream->bytes, (jint)(size < HD_BYTES ? size : HD_BYTES));
    if (java_failed(env)) return HD_NET_FAILED;
    if (got > 0) {
        (*env)->GetByteArrayRegion(env, stream->bytes, 0, got, (jbyte *)buffer);
        if (java_failed(env)) return HD_NET_FAILED;
    }
    return got;
}

static void hd_net_close(void *data)
{
    JNIEnv *env = hd_java();
    if (data && env) hd_drop(env, data);
}

static int hd_net_fetch(const char *url, const char *path, long long *id)
{
    JNIEnv *env = hd_java();
    jstring text = NULL, file = NULL, title = NULL;
    jlong got = HD_NET_FAILED;
    if (!env) return HD_NET_FAILED;
    if (hd_ok(env, text = (*env)->NewStringUTF(env, url)) && hd_ok(env, file = (*env)->NewStringUTF(env, path)) &&
        hd_ok(env, title = (*env)->NewStringUTF(env, "YFM Re-Decomp: HD pack"))) {
        got = (*env)->CallStaticLongMethod(env, hd_class, hd_fetch, hd_context, text, file, title);
        if (java_failed(env)) got = HD_NET_FAILED;
    }
    if (text) (*env)->DeleteLocalRef(env, text);
    if (file) (*env)->DeleteLocalRef(env, file);
    if (title) (*env)->DeleteLocalRef(env, title);
    if (got < 0) return got == HD_NET_NO_MANAGER ? HD_NET_NO_MANAGER : HD_NET_FAILED;
    *id = (long long)got;
    fprintf(stderr, "memories-pc: HD pack: the download manager took it (id %lld)\n", *id);
    return 0;
}

static int hd_net_poll(long long id, unsigned long *done, int *reason)
{
    JNIEnv *env = hd_java();
    jlongArray answer;
    jlong values[3] = {HD_FETCH_FAILED, 0, 1000};
    if (!env) return HD_FETCH_FAILED;
    answer = (jlongArray)(*env)->CallStaticObjectMethod(env, hd_class, hd_poll, hd_context, (jlong)id);
    if (hd_ok(env, answer)) {
        (*env)->GetLongArrayRegion(env, answer, 0, 3, values);
        java_failed(env);
    }
    if (answer) (*env)->DeleteLocalRef(env, answer);
    *done = values[1] > 0 ? (unsigned long)values[1] : 0;
    *reason = (int)values[2];
    return (int)values[0];
}

static int hd_net_where(long long id, char *path, size_t size)
{
    JNIEnv *env = hd_java();
    jstring found;
    const char *text;
    path[0] = 0;
    if (!env) return 0;
    found = (jstring)(*env)->CallStaticObjectMethod(env, hd_class, hd_where, hd_context, (jlong)id);
    if (!hd_ok(env, found)) return 0;
    if ((text = (*env)->GetStringUTFChars(env, found, NULL))) {
        snprintf(path, size, "%s", text);
        (*env)->ReleaseStringUTFChars(env, found, text);
    } else
        java_failed(env);
    (*env)->DeleteLocalRef(env, found);
    return path[0] != 0;
}

static void hd_net_stop(long long id)
{
    JNIEnv *env = hd_java();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, hd_class, hd_stop, hd_context, (jlong)id);
    java_failed(env);
}

static void hd_net_forget(void)
{
    JNIEnv *env = hd_java();
    if (!env) return;
    (*env)->CallStaticVoidMethod(env, hd_class, hd_forget, hd_context);
    java_failed(env);
}

static int hd_net_sha256(const char *path, char *out)
{
    JNIEnv *env = hd_java();
    jstring file, hex = NULL;
    const char *text;
    out[0] = 0;
    if (!env || !hd_ok(env, file = (*env)->NewStringUTF(env, path))) return 0;
    hex = (jstring)(*env)->CallStaticObjectMethod(env, hd_class, hd_sha256, file);
    (*env)->DeleteLocalRef(env, file);
    if (!hd_ok(env, hex)) return 0;
    if ((text = (*env)->GetStringUTFChars(env, hex, NULL))) {
        snprintf(out, 65, "%s", text);
        (*env)->ReleaseStringUTFChars(env, hex, text);
    } else
        java_failed(env);
    (*env)->DeleteLocalRef(env, hex);
    return strlen(out) == 64;
}

/* The download's network, for HdPack_SetNet (sdl.c, on the game thread:
 * nothing here calls Java). MEMORIES_HD_TEST_SHA256=<hex> (environment.txt)
 * makes a download need that SHA-256 instead of the release's, to see the
 * damaged-download path; a release build (one with a version) ignores it. */
const HdNet *Platform_HdNet(void)
{
    static HdNet net = {hd_net_open, hd_net_read, hd_net_close,  hd_net_fetch,  hd_net_poll,
                        hd_net_where, hd_net_stop, hd_net_sha256, hd_net_forget, NULL};
    static int once;
    const char *test = getenv("MEMORIES_HD_TEST_SHA256");
    if (once++) return &net; /* set before any job's thread reads it */
    if (test && *test) {
        if (Update_ParseVersion(Memories_Version, NULL))
            fprintf(stderr, "memories-pc: MEMORIES_HD_TEST_SHA256 is ignored in a release build\n");
        else
            net.test_sha256 = test;
    }
    return &net;
}

/* --- the screen's rotation --------------------------------------------- */

/* SDL asks for the activity's orientation when it makes the window, from
 * SDL_HINT_ORIENTATIONS (Memories_AndroidMain): both landscapes give
 * USER_LANDSCAPE, which turns over only while the system's auto-rotate is
 * on, so with it off (a phone's rotation lock) a phone turned upside down
 * showed the picture upside down. SDL gives SENSOR_LANDSCAPE only without
 * the hint and for a window that cannot be resized, and the port's can, so
 * the activity is asked here instead, after the window is made and when
 * the setting changes (sdl.c, apply_display_settings): SENSOR_LANDSCAPE
 * follows the sensor to either landscape whatever auto-rotate says, as most
 * landscape games do; USER_LANDSCAPE is SDL's own request. SDL asks again
 * only when a window is made or made resizable (SDL_androidwindow.c), which
 * the port does once. Turning over by half a circle changes no
 * configuration: the activity stays, and so does the surface's size. */
void Android_ApplyScreenRotation(int follow_system)
{
    enum { SENSOR_LANDSCAPE = 6, USER_LANDSCAPE = 11 }; /* ActivityInfo.SCREEN_ORIENTATION_* */
    static int asked = -1;
    int wanted = follow_system ? USER_LANDSCAPE : SENSOR_LANDSCAPE;
    JNIEnv *env;
    jobject activity;
    jclass type;
    jmethodID request;
    if (wanted == asked) return;
    env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!activity) {
        fprintf(stderr, "memories-pc: screen rotation: no activity (%s)\n", SDL_GetError());
        return;
    }
    type = (*env)->GetObjectClass(env, activity);
    request = (*env)->GetMethodID(env, type, "setRequestedOrientation", "(I)V");
    if (request) (*env)->CallVoidMethod(env, activity, request, wanted);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        request = NULL;
    }
    (*env)->DeleteLocalRef(env, type);
    (*env)->DeleteLocalRef(env, activity);
    if (!request) {
        fprintf(stderr, "memories-pc: screen rotation: setRequestedOrientation(%d) failed\n", wanted);
        return;
    }
    asked = wanted;
    fprintf(stderr, "memories-pc: screen rotation: %s (requested orientation %d)\n",
            follow_system ? "follows auto-rotate" : "turns with the phone", wanted);
}

static int log_pipe[2] = {-1, -1};
static int log_forwarding; /* 1 while forward_log runs (__atomic) */

static void *forward_log(void *unused)
{
    char buffer[1024];
    size_t used = 0;
    ssize_t got;
    (void)unused;
    for (;;) {
        char *line = buffer, *end;
        got = read(log_pipe[0], buffer + used, sizeof(buffer) - 1 - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break; /* the end of the pipe: drain_log closed its last writer */
        used += (size_t)got;
        buffer[used] = '\0';
        while ((end = memchr(line, '\n', used - (size_t)(line - buffer))) != NULL) {
            *end = '\0';
            __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
            line = end + 1;
        }
        used -= (size_t)(line - buffer);
        memmove(buffer, line, used);
        if (used == sizeof(buffer) - 1) { /* a line longer than the buffer: in pieces */
            buffer[used] = '\0';
            __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buffer);
            used = 0;
        }
    }
    if (used) { /* the last words, without their line end */
        buffer[used] = '\0';
        __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buffer);
    }
    __atomic_store_n(&log_forwarding, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* The reader starts first, with the game clock (SIGALRM) blocked, as it
 * must reach the game's thread, and only then do stdout and stderr go into
 * the pipe: without a reader, a full pipe would stop the game, and its end
 * (fflush) for good. A fault in the reader still reaches crash.c's handler
 * (its report goes to the report file, not to logcat: stderr is this pipe,
 * which no one reads any more; with the pipe full, the handler would stop
 * at its first write),
 * and read retries on EINTR. The pipe's own two descriptors are close on
 * exec; stdout and stderr, which point into it, are not, so a child would
 * keep the pipe open past drain_log's wait (200 ms). Nothing starts one on
 * Android. */
static void log_to_logcat(void)
{
    pthread_t thread;
    sigset_t held, previous;
    int started;
    if (pipe(log_pipe)) {
        log_pipe[0] = log_pipe[1] = -1;
        return;
    }
    fcntl(log_pipe[0], F_SETFD, FD_CLOEXEC); /* pipe2 needs _GNU_SOURCE in bionic */
    fcntl(log_pipe[1], F_SETFD, FD_CLOEXEC);
    __atomic_store_n(&log_forwarding, 1, __ATOMIC_RELEASE);
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &held, &previous);
    started = pthread_create(&thread, NULL, forward_log, NULL) == 0;
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (!started) {
        __atomic_store_n(&log_forwarding, 0, __ATOMIC_RELEASE);
        close(log_pipe[0]);
        close(log_pipe[1]);
        log_pipe[0] = log_pipe[1] = -1;
        return;
    }
    pthread_detach(thread);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
}

/* What is still in the log pipe reaches the system log: stdout and stderr
 * go to /dev/null in one step each (no other thread can be handed those
 * descriptor numbers meanwhile), the pipe's last writer closes, and
 * forward_log, at the end of the pipe, ends; at most 200 ms. */
static void drain_log(void)
{
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC), waited;
    if (log_pipe[1] < 0 || null < 0) {
        if (null >= 0) close(null);
        return;
    }
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    close(null);
    close(log_pipe[1]);
    log_pipe[1] = -1;
    for (waited = 0; waited < 40 && __atomic_load_n(&log_forwarding, __ATOMIC_ACQUIRE); waited++) usleep(5000);
}

/* --- the process's end ------------------------------------------------ */

/* The game ends with exit() on its own thread (Quit, in libetc.c's VSync;
 * the quit SDL sends when the system destroys the activity; a problem the
 * game reports). exit() runs every handler registered in the process,
 * newest first, the system libraries' static destructors among them, while
 * the activity's own threads still run: HWUI's render workers (hwuiTask0/1)
 * then locked a mutex those destructors had just destroyed, and every Quit
 * aborted ("FORTIFY: pthread_mutex_lock called on a destroyed mutex"),
 * which the system records as a crash. On Android libgame.so is linked
 * with --wrap=exit (build_game32.py): the game's exit() comes here, runs
 * only this library's own handlers (the debug tools' files: profile.c,
 * recorder.c, ai_trace.c, image.c), and ends the process with _exit and the
 * game's status. Nothing of the player's is written at exit: memory cards,
 * save states, deck slots and settings are written (and renamed into
 * place) when they change. The headless runner (tools/pc/android/runner.c)
 * calls main and never Memories_AndroidMain: its exit() is the system's. */
static int app_process;  /* Memories_AndroidMain ran */
static int exit_status;

void __real_exit(int status) __attribute__((noreturn));
void __wrap_exit(int status) __attribute__((noreturn, visibility("hidden")));
void __cxa_finalize(void *dso);
extern void *__dso_handle; /* this library's (crtbegin_so) */
static void end_process(void) __attribute__((noreturn));

static void end_process(void)
{
    char line[96];
    fflush(NULL);
    drain_log();
    snprintf(line, sizeof(line), "memories-pc: the game has ended (status %d); ending the process", exit_status);
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
    _exit(exit_status);
}

void __wrap_exit(int status)
{
    if (!app_process) __real_exit(status);
    exit_status = status;
    /* This library's atexit handlers, newest first; end_process, the
     * first, ends the process. */
    __cxa_finalize(&__dso_handle);
    end_process();
}

/* An app gets no environment of its own: environment.txt in the player's
 * folder, NAME=value per line, stands in for the MEMORIES_* variables the
 * desktop builds read (tracing, scripted input, frame dumps). For testing;
 * adb can put the file there on an image with root, and run-as puts one in
 * the internal files folder (read after it) of a debuggable build on an
 * image without. */
static void read_environment(const char *folder)
{
    char path[1024], line[1024];
    FILE *file;
    snprintf(path, sizeof(path), "%s/environment.txt", folder);
    if (!(file = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), file)) {
        char *equals = strchr(line, '=');
        line[strcspn(line, "\r\n")] = '\0';
        if (!equals || equals == line || line[0] == '#') continue;
        *equals = '\0';
        setenv(line, equals + 1, 1);
        fprintf(stderr, "memories-pc: environment.txt: %s=%s\n", line, equals + 1);
    }
    fclose(file);
}

/* An APK asset (a path under assets/) copied to `to`; 1 when it was. */
static int copy_asset(const char *asset, const char *to)
{
    size_t size = 0;
    void *data = SDL_LoadFile(asset, &size); /* relative: the internal folder, then the assets */
    char temporary[1100];
    FILE *file;
    int written;
    if (!data) return 0;
    snprintf(temporary, sizeof(temporary), "%s.tmp", to);
    written = (file = fopen(temporary, "wb")) != NULL && fwrite(data, 1, size, file) == size;
    if (file && fclose(file)) written = 0;
    SDL_free(data);
    if (!written || rename(temporary, to)) {
        remove(temporary);
        return 0;
    }
    return 1;
}

/* A folder and everything in it (the unpacked mods of an earlier build). */
static void remove_tree(const char *path)
{
    DIR *folder = opendir(path);
    struct dirent *entry;
    char inner[1100];
    if (!folder) {
        remove(path);
        return;
    }
    while ((entry = readdir(folder)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(inner, sizeof(inner), "%s/%s", path, entry->d_name);
        remove_tree(inner);
    }
    closedir(folder);
    rmdir(path);
}

/* The files the desktop games have beside them (the shipped mods, the
 * language packs), listed in build/files.txt and packed under build/files/
 * (package_android.py): the folders of an earlier build go first, so a mod
 * the new build no longer ships does not linger. 1 when every one was. */
static int unpack_files(const char *directory)
{
    static const char *const folders[] = {"mods", "languages"};
    char path[1100], asset[1100], *list, *line, *next, *slash;
    size_t size = 0, i;
    int ok = 1, count = 0;
    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, folders[i]);
        remove_tree(path);
    }
    if (!(list = SDL_LoadFile("build/files.txt", &size))) return 1; /* an APK from before: nothing to unpack */
    for (line = list; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        line[strcspn(line, "\r")] = '\0';
        if (!*line || !Paths_Contained(line)) continue;
        snprintf(path, sizeof(path), "%s/%s", directory, line);
        if ((slash = strrchr(path, '/')) != NULL) {
            *slash = '\0';
            Paths_MakeDirs(path);
            *slash = '/';
        }
        snprintf(asset, sizeof(asset), "build/files/%s", line);
        if (copy_asset(asset, path)) count++;
        else ok = 0;
    }
    SDL_free(list);
    fprintf(stderr, "memories-pc: %d shipped files (mods, languages) unpacked\n", count);
    return ok;
}

/* The build's files out of the APK into <internal>/program, when this build
 * has not unpacked them yet (its buildid differs). Symbol tables of earlier
 * builds stay: a state saved by one of them is carried over by name
 * (state.c). The asset paths (build/...) are not the unpacked ones, since
 * SDL looks in the internal folder before the assets. */
static void unpack_program(void)
{
    const char *internal = SDL_GetAndroidInternalStoragePath();
    char directory[900], path[1100], asset[200], id[32] = "", have[32] = "";
    size_t size = 0;
    char *text;
    FILE *file;
    int ok;
    if (!internal || !*internal) return;
    snprintf(directory, sizeof(directory), "%s/program", internal);
    if ((text = SDL_LoadFile("build/buildid", &size)) != NULL) {
        snprintf(id, sizeof(id), "%.*s", (int)(size < sizeof(id) ? size : sizeof(id) - 1), text);
        id[strcspn(id, "\r\n")] = '\0';
        SDL_free(text);
    }
    snprintf(path, sizeof(path), "%s/buildid", directory);
    if ((file = fopen(path, "r")) != NULL) {
        if (fgets(have, sizeof(have), file)) have[strcspn(have, "\r\n")] = '\0';
        fclose(file);
    }
    if (!id[0]) {
        fprintf(stderr, "memories-pc: the APK has no build/buildid; save states cannot tell builds apart\n");
    } else if (strcmp(id, have)) {
        snprintf(path, sizeof(path), "%s/symbols", directory);
        Paths_MakeDirs(path);
        snprintf(asset, sizeof(asset), "build/symbols/%s.txt", id);
        snprintf(path, sizeof(path), "%s/symbols/%s.txt", directory, id);
        ok = copy_asset(asset, path);
        /* The variable table (control channel, symbols.c): not needed to play. */
        snprintf(asset, sizeof(asset), "build/symbols/%s.vars.txt", id);
        snprintf(path, sizeof(path), "%s/symbols/%s.vars.txt", directory, id);
        copy_asset(asset, path);
        ok = unpack_files(directory) && ok;
        snprintf(path, sizeof(path), "%s/commit", directory);
        copy_asset("build/commit", path);
        snprintf(path, sizeof(path), "%s/buildid", directory);
        ok = ok && copy_asset("build/buildid", path); /* last: it marks the rest as there */
        fprintf(stderr, "memories-pc: build %s %s %s\n", id, ok ? "unpacked into" : "could not be unpacked into",
                directory);
    }
    setenv("MEMORIES_PROGRAM_DIR", directory, 0);
}

#ifndef MEMORIES_TRANSLATED
/* The loader could not put the game where it was linked (android_loader.c):
 * the addresses in a state saved now would not hold in the next launch, nor
 * those of the states saved before in this one. States then go to a folder
 * of this launch alone, emptied at the start. Not for the translated game,
 * which is loaded anywhere and carries its states by its build ID
 * (translated_state_backend.c). */
static void check_load_bias(void)
{
    const char *bias = getenv("MEMORIES_ANDROID_LOAD_BIAS");
    const char *cache = SDL_GetAndroidCachePath();
    char directory[900], path[1100];
    DIR *folder;
    struct dirent *entry;
    if (!bias || !strcmp(bias, "0") || !cache) return;
    snprintf(directory, sizeof(directory), "%s/states-this-launch", cache);
    if ((folder = opendir(directory)) != NULL) {
        while ((entry = readdir(folder)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            remove(path);
        }
        closedir(folder);
    }
    Paths_MakeDirs(directory);
    setenv("MEMORIES_STATE_DIR", directory, 1);
    fprintf(stderr, "memories-pc: the game is loaded %s bytes from its link address; save states are kept for "
                    "this launch only (%s)\n", bias, directory);
}
#endif

/* Java's getExternalFilesDir(null), asked now, into `out`: 0, with the
 * reason in `why`, when it gives no folder. Asking is also what has the
 * system make the folder: under scoped storage (Android 11 on) the app
 * cannot make its own Android/data/<package> folder, the system does when
 * Java asks for it. SDL_GetAndroidExternalStoragePath keeps its first
 * answer, so it asks only once. */
static int external_files_dir(char *out, size_t size, char *why, size_t why_size)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity, file = NULL, text = NULL;
    jclass type;
    jmethodID method;
    int ok = 0;
    if (!env || !(activity = (jobject)SDL_GetAndroidActivity())) {
        snprintf(why, why_size, "Android gave no activity to ask for the folder");
        return 0;
    }
    if ((type = (*env)->GetObjectClass(env, activity)) != NULL &&
        (method = (*env)->GetMethodID(env, type, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;")) != NULL)
        file = (*env)->CallObjectMethod(env, activity, method, (jstring)NULL);
    if (type) (*env)->DeleteLocalRef(env, type);
    if (file && !(*env)->ExceptionCheck(env) && (type = (*env)->GetObjectClass(env, file)) != NULL) {
        if ((method = (*env)->GetMethodID(env, type, "getAbsolutePath", "()Ljava/lang/String;")) != NULL)
            text = (*env)->CallObjectMethod(env, file, method);
        (*env)->DeleteLocalRef(env, type);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        snprintf(why, why_size, "Android's getExternalFilesDir failed");
    } else if (!text) {
        snprintf(why, why_size, "Android gave no folder (the shared storage is not mounted, or Android could not make the folder)");
    } else {
        const char *chars = (*env)->GetStringUTFChars(env, (jstring)text, NULL);
        if (chars) {
            ok = *chars && snprintf(out, size, "%s", chars) < (int)size;
            (*env)->ReleaseStringUTFChars(env, (jstring)text, chars);
        }
        if (!ok) snprintf(why, why_size, "Android's folder name could not be read");
    }
    if (text) (*env)->DeleteLocalRef(env, text);
    if (file) (*env)->DeleteLocalRef(env, file);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

/* Why `files` could not be made (mkdir's `error`), with what is there of
 * it: the folder, its package folder and Android/data, each there (its
 * owner and mode) or stat's reason, beside the app's own user and the
 * Android version, so a player's report tells which of them is in the way. */
static void describe_folder(const char *files, int error, char *why, size_t why_size)
{
    char level[1024];
    int used, i;
    used = snprintf(why, why_size, "%s: %s; Android %d, the app is user %u", files,
                    error ? strerror(error) : "the system gave no reason", SDL_GetAndroidSDKVersion(),
                    (unsigned)getuid());
    snprintf(level, sizeof(level), "%s", files);
    for (i = 0; i < 3 && used >= 0 && (size_t)used < why_size; i++) {
        struct stat seen;
        char *name = strrchr(level, '/');
        const char *label = name ? name + 1 : NULL;
        int more;
        if (!name || name == level) break;
        if (i == 2) { /* "Android/data", as a player's file manager names it */
            const char *above = name;
            while (above > level && above[-1] != '/') above--;
            label = above;
        }
        if (stat(level, &seen))
            more = snprintf(why + used, why_size - (size_t)used, "; %s: %s", label, strerror(errno));
        else
            more = snprintf(why + used, why_size - (size_t)used, "; %s: there, owner %u, mode %03o", label,
                            (unsigned)seen.st_uid, (unsigned)(seen.st_mode & 0777));
        if (more < 0) break;
        used += more;
        *name = 0;
    }
}

/* The player's folder (the external files folder, top of this file), made
 * if it is not there yet; NULL when it cannot be, with the reason in `why`.
 * While the shared storage is not mounted (it can be unmounted, and may
 * not be mounted yet just after a boot) Java's getExternalFilesDir gives
 * no folder; it is asked again each time (external_files_dir), so a few
 * seconds of asking again, and the storage mounted meanwhile reaches this
 * process too. There is no other folder to fall back on: the disc image and
 * the saves are in this one, and a second folder would split them. */
static const char *user_folder(char *why, size_t why_size)
{
    static char files[1024];
    char said[1000] = "";
    int attempt;
    for (attempt = 0; attempt < 40; attempt++) { /* 10 s */
        if (attempt) SDL_Delay(250);
        if (!external_files_dir(files, sizeof(files), why, why_size)) {
            /* the reason is in `why` */
        } else if (!Paths_MakeDirs(files)) {
            if (attempt) fprintf(stderr, "memories-pc: the external files folder is there after %d ms\n", attempt * 250);
            return files;
        } else {
            /* Even "File exists" is waited for: just as the storage mounts,
             * Java may give the folder (made under /data/media) while mkdir
             * here says it exists, whether or not stat sees it yet. */
            describe_folder(files, errno, why, why_size);
        }
        if (strcmp(said, why)) { /* each new reason once */
            fprintf(stderr, "memories-pc: no external files folder yet after %d ms (%s); waiting for the storage\n",
                    attempt * 250, why);
            snprintf(said, sizeof(said), "%s", why);
        }
    }
    fprintf(stderr, "memories-pc: no external files folder after 10 s (%s)\n", why);
    return NULL;
}

/* Called by the loader (android_loader.c) in place of SDL_main. */
int Memories_AndroidMain(int argc, char **argv)
{
    static char name[] = "memories-pc";
    char *args[] = {name, NULL};
    char why[1000], message[2000];
    const char *files;
    (void)argc;
    (void)argv;
    app_process = 1;
    /* An exit() from outside the game (Java's System.exit; the loader's,
     * should this function return) runs the handlers registered so far,
     * newest first: this one, the first of the game's, ends the process
     * before the system libraries' destructors that were loaded before it.
     * Those of a library loaded later (one SDL opens during main) run
     * first. An atexit handler is not told the status: that exit ends with
     * 0. */
    atexit(end_process);
    log_to_logcat();
    if ((files = user_folder(why, sizeof(why))) != NULL) {
        setenv("MEMORIES_USER_DIR", files, 0);
        read_environment(files);
    }
    if (SDL_GetAndroidInternalStoragePath()) read_environment(SDL_GetAndroidInternalStoragePath());
    if (!files) {
        /* Only a folder a test names itself (MEMORIES_USER_DIR in the
         * internal environment.txt) stands in for it. */
        const char *named = getenv("MEMORIES_USER_DIR");
        if (!named || !*named || Paths_MakeDirs(named)) {
            if (named && *named)
                fprintf(stderr, "memories-pc: nor the MEMORIES_USER_DIR environment.txt names: %s: %s\n", named,
                        strerror(errno));
            snprintf(message, sizeof(message), "The game could not use its storage folder.\n\n"
                     "Close the game and open it again. If this comes back:\n"
                     "- restart the phone;\n"
                     "- if a file manager shows the folder Android/data/org.yfmredecomp.game, delete it and open "
                     "the game again (the saves in it go with it: copy them out first if you can);\n"
                     "- if you never played, Settings > Apps > the game > Storage > Clear storage;\n"
                     "- last, uninstall the game and install it again (this too removes the saves; a game of the "
                     "same name signed by someone else must go first).\n\n(%s)", why);
            Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", message);
            return 1;
        }
        fprintf(stderr, "memories-pc: environment.txt names the user folder: %s\n", named);
    }
    unpack_program();
    if (files) forget_old_pick(files);
#ifndef MEMORIES_TRANSLATED
    check_load_bias();
#endif
    /* The window is resizable, which SDL takes for "any orientation";
     * the game is a landscape picture. Which landscape follows what is
     * asked again once the window is made (Android_ApplyScreenRotation). */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* Back is the game's to answer (sdl.c: menus, notices, then the quit
     * question), not the system's, which would end the activity at once. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* The whole screen, without the status and navigation bars (SDL makes
     * a fullscreen window immersive). */
    setenv("MEMORIES_FULLSCREEN", "1", 0);
    setenv("MEMORIES_NO_MONITOR", "1", 0);
    setenv("MEMORIES_NO_UPDATE_CHECK", "1", 0);
    {
        /* Where the game is: its link address when the loader's range held
         * (load bias 0), so crash addresses are the build's symbols' own. */
        Dl_info info;
#ifdef MEMORIES_TRANSLATED
        /* Linked at 0 and loaded where the system puts it: symbolize a
         * crash address as its offset from this. */
        fprintf(stderr, "memories-pc: Android, user folder %s, libgame.so at %p (translated)\n",
                getenv("MEMORIES_USER_DIR") ? getenv("MEMORIES_USER_DIR") : "?",
                dladdr((void *)Memories_AndroidMain, &info) ? info.dli_fbase : NULL);
#else
        fprintf(stderr, "memories-pc: Android, user folder %s, libgame.so at %p (load bias %s)\n",
                getenv("MEMORIES_USER_DIR") ? getenv("MEMORIES_USER_DIR") : "?",
                dladdr((void *)Memories_AndroidMain, &info) ? info.dli_fbase : NULL,
                getenv("MEMORIES_ANDROID_LOAD_BIAS") ? getenv("MEMORIES_ANDROID_LOAD_BIAS") : "?");
#endif
    }
    /* main's status through __wrap_exit, as the game's own exit() */
    exit(main(1, args));
}
#endif
