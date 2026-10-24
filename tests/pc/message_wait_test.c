/* The wait for a message box whose text will never end, of
 * src/pc/text/message_wait.c (notes/text-control-bytecode.md).
 *
 * The name entry shows the disc's own text, and a community image's text is
 * its own: the string at the retail's id may have no end of its own, and the
 * box then prints its letters and never finishes. What is checked here is
 * when the wait for such a box is given up on, and that the retail's own
 * wait (a box that ends its text, and waits for the player's press) is left
 * alone. */
#include "pc/text/message_wait.h"
#include <assert.h>

/* A box, run over frames: the player asks for the message to go on at frame
 * `ask_at` (-1 when the player never asks), the box prints one letter on
 * each frame up to `print_until` (-1 when the box prints nothing at all),
 * and the box's text ends at frame `end_at` (-1 when the text never ends).
 * The frame the wait is over on, or -1 when it is not over. */
static int run(int slot, int ask_at, int print_until, int end_at, int frames)
{
    int i;
    int letters = 0;
    MessageWait_Begin(slot);
    for (i = 0; i < frames; i++) {
        int printed;
        int ended;
        if (print_until >= 0 && i <= print_until) letters = i + 1;
        printed = letters;
        ended = (end_at >= 0 && i >= end_at);
        if (MessageWait_Frame(slot, i == ask_at, printed, ended)) return i;
    }
    return -1;
}

int main(void)
{
    /* The retail's own box: it prints its text, it ends its text, and it
     * waits for the player's press. Nothing is given up on, however long the
     * wait lasts and however often the player asks. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, -1, 20, 25, 600) == -1);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 5, 20, 25, 600) == -1);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, 20, 25, 600) == -1);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, -1, 5, 600) == -1);

    /* The retail's own box, asked for in the middle of its printing: it
     * still ends its text on its own. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 10, 60, 90, 600) == -1);

    /* A box that never ends and prints nothing, and that the player never
     * asks for: a message the player is still reading is left alone. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, -1, -1, -1, 600) == -1);

    /* A box that never ends and prints nothing, and that the player has
     * asked for: the wait is over once the box has printed nothing for two
     * seconds. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, -1, -1, 600) == MESSAGE_WAIT_FRAMES - 1);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 30, -1, -1, 600) == 30 + MESSAGE_WAIT_FRAMES - 1);

    /* A box that never ends, prints for a while and then stops (the community
     * image's own text at the retail's id): the wait goes on while the
     * letters come, and is over two seconds after the last one. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 10, 70, -1, 600) == 70 + MESSAGE_WAIT_FRAMES);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, -1, 70, -1, 600) == -1);

    /* A box whose text never ends and never stops printing: the wait is
     * given up on after thirty seconds all the same. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, 1000000, -1, 5000) == MESSAGE_WAIT_LIMIT - 1);

    /* The wait is begun again for the next box, so the next box starts over:
     * a box that prints nothing is given up on only after its own two
     * seconds, not the ones of the box before it. */
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, -1, -1, 600) == MESSAGE_WAIT_FRAMES - 1);
    assert(run(MESSAGE_WAIT_NAME_ENTRY, 0, -1, -1, 600) == MESSAGE_WAIT_FRAMES - 1);

    /* Each box has its own wait: the story's dialog does not disturb the
     * name entry's message, and the other way round. */
    MessageWait_Begin(MESSAGE_WAIT_NAME_ENTRY);
    MessageWait_Begin(MESSAGE_WAIT_STORY);
    assert(MessageWait_Frame(MESSAGE_WAIT_NAME_ENTRY, 1, 42, 0) == 0);
    assert(MessageWait_Frame(MESSAGE_WAIT_STORY, 1, 42, 0) == 0);
    {
        int i;
        for (i = 0; i < MESSAGE_WAIT_FRAMES - 1; i++) {
            assert(MessageWait_Frame(MESSAGE_WAIT_NAME_ENTRY, 0, 42, 0) == 0);
        }
        /* The name entry's wait is over; the story's is not (it has counted
         * one frame less). */
        assert(MessageWait_Frame(MESSAGE_WAIT_NAME_ENTRY, 0, 42, 0) == 1);
        assert(MessageWait_Frame(MESSAGE_WAIT_STORY, 0, 42, 0) == 0);
    }

    return 0;
}
