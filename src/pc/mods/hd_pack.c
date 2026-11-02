/* The HD pack's download (hd_pack.h). The job's thread owns `job` from a
 * step's start until it publishes the step's end in job.state (atomic, the
 * last thing it writes); the game thread reads the rest only after that,
 * except job.done (atomic) and job.cancel (atomic, the game thread's). */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "hd_pack.h"
#include "json.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#define API_MAX (2L << 20)                  /* the /releases/latest answer */

static struct {
    const HdNet *net;
    int state;     /* atomic */
    int cancel;    /* atomic: set by the game thread */
    unsigned done; /* atomic: the step's bytes so far */
    int waiting;   /* atomic: the download waits for the network */
    unsigned long total;
    int cancelled;
    int threaded;  /* a thread was started and not joined */
    pthread_t thread;
    HdRelease release;
    ModsImport *import;
    char folder[1024], mods[1024], zip[1200], part[1210];
    char why[400];
} job;

static int load(const int *at) { return __atomic_load_n(at, __ATOMIC_ACQUIRE); }
static void publish(int state) { __atomic_store_n(&job.state, state, __ATOMIC_RELEASE); }

static void reap(void)
{
    int state = load(&job.state);
    if (job.threaded && state != HD_LOOKING && state != HD_DOWNLOADING && state != HD_INSTALLING &&
        state != HD_CLEANING) {
        pthread_join(job.thread, NULL);
        job.threaded = 0;
    }
}

void HdPack_SetNet(const HdNet *net)
{
    if (!HdPack_Busy())
        job.net = net;
}
int HdPack_Available(void) { return job.net != NULL; }
int HdPack_State(void)
{
    reap();
    return load(&job.state);
}
int HdPack_Busy(void)
{
    int state = load(&job.state);
    return state == HD_LOOKING || state == HD_DOWNLOADING || state == HD_INSTALLING || state == HD_CLEANING;
}
unsigned long HdPack_Done(void) { return __atomic_load_n(&job.done, __ATOMIC_ACQUIRE); }
unsigned long HdPack_Total(void) { return job.total; }
int HdPack_Waiting(void) { return load(&job.waiting); }
const HdRelease *HdPack_Release(void) { return &job.release; }
ModsImport *HdPack_Import(void) { return HdPack_Busy() ? NULL : job.import; }
const char *HdPack_Why(void) { return job.why; }
int HdPack_Cancelled(void) { return job.cancelled; }
void HdPack_Cancel(void) { __atomic_store_n(&job.cancel, 1, __ATOMIC_RELEASE); }

/* The step failed: the reason, and HD_FAILED (published last). */
static void fail(const char *format, ...)
{
    va_list list;
    if (load(&job.cancel)) {
        job.cancelled = 1;
        snprintf(job.why, sizeof(job.why), "Cancelled.");
    } else {
        va_start(list, format);
        vsnprintf(job.why, sizeof(job.why), format, list);
        va_end(list);
    }
    publish(HD_FAILED);
}

/* Plain words for a failed connection or an HTTP status that is not 200. */
static void say_net(char *out, size_t size, int error, int status)
{
    if (error == HD_NET_OFFLINE)
        snprintf(out, size, "No internet connection. Connect to Wi-Fi or mobile data and try again.");
    else if (error == HD_NET_TIMEOUT)
        snprintf(out, size, "GitHub did not answer in time. Check the connection and try again.");
    else if (error)
        snprintf(out, size, "The connection to GitHub failed. Try again later.");
    else if (status == 403 || status == 429)
        snprintf(out, size, "GitHub is limiting requests from this network right now. Try again later.");
    else
        snprintf(out, size, "GitHub answered with an error (HTTP %d). Try again later.", status);
}

/* --- the release ------------------------------------------------------ */

static int hex_digest(const char *text, char *out)
{
    if (!text || strncmp(text, "sha256:", 7))
        return 0;
    text += 7;
    for (int i = 0; i < 64; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'F')
            c = (char)(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
        out[i] = c;
    }
    out[64] = 0;
    return text[64] == 0;
}

