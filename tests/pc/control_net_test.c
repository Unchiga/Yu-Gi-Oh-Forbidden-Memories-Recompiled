/* Real loopback sockets: stalled readers must not hold the game thread, and
 * a stalled game must not lose a client that reads. */
#define _POSIX_C_SOURCE 200809L
#include "pc/debug/control_net.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
typedef SOCKET Socket;
#define CLOSE closesocket
#define INVALID INVALID_SOCKET
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int Socket;
#define CLOSE close
#define INVALID (-1)
static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
#endif

static int pumps, cancel_after;
static int idle(void)
{
    pumps++;
    return !cancel_after || pumps < cancel_after;
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec wait = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&wait, NULL);
#endif
}

/* A game that stalls, not a client: the window's pump (or a loaded machine's
 * scheduler) holds the game thread past the deadline once, while the client
 * reads everything it is sent. */
static Socket reader = INVALID;
static int stall_ms;
static size_t drained;
static void drain(int timeout_ms)
{
    char buffer[65536];
    for (;;) {
        fd_set set;
        struct timeval wait = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        long got;
        FD_ZERO(&set);
        FD_SET(reader, &set);
        if (select((int)reader + 1, &set, NULL, NULL, &wait) <= 0) return;
        got = (long)recv(reader, buffer, sizeof(buffer), 0);
        if (got <= 0) return;
        drained += (size_t)got;
    }
}
static int stalling_idle(void)
{
    pumps++;
    if (stall_ms) {
        sleep_ms(stall_ms);
        stall_ms = 0;
    }
    drain(0);
    return 1;
}

/* The blocked path: the reader pauses until the reply has filled the
 * socket, so the writes block and the client's clock runs; then the pump
 * holds the game 1.2 s, while the client reads all that was sent
 * (stall_reads) or takes nothing. */
static uint64_t pause_until, stall_ended;
static int stall_reads, stalled, pumps_before, pumps_after;
static int blocked_idle(void)
{
    pumps++;
    if (!stalled) {
        uint64_t end;
        if (now_ms() < pause_until) return 1;
        stalled = 1;
        pumps_before = pumps - 1;
        end = now_ms() + 1200;
        if (stall_reads) {
            while (now_ms() < end) drain(10);
        } else {
            sleep_ms(1200);
        }
        stall_ended = now_ms();
        return 1;
    }
    pumps_after++;
    if (stall_reads) drain(10);
    return 1;
}

/* The game's standard error into `path` meanwhile, for its log line. */
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define open _open
#define close_fd _close
#else
#include <fcntl.h>
#define close_fd close
#endif
static int capture_begin(const char *path)
{
    int saved, file;
    fflush(stderr);
    saved = dup(2);
    file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(saved >= 0 && file >= 0);
    dup2(file, 2);
    close_fd(file);
    return saved;
}
static int capture_end(int saved, const char *path, const char *wanted)
{
    char text[1024] = {0};
    FILE *file;
    fflush(stderr);
    dup2(saved, 2);
    close_fd(saved);
    file = fopen(path, "rb");
    assert(file);
    if (!fread(text, 1, sizeof(text) - 1, file)) text[0] = 0;
    fclose(file);
    remove(path);
    fputs(text, stderr);
    return strstr(text, wanted) != NULL;
}

