/* A message box whose text will never end, and the wait for it
 * (message_wait.h, notes/text-control-bytecode.md).
 *
 * The name entry shows the disc's own text, and a community image's text is
 * its own: the string the retail code asks for by id is the mod's string at
 * that id, and it may have no end of its own. The box then prints its
 * letters and never finishes, and the game waits for a text that will never
 * be done.
 *
 * The retail's own text ends: the box posts its end and waits for the
 * player's press, as it always did. Only a box that has printed all it will
 * print and has not ended is given up on, so the retail's own wait is left
 * as it was. */
#include "message_wait.h"

/* One wait: whether the player has asked for the message to go on; the
 * frames since they did; the frames in a row the box has printed nothing;
 * the letters the box had printed when it was last seen printing. */
typedef struct {
    int asked;
    unsigned since;
    unsigned waited;
    int printed;
} MessageWait;

static MessageWait waits[MESSAGE_WAIT_SLOTS];

void MessageWait_Begin(int slot)
{
    MessageWait *wait;
    if (slot < 0 || slot >= MESSAGE_WAIT_SLOTS) return;
    wait = &waits[slot];
    wait->asked = 0;
    wait->since = 0;
    wait->waited = 0;
    wait->printed = 0;
}

int MessageWait_Frame(int slot, int asked_now, int printed_now, int ended)
{
    MessageWait *wait;
    if (slot < 0 || slot >= MESSAGE_WAIT_SLOTS) return 0;
    wait = &waits[slot];

    /* The box ended its text: the retail's own wait for the player's press
     * goes on as it always did. */
    if (ended) return 0;

    /* The player asked for the message to go on. A message the player is
     * still reading is left alone. */
    if (asked_now) {
        wait->asked = 1;
        wait->since = 0;
    }
    if (wait->asked == 0) return 0;
    wait->since++;

    /* A letter came: the box is still printing, so the wait goes on. */
    if (printed_now != wait->printed) {
        wait->printed = printed_now;
        wait->waited = 0;
    } else {
        wait->waited++;
    }

    /* The box printed nothing for two seconds: it will print nothing more,
     * and the game goes on without it. */
    if (wait->waited >= MESSAGE_WAIT_FRAMES) return 1;

    /* The wait has lasted thirty seconds: the game goes on. */
    if (wait->since >= MESSAGE_WAIT_LIMIT) return 1;
    return 0;
}
