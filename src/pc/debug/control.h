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
/* A `poke` or `jump` line a recording kept (recorder.h, C lines), done
 * again as the channel did it: 0 done, -1 not (a line of another kind, or
 * one the game refuses now). The line is modified in place. */
int Control_Apply(char *text);

#endif
