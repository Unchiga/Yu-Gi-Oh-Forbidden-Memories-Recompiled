#ifndef MEMORIES_PC_DEBUG_CONTROL_PROTOCOL_H
#define MEMORIES_PC_DEBUG_CONTROL_PROTOCOL_H
/* The control channel's text protocol (control.h, notes/agent-control.md):
 * one command a line, one reply line a command ("ok ..." or "err ...").
 * Only parsing and framing here, with nothing of the game or of sockets, so
 * that CTest checks it on its own (tests/pc/control_protocol_test.c).
 *
 *     step N            run N VBlanks (decimal), stop at the next safe point
 *     pad P BITS        hold pad P's (1 or 2) active-high bits (hex) from now on
 *     shot PATH         the presented picture as PNG (PPM when PATH ends in .ppm)
 *     hash              FNV-1a of all of VRAM, as MEMORIES_FRAME_HASHES writes it
 *     peek ADDR LEN     LEN bytes (decimal) of guest memory at ADDR (hex)
 *     poke ADDR HEX     write the bytes HEX spells at ADDR (hex)
 *                       ADDR may be @NAME[+OFF] (OFF hex): the game variable
 *                       NAME wherever this build keeps it, which for the
 *                       game's small data is not its retail address
 *     save PATH         a save state
 *     load PATH         resume a save state
 *     info              frame, VBlank, mode, build id
 *     jump TARGET [OPPONENT [DECK]]
 *                       another screen through the debug menu (title_jump.h):
 *                       title, debug, duel, free_duel, build_deck, library,
 *                       password, map, credits, options; a duel against
 *                       OPPONENT (decimal) with DECK ("723-762", "1,2,3")
 *     quit              end the game
 *
 * Numbers that are hex may carry a 0x; decimal ones may be given as 0x...
 * too. A path is the rest of the line, spaces included. */
#include <stddef.h>
#include <stdint.h>

#define CONTROL_LINE_MAX 65536u   /* the longest command, its newline excluded */
#define CONTROL_DATA_MAX 16384u   /* the most bytes one peek or poke moves */

typedef enum {
    CONTROL_STEP,
    CONTROL_PAD,
    CONTROL_SHOT,
    CONTROL_HASH,
    CONTROL_PEEK,
    CONTROL_POKE,
    CONTROL_SAVE,
    CONTROL_LOAD,
    CONTROL_INFO,
    CONTROL_JUMP,
    CONTROL_QUIT
} ControlKind;

typedef struct {
    ControlKind kind;
    uint32_t count;   /* step: VBlanks */
    int pad;          /* pad: 0 for pad 1, 1 for pad 2 */
    uint16_t bits;    /* pad */
    uint32_t address; /* peek, poke: as given (normalised by ControlProtocol_GuestRange) */
    const char *symbol; /* peek, poke: the variable's name when ADDR was @NAME (inside the line); else NULL */
    uint32_t offset;  /* peek, poke: the +OFF after @NAME */
    uint32_t length;  /* peek: bytes asked for; poke: bytes decoded into the caller's buffer */
    const char *path; /* shot, save, load: inside the parsed line; jump: the target */
    int opponent;     /* jump */
    const char *deck; /* jump: NULL when not given */
} ControlCommand;

/* Parse one line (without its newline; a trailing '\r' is ignored). `data`
 * receives a poke's bytes and has room for CONTROL_DATA_MAX of them. 0 on
 * success, -1 with the reason in `error`. The line is modified in place. */
int ControlProtocol_Parse(char *line, ControlCommand *command, uint8_t *data, char *error, size_t error_size);

/* Where guest bytes [address, address + length) are: KSEG0 (0x80000000),
 * KSEG1 (0xA0000000) and physical addresses all name the same 2 MiB of RAM
 * or the 1 KiB scratchpad at 0x1F800000. Sets *scratchpad (0 RAM, 1 the
 * scratchpad) and *offset from the start of it. 0 on success, -1 when the
 * range leaves both or crosses an end. */
int ControlProtocol_GuestRange(uint32_t address, uint32_t length, int *scratchpad, uint32_t *offset);

/* Lowercase hex of `size` bytes into `out` (2 * size + 1 bytes). */
void ControlProtocol_Hex(const uint8_t *bytes, size_t size, char *out);

/* Bytes received, split into lines. */
typedef struct {
    char data[CONTROL_LINE_MAX + 2];
    size_t used;
    int dropping; /* inside a line that was too long, until its newline */
} ControlLines;

/* Where the next bytes received go, and how many fit. */
char *ControlLines_Room(ControlLines *lines, size_t *room);
void ControlLines_Added(ControlLines *lines, size_t count);
/* The next whole line into `out` (CONTROL_LINE_MAX + 1 bytes), without its
 * newline: 1 a line, 0 none yet, -1 a line longer than CONTROL_LINE_MAX was
 * dropped (reported once, when its end arrives or the buffer fills). */
int ControlLines_Next(ControlLines *lines, char *out);

#endif
