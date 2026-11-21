#ifndef MEMORIES_PC_DEBUG_CONTROL_NET_H
#define MEMORIES_PC_DEBUG_CONTROL_NET_H
/* The control channel's socket (control.h): TCP on 127.0.0.1, one client at
 * a time, through Winsock or POSIX sockets. Kept apart from control.c, which
 * includes game headers that clash with <windows.h>. Neither socket is
 * inherited by a process the game starts, so a restart can listen again. */
#include <stddef.h>

/* Listen on 127.0.0.1:port (0: a port the system picks); *bound gets the
 * port. 0 on success, -1 with the reason on stderr. */
int ControlNet_Listen(unsigned port, unsigned *bound);
/* Take a waiting client, waiting up to timeout_ms for one (0: only look).
 * 1 when one is connected now, 0 when none came. */
int ControlNet_Accept(int timeout_ms);
/* For tests: the send buffer (SO_SNDBUF) of the clients taken from now on,
 * 0 for the system's. A fixed one is not grown by the kernel, so a test's
 * writes block after the same few kilobytes on every system (Linux grows
 * its own to megabytes, macOS to hundreds of kilobytes). The game never
 * sets it. */
void ControlNet_SetSendBuffer(int bytes);
/* Bytes from the client, waiting up to timeout_ms: the count, 0 when none
 * came in time, -1 when the client has gone. */
long ControlNet_Receive(char *buffer, size_t size, int timeout_ms);
/* All of `size` bytes to the client. The deadline is the client's: a second
 * in which the socket stays full and the client takes not one byte; time the
 * game itself is held is not counted against a client that reads. idle is
 * called between writes and while waiting, to service the window; return 0
 * to cancel. -1 on disconnect, timeout, or cancellation; the caller drops
 * the client. NULL idle is allowed for callers without a window. */
int ControlNet_Send(const char *data, size_t size, int (*idle)(void));
/* While a client is attached: answer any other that connects with `reply`
 * and close it, so it is told rather than left waiting. */
void ControlNet_RefuseOthers(const char *reply);
/* Without waiting: 1 when the client has closed its end (or there is none). */
int ControlNet_Gone(void);
/* Close the client's connection; the listener stays. */
void ControlNet_Drop(void);

#endif