int HdPack_PickAsset(const char *json, HdRelease *out, char *why, size_t why_size)
{
    char error[160], exact[200];
    JsonDocument *document = Json_Parse(json, error, sizeof(error));
    const JsonValue *root = document ? Json_Root(document) : NULL, *assets, *asset;
    const char *tag;
    int found = -1;
    memset(out, 0, sizeof(*out));
    snprintf(why, why_size, "GitHub's answer could not be read.");
    if (!root || Json_TypeOf(root) != JSON_OBJECT || !(tag = Json_String(Json_Member(root, "tag_name"), NULL)) ||
        Json_TypeOf(assets = Json_Member(root, "assets")) != JSON_ARRAY)
        goto done;
    found = 0;
    snprintf(out->tag, sizeof(out->tag), "%s", tag);
    snprintf(why, why_size, "The latest release (%s) has no HD pack.", out->tag);
    snprintf(exact, sizeof(exact), "%s%s.zip", HD_PACK_PREFIX, out->tag);
    for (asset = Json_At(assets, 0); asset; asset = Json_Next(asset)) /* the tag's own, if it is there */
        if (!strcmp(Json_String(Json_Member(asset, "name"), ""), exact))
            break;
    for (asset = asset ? asset : Json_At(assets, 0); asset; asset = Json_Next(asset)) {
        const char *name = Json_String(Json_Member(asset, "name"), ""),
                   *url = Json_String(Json_Member(asset, "browser_download_url"), "");
        long size = Json_Number(Json_Member(asset, "size"), -1);
        size_t n = strlen(name);
        if (strncmp(name, HD_PACK_PREFIX, strlen(HD_PACK_PREFIX)) || n < 4 || strcmp(name + n - 4, ".zip"))
            continue;
        if (strchr(name, '/') || strchr(name, '\\') || n >= sizeof(out->asset) || strncmp(url, "https://", 8) ||
            strlen(url) >= sizeof(out->url) || size <= 0) {
            snprintf(why, why_size, "GitHub's answer could not be read.");
            break;
        }
        if ((unsigned long)size > MODS_IMPORT_BYTES) { /* the importer's cap: pictures barely compress */
            snprintf(why, why_size, "The HD pack of %s is too large: more than %u MB once unpacked.", out->tag,
                     (MODS_IMPORT_BYTES >> 20) + 1);
            break;
        }
        if (!hex_digest(Json_String(Json_Member(asset, "digest"), NULL), out->sha256)) {
            snprintf(why, why_size, "GitHub gives no checksum for the HD pack of %s, so a download could not be "
                     "checked. Try again later.", out->tag);
            break;
        }
        snprintf(out->asset, sizeof(out->asset), "%s", name);
        snprintf(out->url, sizeof(out->url), "%s", url);
        out->size = (unsigned long)size;
        found = 1;
        why[0] = 0;
        break;
    }
done:
    Json_Free(document);
    return found;
}

static void *look(void *unused)
{
    char *body = NULL, why[400];
    long used = 0, got = 0;
    void *stream = NULL;
    int status = 0, error, picked;
    (void)unused;
    error = job.net->open(HD_PACK_LATEST, &stream, &status);
    if (error || status != 200) {
        if (!error)
            job.net->close(stream);
        say_net(why, sizeof(why), error, status);
        fail("%s", why);
        return NULL;
    }
    if (!(body = malloc(API_MAX + 1))) {
        job.net->close(stream);
        fail("Out of memory.");
        return NULL;
    }
    while (used < API_MAX && !load(&job.cancel) &&
           (got = job.net->read(stream, (unsigned char *)body + used, (size_t)(API_MAX - used))) > 0)
        used += got;
    job.net->close(stream);
    body[used] = 0;
    if (got < 0 || load(&job.cancel)) {
        say_net(why, sizeof(why), (int)got, 0);
        free(body);
        fail("%s", why);
        return NULL;
    }
    picked = HdPack_PickAsset(body, &job.release, why, sizeof(why));
    free(body);
    if (picked != 1) {
        fail("%s", why);
        return NULL;
    }
    fprintf(stderr, "memories-pc: HD pack: %s, %s (%lu bytes, sha256 %s)\n", job.release.tag, job.release.asset,
            job.release.size, job.release.sha256);
    publish(HD_FOUND);
    return NULL;
}

