/* The loading screen's pacing (loading.h). The drawing is the platform's
 * (sdl.c, Platform_ShowLoading); this file only decides when to ask for it,
 * so a start that takes a fraction of a second never does. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
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
    pace->delay_us = LOADING_DELAY_US;
    pace->started = 1;
}

int LoadingPace_Due(LoadingPace *pace, uint64_t now_us, const char *what)
{
    if (!pace->started || now_us - pace->started_us < pace->delay_us) return 0;
    if (pace->drawn && now_us - pace->drawn_us < LOADING_FLOOR_US) return 0;
    if (pace->drawn && !strncmp(pace->what, what, sizeof(pace->what) - 1) &&
        now_us - pace->drawn_us < LOADING_INTERVAL_US)
        return 0;
    snprintf(pace->what, sizeof(pace->what), "%s", what);
    pace->drawn_us = now_us;
    pace->drawn = 1;
    return 1;
}

int LoadingPace_Now(LoadingPace *pace, uint64_t now_us, const char *what)
{
    if (!pace->started) return 0;
    if (pace->drawn && now_us - pace->drawn_us < LOADING_FLOOR_US) return 0;
    snprintf(pace->what, sizeof(pace->what), "%s", what);
    pace->drawn_us = now_us;
    pace->drawn = 1;
    pace->delay_us = 0;
    return 1;
}

static LoadingSink sink;
static LoadingPace pace;
static void (*quit_hook)(void);
static int active;

static uint64_t clock_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency; /* fixed at boot: asked once */
    LARGE_INTEGER count;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (uint64_t)count.QuadPart / (uint64_t)frequency.QuadPart * 1000000u +
           (uint64_t)count.QuadPart % (uint64_t)frequency.QuadPart * 1000000u / (uint64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
#endif
}

static uint64_t now_us(void) { return sink.now ? sink.now() : clock_us(); }

void Loading_SetQuit(void (*quit)(void)) { quit_hook = quit; }

void Loading_Begin(const LoadingSink *to)
{
    const char *knob;
    if (active || !to || !to->show) return;
    sink = *to;
    LoadingPace_Begin(&pace, now_us());
    if ((knob = getenv("MEMORIES_LOADING_DELAY_MS")) && *knob) pace.delay_us = (uint64_t)strtoul(knob, NULL, 10) * 1000u;
    active = 1;
}

static void draw(const char *what, unsigned done, unsigned total)
{
    char line[160];
    int shown;
    if (total) snprintf(line, sizeof(line), "%s %u of %u", what, done, total);
    else snprintf(line, sizeof(line), "%s", what);
    shown = sink.show(line, done, total);
    if (shown < 0) {
        Loading_Finish();   /* nothing to show it on: not asked again */
    } else if (shown > 0) {
        /* Closed while it loaded: there is nothing to lose, so no question. */
        Loading_Finish();
        if (quit_hook) quit_hook();
        else exit(0);
    }
}

void Loading_Step(const char *what, unsigned done, unsigned total)
{
    if (active && LoadingPace_Due(&pace, now_us(), what)) draw(what, done, total);
}

void Loading_Show(const char *what, unsigned done, unsigned total)
{
    if (active && LoadingPace_Now(&pace, now_us(), what)) draw(what, done, total);
}

void Loading_Finish(void)
{
    void (*end)(void) = sink.end;
    if (!active) return;
    active = 0;
    memset(&sink, 0, sizeof(sink));
    if (end) end();
}
