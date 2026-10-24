#ifndef MEMORIES_PC_TEXT_MESSAGE_WAIT_H
#define MEMORIES_PC_TEXT_MESSAGE_WAIT_H
/* A message box whose text will never end, and the wait for it
 * (notes/text-control-bytecode.md).
 *
 * The name entry shows the disc's own text, and a community image's text is
 * its own: the string the retail code asks for by id is the mod's string at
 * that id, and it may have no end of its own. The box then prints its
 * letters and never finishes, and the game waits for a text that will never
 * be done.
 *
 * The retail's own text ends: the box posts its end (TEXT_BOX_FLAG_DONE)
 * and waits for the player's press, as it always did. Only a box that has
 * printed all it will print and has not ended is given up on. */

/* The waits the port keeps, one a box: the name entry's message, and the
 * story's dialog. Each has its own count, so one box's wait does not
 * disturb another's. */
#define MESSAGE_WAIT_NAME_ENTRY 0
#define MESSAGE_WAIT_STORY 1
#define MESSAGE_WAIT_SLOTS 2

/* How long a box may print nothing before the wait for it is given up on,
 * once the player has asked for the message to go on: two seconds. */
#define MESSAGE_WAIT_FRAMES 120

/* How long the wait lasts in all, whether the box prints or not: thirty
 * seconds. */
#define MESSAGE_WAIT_LIMIT 1800

/* The wait for a message box, begun when a box is made. */
void MessageWait_Begin(int slot);

/* A frame of the wait: the box printed `printed` letters so far, and it has
 * ended (`ended` is nonzero) or it has not. 1 when the wait is over and the
 * game should go on: the box printed nothing for MESSAGE_WAIT_FRAMES frames
 * in a row, or the wait has lasted MESSAGE_WAIT_LIMIT frames.
 *
 * The wait ends only once the player has asked for the message to go on
 * (`asked`), so a message the player is still reading is left alone.
 * A box that has ended its text is never given up on: the retail's own
 * text ends, and its wait is the retail's own. */
int MessageWait_Frame(int slot, int asked, int printed, int ended);

#endif