/* --- the download ----------------------------------------------------- */

/* Removes the HD_PACK_PREFIX* files in `folder`. */
static void remove_stale(const char *folder)
{
    DIR *directory;
    struct dirent *entry;
    char path[1400];
    if (!(directory = opendir(folder)))
        return;
    while ((entry = readdir(directory)))
        if (!strncmp(entry->d_name, HD_PACK_PREFIX, strlen(HD_PACK_PREFIX)) &&
            snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) < (int)sizeof(path))
            remove(path);
    closedir(directory);
}

/* Plain words for a download the system gave up on. */
static void say_fetch(char *out, size_t size, int reason)
{
    if (reason == 1006)
        snprintf(out, size, "Not enough free space for the HD pack. Free some room and try again.");
    else if (reason >= 400 && reason < 600)
        say_net(out, size, 0, reason);
    else if (reason == 1001 || reason == 1007 || reason == 1009)
        snprintf(out, size, "Could not save the download (the download manager's error %d).", reason);
    else
        snprintf(out, size, "The download from GitHub failed. Try again.");
}

static void *download(void *unused)
{
    struct timespec nap = {0, 250000000};
    struct stat info;
    char sha[65] = "", why[400], saved[1300];
    const char *expected = job.net->test_sha256 && *job.net->test_sha256 ? job.net->test_sha256 : job.release.sha256;
    long long id = -1;
    unsigned long done = 0;
    int state = 0, reason = 0, error;
    (void)unused;
    /* what a job the app ended in left: the system's downloads, then files */
    job.net->forget();
    remove_stale(job.folder);
    if ((error = job.net->fetch(job.release.url, job.part, &id))) {
        if (error == HD_NET_NO_MANAGER)
            snprintf(why, sizeof(why), "This phone's download manager is turned off (Settings > Apps > Download "
                     "Manager). Turn it on, or put the HD pack's .zip in with Import mod...");
        else
            snprintf(why, sizeof(why), "Could not start the download.");
        fail("%s", why);
        return NULL;
    }
    for (;;) {
        if (load(&job.cancel)) {
            job.net->stop(id);
            remove(job.part);
            fail("Cancelled.");
            return NULL;
        }
        state = job.net->poll(id, &done, &reason);
        __atomic_store_n(&job.done, (unsigned)done, __ATOMIC_RELEASE);
        __atomic_store_n(&job.waiting, state != HD_FETCH_WAITING ? 0 : reason == 3 ? 2 : 1, __ATOMIC_RELEASE);
        if (state == HD_FETCH_DONE || state == HD_FETCH_FAILED || state == HD_FETCH_GONE)
            break;
        nanosleep(&nap, NULL);
    }
    __atomic_store_n(&job.waiting, 0, __ATOMIC_RELEASE);
    if (state != HD_FETCH_DONE) {
        if (state == HD_FETCH_GONE)
            snprintf(why, sizeof(why), "The download was cancelled outside the game. Nothing was installed.");
        else
            say_fetch(why, sizeof(why), reason);
        fprintf(stderr, "memories-pc: HD pack: the download ended (%d, reason %d)\n", state, reason);
        job.net->stop(id);
        remove(job.part);
        fail("%s", why);
        return NULL;
    }
    if (load(&job.cancel)) { /* Stop as the last piece came */
        job.net->stop(id);
        remove(job.part);
        fail("Cancelled.");
        return NULL;
    }
    /* Its own name before the system forgets it (which removes the file
     * still at its path): from here on a plain file of the job's. The
     * system names the file itself (<name>-1 when one is in the way), so
     * the path is the one it says, which must be in the folder. */
    if (!job.net->where(id, saved, sizeof(saved)) || strncmp(saved, job.folder, strlen(job.folder)) ||
        saved[strlen(job.folder)] != '/' || strstr(saved, "/..")) {
        fprintf(stderr, "memories-pc: HD pack: the download manager saved it at \"%s\"\n", saved);
        snprintf(saved, sizeof(saved), "%s", job.part);
    }
    error = rename(saved, job.zip);
    job.net->stop(id);
    if (error) {
        snprintf(why, sizeof(why), "Could not save the download: %s.", strerror(errno));
        goto failed;
    }
    if (stat(job.zip, &info) || (unsigned long)info.st_size != job.release.size || !job.net->sha256(job.zip, sha) ||
        strcmp(sha, expected)) {
        fprintf(stderr, "memories-pc: HD pack: %ld of %lu bytes, sha256 %s, expected %s\n",
                stat(job.zip, &info) ? -1L : (long)info.st_size, job.release.size, sha, expected);
        snprintf(why, sizeof(why), "The download was damaged. Try again.");
        goto failed;
    }
    if (!(job.import = Mods_ImportOpen(job.zip, why, sizeof(why))))
        goto failed;
    if (Mods_ImportCount(job.import) != 1 || strcmp(Mods_ImportMod(job.import, 0)->id, HD_PACK_ID)) {
        snprintf(why, sizeof(why), "That download is not the HD pack (no %s mod in it).", HD_PACK_ID);
        goto failed;
    }
    if (load(&job.cancel)) /* Stop while it was checked: nothing goes in */
        goto failed;
    fprintf(stderr, "memories-pc: HD pack: downloaded and checked (%lu bytes)\n", job.release.size);
    publish(HD_DOWNLOADED);
    return NULL;
failed:
    Mods_ImportClose(job.import);
    job.import = NULL;
    remove(job.part);
    remove(job.zip);
    fail("%s", why);
    return NULL;
}

