#ifndef MEMORIES_PC_DEBUG_RECORDER_H
#define MEMORIES_PC_DEBUG_RECORDER_H
/* The input recorder and player (notes/agent-control.md, step 3).
 *
 * MEMORIES_RECORD=<path> writes, as text, the pad bits the game is given at
 * every VBlank from every source (the keyboard, the controllers, the mouse,
 * MEMORIES_INPUT and a control client, already ORed), at the one point they
 * all pass (run_vblank, libetc.c), before the mods' INPUT hooks and View >
 * Japanese buttons, which act on them again when they are played back.
 * Lines, one kind a letter:
 *
 *     yfm-recording 1
 *     build <id>                      the build that made it
 *     F <key>: <value>                build, os, settings and mods, as the
 *                                     crash reports' facts give them
 *     start boot | start state <path> (MEMORIES_LOAD_STATE: the recording
 *                                     begins at the first safe point after
 *                                     the state is loaded)
 *     I <index> <p1> <f1> <p2> <f2> <c2>
 *                                     from VBlank <index> on (counted from
 *                                     the start) pad 1 and pad 2 hold these
 *                                     bits, of which f1/f2 are exempt from
 *                                     the Japanese buttons' exchange; c2 is
 *                                     1 when pad 2 is connected. Written only
 *                                     when something changed.
 *     H <index> <frame> <hash>        at every safe point (the end of a
 *                                     VSync(0)): the VBlank, the presented
 *                                     frame and Memories_VramHash
 *     S <index> <frame> <path>        a save state taken there, every
 *                                     MEMORIES_RECORD_STATES VBlanks
 *     C <index> <frame> <line>        a control client's `poke` or `jump`,
 *                                     taken at the safe point of that VBlank
 *                                     and frame (control.c): done again at
 *                                     the same point, after its H line, so a
 *                                     session that jumps to a screen or
 *                                     arranges a deck replays from the pads
 *                                     and these alone
 *     E <index> <frame>               the end
 *
 * MEMORIES_PLAY=<path> plays such a file back: the pads get its bits at
 * each VBlank instead of the live ones (which are ignored), from the same
 * start, does the C lines again at their points, and the game ends at its E
 * line. Recording while playing gives the lines to compare
 * (tools/pc/replay.py), C lines included. Host actions (F5/F7, the deck
 * slots, the menus) and the client's `load` are not recorded.
 *
 * Unset, both cost one test per VBlank and one per frame. */
#include <stdint.h>

/* ResetCallback: read MEMORIES_PLAY's file, open MEMORIES_RECORD's. */
void Recorder_Init(void);
/* run_vblank, before anything reads the pads: the bits from every source
 * and the Japanese-buttons-exempt part of them, by pad, and whether pad 2
 * is connected. Playing, they are replaced by the file's. Signal-safe. */
void Recorder_Pads(uint16_t bits[2], uint16_t fixed[2], int *pad2_connected);
/* A mod's host->pad: while playing, the bits the last VBlank was given
 * (what the live pad read between two VBlanks when recorded); else `live`. */
uint16_t Recorder_HostPad(int port, uint16_t live);
/* The end of VSync(0), with the presented frame count. */
void Recorder_Point(unsigned frame);
/* A control client's poke or jump, done at the point Recorder_Point just
 * passed (control.c): kept as a C line. */
void Recorder_Command(const char *line);

#endif