/* receive_buffer: SO_RCVBUF, or 0 for the system's (a client's own). */
static Socket connect_client(unsigned port, int receive_buffer)
{
    struct sockaddr_in address;
    Socket peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(peer != INVALID);
    if (receive_buffer) {
        assert(!setsockopt(peer, SOL_SOCKET, SO_RCVBUF, (const char *)&receive_buffer, sizeof(receive_buffer)));
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    assert(!connect(peer, (struct sockaddr *)&address, sizeof(address)));
    assert(ControlNet_Accept(1000));
    return peer;
}

static void round_trip(Socket peer)
{
    char buffer[8] = {0};
    assert(send(peer, "info\n", 5, 0) == 5);
    assert(ControlNet_Receive(buffer, sizeof(buffer), 1000) == 5);
    assert(!memcmp(buffer, "info\n", 5));
    assert(!ControlNet_Gone());
    assert(!ControlNet_Send("ok\n", 3, idle));
    assert(recv(peer, buffer, sizeof(buffer), 0) == 3);
    assert(!memcmp(buffer, "ok\n", 3));
}

int main(void)
{
    static char large[8 * 1024 * 1024];
    unsigned port;
    uint64_t started, elapsed;
    Socket peer;
    memset(large, 'x', sizeof(large));
    assert(!ControlNet_Listen(0, &port));
    peer = connect_client(port, 1024);
    round_trip(peer);

    /* The peer stays connected but never consumes the large reply. */
    pumps = 0;
    started = now_ms();
    assert(ControlNet_Send(large, sizeof(large), idle) == -1);
    elapsed = now_ms() - started;
    assert(elapsed >= 900 && elapsed < 3000);
    assert(pumps > 5); /* the game's callback keeps servicing its window */
    ControlNet_Drop();
    CLOSE(peer);
    assert(ControlNet_Gone());

    /* The listener survives the timeout and serves a fresh connection. */
    peer = connect_client(port, 1024);
    round_trip(peer);
    pumps = 0;
    cancel_after = 3; /* the window asked to close while a send was pending */
    started = now_ms();
    assert(ControlNet_Send(large, sizeof(large), idle) == -1);
    assert(pumps == cancel_after && now_ms() - started < 1000);
    ControlNet_Drop();
    CLOSE(peer);
    cancel_after = 0;

    /* The blocked path: the writes block, then the game stalls past the
     * deadline while the client reads what was sent. The next write goes
     * through (it is tried before the clock is read), and the reply
     * completes. Both buffers are fixed (16 KiB to send, 1 KiB to
     * receive), so the kernel does not grow them and 512 KiB blocks on
     * every system; with the system's send buffer a CI runner's Linux took
     * about a megabyte, macOS about 460 KiB, without blocking. */
    ControlNet_SetSendBuffer(16 * 1024);
    peer = connect_client(port, 1024);
    ControlNet_SetSendBuffer(0);
    round_trip(peer);
    reader = peer;
    pumps = pumps_after = stalled = 0;
    stall_reads = 1;
    drained = 0;
    started = now_ms();
    pause_until = started + 300;
    assert(!ControlNet_Send(large, 512 * 1024, blocked_idle));
    assert(stalled && pumps_before > 3 && stall_ended - started >= 1400);
    drain(200); /* all of it is on its way: 200 ms without a byte is its end */
    assert(drained == 512 * 1024);
    round_trip(peer);
    /* Its converse: a client that takes nothing through the stall is
     * dropped at the next write, with no second wait and a log line. */
    pumps = pumps_after = stalled = 0;
    stall_reads = 0;
    pause_until = now_ms() + 300;
    {
        int saved = capture_begin("control_net_test.log");
        int result = ControlNet_Send(large, 512 * 1024, blocked_idle);
        int logged = capture_end(saved, "control_net_test.log", "the client took none of a reply for a second");
        assert(result == -1 && stalled && pumps_before > 3 && !pumps_after && now_ms() - stall_ended < 500);
        assert(logged);
    }
    ControlNet_Drop();
    CLOSE(peer);

    /* With the system's buffers, as a client has them (a 1 KiB one makes
     * a megabyte crawl through Linux loopback). */
    peer = connect_client(port, 0);
    round_trip(peer);

    /* The game stalls longer than the deadline while its client reads: a
     * short reply goes out whole, the client keeps its channel. */
    reader = peer;
    pumps = 0;
    stall_ms = 1500;
    assert(!ControlNet_Send("ok frame 1 vblank 1\n", 20, stalling_idle));
    drained = 0;
    drain(200);
    assert(drained == 20);
    /* So does a long one, with the stall in the middle of it. */
    pumps = 0;
    drained = 0;
    stall_ms = 1500;
    started = now_ms();
    assert(!ControlNet_Send(large, 1024 * 1024, stalling_idle));
    assert(pumps > 0 && now_ms() - started >= 1400);
    drain(200);
    assert(drained == 1024 * 1024);
    assert(!ControlNet_Gone());
    round_trip(peer);
    ControlNet_Drop();
    CLOSE(peer);
    puts("control net: timeout, cancellation, event pumping, a stalled game, and reconnect passed");
    return 0;
}
