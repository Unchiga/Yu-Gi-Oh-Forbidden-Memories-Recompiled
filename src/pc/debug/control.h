#ifndef MEMORIES_PC_DEBUG_CONTROL_H
#define MEMORIES_PC_DEBUG_CONTROL_H
/* The control channel (notes/agent-control.md): MEMORIES_CONTROL=<port>
 * listens on 127.0.0.1:<port> for one client at a time, which drives the
 * game with the line commands of control_protocol.h. Unset, nothing here
 * does anything and the game runs as without it.
 *
 * The channel is served at the end of every VSync(0), after the state point
 * (libetc.c, Memories_StatePoint): the one place where the game's state is
 * whole, and where save states are taken. The game waits there for its
 * first client, MEMORIES_CONTROL_WAIT seconds (10 unset, negative: as long
 * as it takes). While a client is attached the game runs only when told to
 * (`step N`: until N VBlanks have passed, then to the next such point), and
 * the freeze watchdog is off. When the client goes, the game runs on by
 * itself, and a new client stops it at the next point. */
#include <stdint.h>

/* End of VSync(0), on the game's thread. */
void Control_Point(void);
/* The bits the client holds on pad 0 or 1 (`pad`), for run_vblank, which
 * ORs them into what the game reads, like MEMORIES_INPUT. Signal-safe. */
uint16_t Control_Pad(int port);
/* Pad 2 counts as connected once the client has set it. */
int Control_PadConnected(int port);
/* The pointer the client holds (`pointer`), in picture coordinates, which
 * host->pointer gives a mod in place of the real mouse: 1 with its place
 * and buttons (bit 0 left, 1 right, 2 middle), 0 while the client has none.
 * Signal-safe. */
int Control_Pointer(int *x, int *y, unsigned *buttons);
/* The notches the client turned (`wheel`) since the last call, which
 * host->wheel adds to the real wheel's. Signal-safe. */
int Control_PointerWheel(void);
/* The next key the client typed (`key`), which host->text_key gives a mod ahead
 * of the real keyboard's; 0 when none. Signal-safe. */
int Control_TextKey(void);
/* A `poke` or `jump` line a recording kept (recorder.h, C lines), done
 * again as the channel did it: 0 done, -1 not (a line of another kind, or
 * one the game refuses now). The line is modified in place. */
int Control_Apply(char *text);

#endif