/* --- the unpacking ---------------------------------------------------- */

static void *install(void *unused)
{
    char why[400], folder[1200];
    const ModsImportMod *mod = Mods_ImportMod(job.import, 0);
    (void)unused;
    Mods_ImportWatch(job.import, &job.done, &job.cancel);
    if (!Mods_ImportInstall(job.import, job.mods, why, sizeof(why))) {
        Mods_ImportWatch(job.import, NULL, NULL);
        fail("%s", why);
        return NULL;
    }
    Mods_ImportWatch(job.import, NULL, NULL);
    snprintf(folder, sizeof(folder), "%s/%s", job.mods, mod->folder);
    if (!HdPack_WriteTag(folder, job.release.tag))
        fprintf(stderr, "memories-pc: HD pack: could not write %s/%s: %s\n", folder, HD_PACK_MARKER, strerror(errno));
    fprintf(stderr, "memories-pc: HD pack: %s installed in %s\n", job.release.tag, folder);
    publish(HD_INSTALLED);
    return NULL;
}

/* --- the steps -------------------------------------------------------- */

static int start(int from, int state, void *(*step)(void *), unsigned long total)
{
    if (HdPack_State() != from || !job.net)
        return 0;
    __atomic_store_n(&job.cancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&job.done, 0, __ATOMIC_RELEASE);
    job.total = total;
    job.cancelled = 0;
    job.why[0] = 0;
    publish(state);
    if (pthread_create(&job.thread, NULL, step, NULL)) {
        snprintf(job.why, sizeof(job.why), state == HD_INSTALLING ? "Could not start unpacking the HD pack."
                                           : state == HD_LOOKING ? "Could not ask GitHub for the HD pack."
                                                                 : "Could not start the download.");
        publish(HD_FAILED);
        return 0;
    }
    job.threaded = 1;
    return 1;
}

