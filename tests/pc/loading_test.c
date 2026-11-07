/* The loading screen's pacing (src/pc/platform/loading.c): nothing shown for
 * a start that takes under half a second, a new stage drawn at once after
 * it and the same one at most ten times a second, no calls outside
 * Begin/Finish, and a platform with nothing to show on asked no more. */
#include "pc/platform/loading.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t clock_now;
static int shown, ended, quits, answer;
static char last_line[160];
static unsigned last_done, last_total;

static uint64_t fake_now(void) { return clock_now; }
static int fake_show(const char *line, unsigned done, unsigned total)
{
    shown++;
    snprintf(last_line, sizeof(last_line), "%s", line);
    last_done = done;
    last_total = total;
    return answer;
}
static void fake_end(void) { ended++; }
static void fake_quit(void) { quits++; }
static const LoadingSink sink = {fake_show, fake_end, fake_now};

static void reset(void)
{
    clock_now = 1000000;
    shown = ended = quits = answer = 0;
    last_line[0] = '\0';
    Loading_SetQuit(fake_quit);
}

static void pace_alone(void)
{
    LoadingPace pace;
    const uint64_t t0 = 5000000, shown_at = t0 + LOADING_DELAY_US;
    LoadingPace_Begin(&pace, t0);
    assert(!LoadingPace_Due(&pace, t0, "Cards"));
    assert(!LoadingPace_Due(&pace, shown_at - 1, "Cards"));
    assert(LoadingPace_Due(&pace, shown_at, "Cards"));
    /* The same stage waits for its interval... */
    assert(!LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US - 1, "Cards"));
    assert(LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US, "Cards"));
    /* ...another goes at once, but not within the floor of the last draw. */
    assert(!LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US + LOADING_FLOOR_US - 1, "Tables"));
    assert(LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US + LOADING_FLOOR_US, "Tables"));
    assert(!LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US + LOADING_FLOOR_US + 1, "Tables"));
    /* Back to a label not drawn last: at once, past the floor. */
    assert(LoadingPace_Due(&pace, shown_at + LOADING_INTERVAL_US + 2 * LOADING_FLOOR_US, "Cards"));
}

static void fast_start(void)
{
    unsigned i;
    reset();
    Loading_Step("Cards", 1, 2); /* before Begin: nothing */
    assert(!shown);
    Loading_Begin(&sink);
    for (i = 0; i < 1000; i++) { /* a start of 0.499 s, a step every half millisecond */
        clock_now += 499;
        Loading_Step("Cards", i, 1000);
    }
    assert(!shown && "a quick start shows nothing");
    Loading_Step("Starting the game", 0, 0);
    Loading_Finish();
    assert(!shown && ended == 1);
    Loading_Step("Cards", 1, 2); /* after Finish: nothing */
    assert(!shown);
}

static void slow_start(void)
{
    unsigned i;
    reset();
    Loading_Begin(&sink);
    for (i = 0; i < 3000; i++) { /* 3 s, a step a millisecond */
        clock_now += 1000;
        Loading_Step("Cards", i, 3000);
    }
    /* From 0.5 s to 3 s at ten a second: 25, give or take the edges. */
    assert(shown >= 24 && shown <= 26);
    assert(!strncmp(last_line, "Cards ", 6) && last_total == 3000 && last_done > 2800);
    assert(strstr(last_line, "of 3000"));
    Loading_Step("Tables", 0, 0);
    assert(!strncmp(last_line, "Cards ", 6) && "not within the floor of the last draw");
    clock_now += LOADING_FLOOR_US;
    Loading_Step("Tables", 0, 0);
    assert(!strcmp(last_line, "Tables") && last_total == 0 && "a new stage at once, without a count");
    Loading_Finish();
    assert(ended == 1 && !quits);
    Loading_Finish();
    assert(ended == 1);
}

static void big_file(void)
{
    LoadingPace pace;
    reset();
    Loading_Begin(&sink);
    clock_now += 10000; /* 10 ms in: far under the delay */
    Loading_Step("Reading mods", 0, 3);
    assert(!shown && "a step waits for the delay");
    Loading_Show("Reading mod big", 0, 0);
    assert(shown == 1 && !strcmp(last_line, "Reading mod big") && "a big file's label at once");
    clock_now += 1000;
    Loading_Show("Reading mod bigger", 0, 0);
    assert(shown == 1 && "but not within the floor");
    clock_now += LOADING_FLOOR_US;
    Loading_Step("Reading mods", 1, 3); /* the delay is over once the screen is up */
    assert(shown == 2 && !strcmp(last_line, "Reading mods 1 of 3"));
    Loading_Finish();
    Loading_Show("Reading mod big", 0, 0); /* outside Begin/Finish: nothing */
    assert(shown == 2);
    LoadingPace_Begin(&pace, 0);
    assert(LoadingPace_Now(&pace, 1, "x") && !LoadingPace_Due(&pace, 2, "x"));
    assert(LoadingPace_Due(&pace, 2 + LOADING_FLOOR_US, "y") && "delay_us is 0 after a forced draw");
}

static void without_a_window(void)
{
    reset();
    answer = -1;
    Loading_Begin(&sink);
    clock_now += 2000000;
    Loading_Step("Cards", 1, 2);
    assert(shown == 1 && ended == 1);
    clock_now += 2000000;
    Loading_Step("Tables", 0, 0);
    assert(shown == 1 && "not asked again");
}

static void closed_while_loading(void)
{
    reset();
    answer = 1;
    Loading_Begin(&sink);
    clock_now += 2000000;
    Loading_Step("Cards", 1, 2);
    assert(shown == 1 && quits == 1 && ended == 1);
}

int main(void)
{
    pace_alone();
    fast_start();
    slow_start();
    big_file();
    without_a_window();
    closed_while_loading();
    puts("loading screen: nothing before half a second, ten a second at most, a stage at once, none outside Begin/Finish passed");
    return 0;
}
