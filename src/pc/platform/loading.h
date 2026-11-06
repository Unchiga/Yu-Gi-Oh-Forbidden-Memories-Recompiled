#ifndef MEMORIES_PC_LOADING_H
#define MEMORIES_PC_LOADING_H
#include <stdint.h>

/* The loading screen: a dark window with one line of what the start-up is
 * doing ("Cards 432 of 958") and, when the total is known, a progress bar.
 * A start that takes under LOADING_DELAY_US never shows it -- not a frame --
 * so a plain start looks as it always did; a big mod's takes minutes on a
 * phone, and a window that stays empty that long reads as a hang.
 *
 * Loading_Begin is called once the window is open (main.c) and Loading_Finish
 * when the start-up is done; Loading_Step between them, from the loops the
 * start-up already runs, is a clock read when nothing is due. Outside that
 * pair, and in a headless run (the sink has nothing to show on), it does
 * nothing at all. Main thread only. */

enum {
    LOADING_DELAY_US = 500000,   /* nothing shown before the start-up has taken this long */
    LOADING_INTERVAL_US = 100000 /* the same line is drawn again at most this often */
};

/* When to draw, apart from any clock or window (tests/pc/loading_test.c). */
typedef struct LoadingPace {
    uint64_t started_us, drawn_us;
    int started, drawn;
    char what[96];  /* the label last drawn */
} LoadingPace;

void LoadingPace_Begin(LoadingPace *pace, uint64_t now_us);
/* Whether to draw `what` now (and, when it says so, it counts as drawn): not
 * before LOADING_DELAY_US since Begin; then at once for a label not drawn
 * yet (a new stage), and for the same label once LOADING_INTERVAL_US has
 * passed since it was last drawn. */
int LoadingPace_Due(LoadingPace *pace, uint64_t now_us, const char *what);

/* What shows the screen (platform.h: Platform_ShowLoading and
 * Platform_EndLoading). `show` draws `line` (and a bar done/total when total
 * is not 0) and handles the window's events; it returns 0 when drawn, 1 when
 * the player asked to quit (the window's close button, a phone's Back), and
 * -1 when this run has nothing to show it on, after which nothing asks again.
 * `end` lets go of what `show` made. `now` (the monotonic microseconds) and
 * `quit` (what to do on a quit, else exit(0)) are for tests. */
typedef struct LoadingSink {
    int (*show)(const char *line, unsigned done, unsigned total);
    void (*end)(void);
    uint64_t (*now)(void);
    void (*quit)(void);
} LoadingSink;

void Loading_Begin(const LoadingSink *sink);
/* `what` names the stage, "Cards": the line is "Cards 432 of 958" with a
 * total, "Cards" without (0). A new `what` is drawn at once, the same one a
 * few times a second at most. */
void Loading_Step(const char *what, unsigned done, unsigned total);
void Loading_Finish(void);

#endif
