/* MEMORIES_WINDOW_HIDDEN=1: an unattended run with the OpenGL picture pass. The
 * window is created hidden (sdl.c), so there is no one to answer a dialog:
 * the ones a run could raise (an error box, the disc picker, the crash and
 * hang reports' box) are left out as they are for MEMORIES_HEADLESS, and the
 * message goes to standard error. */
#ifndef WINDOW_HIDDEN_H
#define WINDOW_HIDDEN_H

#include <stdlib.h>
#include <string.h>

static inline int WindowHidden_Asked(void)
{
    const char *hidden = getenv("MEMORIES_WINDOW_HIDDEN");
    return hidden && *hidden && strcmp(hidden, "0") != 0;
}

#endif
