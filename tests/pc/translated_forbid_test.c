/* The thread kept off the guest heap in builds with the test hooks
 * (src/pc/guest/translated_alloc.c): reaching it there stops the game, and when that
 * thread has ended a later one given the same pthread_t is free to. */
#include "pc/guest/translated_runtime.h"
/* Tests must execute their checks in Release CI too. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

void *GuestRuntime_malloc(size_t length);
void GuestRuntime_free(void *pointer);
void GuestRuntime_ForbidHeapOnThisThread(int forbid);

static pthread_t seen;

static void *worker(void *unused)
{
    (void)unused;
    seen = pthread_self();
    GuestRuntime_ForbidHeapOnThisThread(1);
    GuestRuntime_ForbidHeapOnThisThread(0);
    return NULL;
}

static void *later(void *unused)
{
    (void)unused;
    GuestRuntime_free(GuestRuntime_malloc(16));
    return pthread_equal(seen, pthread_self()) ? (void *)1 : NULL;
}

static void *forbidden(void *unused)
{
    (void)unused;
    GuestRuntime_ForbidHeapOnThisThread(1);
    GuestRuntime_malloc(16);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    void *same = NULL;
    pid_t child;
    int status = 0, tries;
    /* A thread that goes and one in its place, until one is really in its place. */
    for (tries = 0; tries < 50 && !same; tries++) {
        assert(!pthread_create(&thread, NULL, worker, NULL) && !pthread_join(thread, NULL));
        assert(!pthread_create(&thread, NULL, later, NULL) && !pthread_join(thread, &same));
    }
    assert(same); /* it was the same pthread_t, and nothing stopped */
    /* The thread itself does stop. */
    child = fork();
    assert(child >= 0);
    if (!child) {
        pthread_create(&thread, NULL, forbidden, NULL);
        pthread_join(thread, NULL);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    return 0;
}
