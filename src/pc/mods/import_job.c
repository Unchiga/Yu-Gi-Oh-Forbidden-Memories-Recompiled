/* Import mod... on a thread of its own (import_job.h). The job's thread owns
 * `job` from a step's start until it publishes the step's end in job.state
 * (atomic, the last thing it writes); the game thread reads the rest only
 * after that, except job.done and job.total (atomic) and job.cancel (atomic,
 * the game thread's). */
#define _POSIX_C_SOURCE 200809L
#include "import_job.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct {
    int state;      /* atomic */
    int cancel;     /* atomic: set by the game thread */
    unsigned done;  /* atomic: the step's bytes so far */
    unsigned total; /* atomic: all of them, when known */
    int cancelled;
    int threaded;   /* a thread was started and not joined */
    pthread_t thread;
    ImportJobFetch fetch;
    ModsImport *import;
    char *zip;
    size_t zip_size;
    char mods[1024];
    char why[400];
    void (*hook)(void);
} job;

static int load(const int *at) { return __atomic_load_n(at, __ATOMIC_ACQUIRE); }
static void publish(int state) { __atomic_store_n(&job.state, state, __ATOMIC_RELEASE); }

static void reap(void)
{
    int state = load(&job.state);
    if (job.threaded && state != IMPORT_JOB_READING && state != IMPORT_JOB_UNPACKING) {
        pthread_join(job.thread, NULL);
        job.threaded = 0;
    }
}

int ImportJob_State(void)
{
    reap();
    return load(&job.state);
}
int ImportJob_Busy(void)
{
    int state = load(&job.state);
    return state == IMPORT_JOB_READING || state == IMPORT_JOB_UNPACKING;
}
unsigned long ImportJob_Done(void) { return __atomic_load_n(&job.done, __ATOMIC_ACQUIRE); }
unsigned long ImportJob_Total(void) { return __atomic_load_n(&job.total, __ATOMIC_ACQUIRE); }
void ImportJob_Cancel(void) { __atomic_store_n(&job.cancel, 1, __ATOMIC_RELEASE); }
int ImportJob_StopAsked(void) { return load(&job.cancel); }
int ImportJob_Cancelled(void) { return job.cancelled; }
const char *ImportJob_Why(void) { return job.why; }
void ImportJob_SetStepHook(void (*hook)(void)) { job.hook = hook; }

/* The step failed: the reason, and IMPORT_JOB_FAILED (published last). The
 * steps say "Cancelled." when the Stop is why they ended; any other reason
 * is kept even if a Stop came too late (an old mod that could not be put
 * back is named in it). */
static void fail(const char *format, ...)
{
    va_list list;
    va_start(list, format);
    vsnprintf(job.why, sizeof(job.why), format, list);
    va_end(list);
    job.cancelled = !strcmp(job.why, "Cancelled.");
    publish(IMPORT_JOB_FAILED);
}

/* --- the copy and the reading ----------------------------------------- */

static void *read_zip(void *unused)
{
    char why[400];
    (void)unused;
    if (!job.fetch(job.zip, job.zip_size, why, sizeof(why), &job.done, &job.total, &job.cancel)) {
        fail("%s", why); /* the copy removed its own file */
        return NULL;
    }
    if (!(job.import = Mods_ImportOpen(job.zip, why, sizeof(why)))) {
        Mods_ImportRemoveTree(job.zip);
        fail("%s", why);
        return NULL;
    }
    if (!Mods_ImportCount(job.import) || load(&job.cancel)) {
        int none = !Mods_ImportCount(job.import);
        Mods_ImportClose(job.import);
        job.import = NULL;
        Mods_ImportRemoveTree(job.zip);
        fail("%s", none ? "This .zip has no mod in it." : "Cancelled.");
        return NULL;
    }
    publish(IMPORT_JOB_READ);
    return NULL;
}

/* --- the unpacking ---------------------------------------------------- */

static void stepped(void)
{
    if (job.hook)
        job.hook();
}

static void *unpack(void *unused)
{
    char why[400];
    int ok;
    (void)unused;
    Mods_ImportWatch(job.import, &job.done, &job.cancel);
    Mods_ImportStepped(job.import, job.hook ? stepped : NULL);
    ok = Mods_ImportInstall(job.import, job.mods, why, sizeof(why));
    Mods_ImportWatch(job.import, NULL, NULL);
    Mods_ImportStepped(job.import, NULL);
    if (!ok) {
        fail("%s", why);
        return NULL;
    }
    publish(IMPORT_JOB_UNPACKED);
    return NULL;
}

/* --- the steps -------------------------------------------------------- */

static int start(int state, void *(*step)(void *), unsigned long total)
{
    if (state == IMPORT_JOB_READING) /* a Stop before the second step is the second step's to see */
        __atomic_store_n(&job.cancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&job.done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&job.total, (unsigned)total, __ATOMIC_RELEASE);
    job.cancelled = 0;
    job.why[0] = 0;
    publish(state);
    if (pthread_create(&job.thread, NULL, step, NULL)) {
        snprintf(job.why, sizeof(job.why), "Could not start %s.",
                 state == IMPORT_JOB_UNPACKING ? "unpacking the .zip" : "importing the .zip");
        publish(IMPORT_JOB_FAILED);
        return 0;
    }
    job.threaded = 1;
    return 1;
}

int ImportJob_Read(ImportJobFetch fetch, char *zip, size_t zip_size)
{
    if (ImportJob_State() != IMPORT_JOB_IDLE || !fetch)
        return 0;
    job.fetch = fetch;
    job.zip = zip;
    job.zip_size = zip_size;
    job.import = NULL;
    return start(IMPORT_JOB_READING, read_zip, 0);
}

ModsImport *ImportJob_Take(void)
{
    ModsImport *import;
    if (ImportJob_State() != IMPORT_JOB_READ)
        return NULL;
    import = job.import;
    job.import = NULL;
    publish(IMPORT_JOB_IDLE);
    return import;
}

int ImportJob_Install(ModsImport *import, const char *mods)
{
    if (ImportJob_State() != IMPORT_JOB_IDLE || !import)
        return 0;
    if (load(&job.cancel)) { /* a Stop after the reading: nothing is unpacked */
        job.cancelled = 1;
        snprintf(job.why, sizeof(job.why), "Cancelled.");
        publish(IMPORT_JOB_FAILED);
        return 0;
    }
    if (snprintf(job.mods, sizeof(job.mods), "%s", mods) >= (int)sizeof(job.mods)) {
        snprintf(job.why, sizeof(job.why), "The app's folder has too long a name.");
        publish(IMPORT_JOB_FAILED);
        return 0;
    }
    job.import = import;
    return start(IMPORT_JOB_UNPACKING, unpack, (unsigned long)Mods_ImportBytes(import));
}

void ImportJob_Reset(void)
{
    if (ImportJob_Busy())
        return;
    reap();
    job.import = NULL; /* the caller's */
    job.why[0] = 0;
    job.cancelled = 0;
    __atomic_store_n(&job.cancel, 0, __ATOMIC_RELEASE);
    publish(IMPORT_JOB_IDLE);
}
