/* A signal handler on the thread that is changing the token registry cannot wait
 * for that change: with MEMORIES_CLOCK=interrupt or MEMORIES_PROFILE the clock's
 * tick runs game code there. It must still resolve and encode, one past a block
 * too. The handler includes the file's own `changing` to prove it hit a change. */
#include "types.h"
#include "pc/guest/translated_runtime.c"
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define BLOCKS 200000
static unsigned char *blocks[BLOCKS];
static u32 tokens[BLOCKS];
static volatile long in_change, total, handled;
static volatile int stop;
static pthread_t main_thread;
static unsigned cursor;

void *GuestRuntime_malloc(size_t length);
void GuestRuntime_free(void *pointer);

static void fail(const char *why, size_t length)
{
    if (write(2, why, length) < 0) _exit(4);
    _exit(3);
}
#define FAIL(text) fail(text "\n", sizeof(text))

static void on_signal(int number)
{
    unsigned i = cursor++ % BLOCKS;
    (void)number;
    total++;
    if (changing) in_change++;
    if (GuestRuntime_ResolveData((void *)(uintptr_t)(tokens[i] + (i & 15u)), 4) != blocks[i] + (i & 15u))
        FAIL("a token resolved to the wrong block");
    if (GuestRuntime_EncodePointer(blocks[i] + 4) != tokens[i] + 4) FAIL("a block encoded to the wrong token");
    if (GuestRuntime_EncodePointer(blocks[i] + 48) != tokens[i] + 48) FAIL("one past a block encoded wrong");
    handled++;
}

/* One signal at a time, and a gap after it: one that is already waiting when the
 * handler returns runs before the thread's next instruction, so signals sent on a timer
 * can stop the thread for good in the middle of a change, on a slow machine. */
static void *kicker(void *unused)
{
    struct timespec gap = {0, 20000};
    (void)unused;
    while (!stop) {
        long seen = handled;
        pthread_kill(main_thread, SIGUSR1);
        while (!stop && handled == seen) sched_yield();
        nanosleep(&gap, NULL);
    }
    return NULL;
}

int main(void)
{
    MemoriesMemory *memory = calloc(1, sizeof(*memory));
    pthread_t thread;
    struct sigaction action;
    time_t end;
    long rounds = 0;
    if (!memory || GuestRuntime_Bind(memory)) return 1;
    for (int i = 0; i < BLOCKS; i++) {
        blocks[i] = GuestRuntime_malloc(48);
        tokens[i] = GuestRuntime_EncodePointer(blocks[i]);
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, NULL);
    main_thread = pthread_self();
    pthread_create(&thread, NULL, kicker, NULL);
    end = time(NULL) + 5;
    while (time(NULL) < end) {
        void *block = GuestRuntime_malloc(32);
        GuestRuntime_free((void *)(uintptr_t)GuestRuntime_EncodePointer(block));
        rounds++;
    }
    stop = 1;
    pthread_join(thread, NULL);
    printf("Token registry: %ld changes, %ld signals, %ld inside a change\n", rounds, total, in_change);
    return in_change ? 0 : 2;
}
