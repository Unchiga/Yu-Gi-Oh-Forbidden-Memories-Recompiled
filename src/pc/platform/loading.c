/* The loading screen's pacing (loading.h). The drawing is the platform's
 * (sdl.c, Platform_ShowLoading); this file only decides when to ask for it,
 * so a start that takes a fraction of a second never does. */
#include "pc/platform/loading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

void LoadingPace_Begin(LoadingPace *pace, uint64_t now_us)
{
    memset(pace, 0, sizeof(*pace));
    pace->started_us = now_us;
    pace->started = 1;
}

int LoadingPace_Due(LoadingPace *pace, uint64_t now_us, const char *what)
{
    if (!pace->started || now_us - pace->started_us < LOADING_DELAY_US) return 0;
    if (pace->drawn && !strncmp(pace->what, what, sizeof(pace->what) - 1) &&
        now_us - pace->drawn_us < LOADING_INTERVAL_US)
        return 0;
    snprintf(pace->what, sizeof(pace->what), "%s", what);
    pace->drawn_us = now_us;
    pace->drawn = 1;
    return 1;
}

static LoadingSink sink;
static LoadingPace pace;
static int active;

static uint64_t clock_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)count.QuadPart / (uint64_t)frequency.QuadPart * 1000000u +
           (uint64_t)count.QuadPart % (uint64_t)frequency.QuadPart * 1000000u / (uint64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
#endif
}

static uint64_t now_us(void) { return sink.now ? sink.now() : clock_us(); }

void Loading_Begin(const LoadingSink *to)
{
    if (active || !to || !to->show) return;
    sink = *to;
    LoadingPace_Begin(&pace, now_us());
    active = 1;
}

void Loading_Step(const char *what, unsigned done, unsigned total)
{
    char line[160];
    void (*quit)(void) = sink.quit;
    int shown;
    if (!active || !LoadingPace_Due(&pace, now_us(), what)) return;
    if (total) snprintf(line, sizeof(line), "%s %u of %u", what, done, total);
    else snprintf(line, sizeof(line), "%s", what);
    shown = sink.show(line, done, total);
    if (shown < 0) {
        Loading_Finish();   /* nothing to show it on: not asked again */
    } else if (shown > 0) {
        /* Closed while it loaded: there is nothing to lose, so no question. */
        Loading_Finish();
        if (quit) quit();
        else exit(0);
    }
}

void Loading_Finish(void)
{
    void (*end)(void) = sink.end;
    if (!active) return;
    active = 0;
    memset(&sink, 0, sizeof(sink));
    if (end) end();
}