int HdPack_Lookup(void)
{
    if (HdPack_State() != HD_IDLE)
        return 0;
    memset(&job.release, 0, sizeof(job.release));
    return start(HD_IDLE, HD_LOOKING, look, 0);
}

int HdPack_Download(const char *folder)
{
    if (HdPack_State() != HD_FOUND)
        return 0;
    if (snprintf(job.folder, sizeof(job.folder), "%s", folder) >= (int)sizeof(job.folder) ||
        snprintf(job.zip, sizeof(job.zip), "%s/%s", folder, job.release.asset) >= (int)sizeof(job.zip)) {
        snprintf(job.why, sizeof(job.why), "The app's folder has too long a name.");
        job.zip[0] = 0; /* cut short: nothing of the job's to remove */
        publish(HD_FAILED);
        return 0;
    }
    snprintf(job.part, sizeof(job.part), "%s.part", job.zip);
    return start(HD_FOUND, HD_DOWNLOADING, download, job.release.size);
}

int HdPack_Install(const char *mods)
{
    if (HdPack_State() != HD_DOWNLOADED)
        return 0;
    if (load(&job.cancel)) { /* a Stop after the download's last check: nothing goes in */
        fail("Cancelled.");
        return 0;
    }
    snprintf(job.mods, sizeof(job.mods), "%s", mods);
    return start(HD_DOWNLOADED, HD_INSTALLING, install, (unsigned long)Mods_ImportBytes(job.import));
}

void HdPack_Reset(void)
{
    if (HdPack_Busy())
        return;
    reap();
    Mods_ImportClose(job.import);
    job.import = NULL;
    if (job.zip[0]) {
        remove(job.part);
        remove(job.zip);
    }
    job.zip[0] = job.part[0] = 0;
    job.why[0] = 0;
    job.cancelled = 0;
    publish(HD_IDLE);
}

/* What a job the app ended in left: the system's downloads (a killed app's
 * download goes on without it) and then the files. The download manager is
 * Java: on a thread of its own, never the game's. */
static void *clean(void *unused)
{
    (void)unused;
    if (job.net->forget)
        job.net->forget();
    remove_stale(job.folder);
    publish(HD_IDLE);
    return NULL;
}
void HdPack_Cleanup(const char *folder)
{
    if (HdPack_State() != HD_IDLE || !job.net ||
        snprintf(job.folder, sizeof(job.folder), "%s", folder) >= (int)sizeof(job.folder))
        return;
    if (!start(HD_IDLE, HD_CLEANING, clean, 0) && load(&job.state) == HD_FAILED)
        HdPack_Reset(); /* no thread: nothing was cleared, nothing is wrong */
}

/* --- space and the installed tag -------------------------------------- */

unsigned long long HdPack_SpaceNeeded(const HdRelease *release)
{
    return (unsigned long long)release->size * 9 / 4 + (32ULL << 20);
}

long long HdPack_FreeBytes(const char *path)
{
#ifndef _WIN32
    struct statvfs space;
    if (!statvfs(path, &space))
        return (long long)space.f_bavail * (long long)space.f_frsize;
#else
    (void)path;
#endif
    return -1;
}

int HdPack_InstalledTag(const char *directory, char *tag, size_t size)
{
    char path[1200];
    FILE *file;
    int ok = 0;
    tag[0] = 0;
    snprintf(path, sizeof(path), "%s/%s", directory, HD_PACK_MARKER);
    if ((file = fopen(path, "r"))) {
        ok = fgets(tag, (int)size, file) != NULL;
        fclose(file);
        tag[strcspn(tag, "\r\n")] = 0;
        ok = ok && tag[0];
    }
    return ok;
}

int HdPack_WriteTag(const char *directory, const char *tag)
{
    char path[1200];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", directory, HD_PACK_MARKER);
    if (!(file = fopen(path, "w")))
        return 0;
    fprintf(file, "%s\n", tag);
    return !fclose(file);
}
