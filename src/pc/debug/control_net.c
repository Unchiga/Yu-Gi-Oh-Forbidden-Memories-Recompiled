/* The control channel's socket (control_net.h). */
#define _GNU_SOURCE
#include "control_net.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Socket;
#define NO_SOCKET INVALID_SOCKET
#define close_socket closesocket
static int last_error(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
typedef int Socket;
#define NO_SOCKET (-1)
#define close_socket close
static int last_error(void) { return errno; }
#endif

static Socket listener = NO_SOCKET, client = NO_SOCKET;

#ifdef __APPLE__
/* Darwin has neither SOCK_CLOEXEC nor accept4. */
static Socket close_on_exec(Socket socket)
{
    int flags, error;
    if (socket == NO_SOCKET) return socket;
    flags = fcntl(socket, F_GETFD);
    if (flags >= 0 && fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == 0) return socket;
    error = errno;
    close_socket(socket);
    errno = error;
    return NO_SOCKET;
}
#endif

static int nonblocking(Socket socket)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled);
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int retryable(void)
{
    int error = last_error();
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

static uint64_t milliseconds(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* Wait up to timeout_ms for `socket` to have something to read (a client to
 * take, for the listener). 1 yes, 0 not in time. */
static int readable(Socket socket, int timeout_ms)
{
    fd_set set;
    struct timeval wait;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    wait.tv_sec = timeout_ms / 1000;
    wait.tv_usec = (timeout_ms % 1000) * 1000;
    return select((int)socket + 1, &set, NULL, NULL, &wait) > 0;
}

int ControlNet_Listen(unsigned port, unsigned *bound)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "memories-pc: control: Winsock did not start\n");
        return -1;
    }
    listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (listener != NO_SOCKET) {
        /* The port is this game's alone: no other program binds it too. */
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    }
#else
    int yes = 1;
#ifdef __APPLE__
    listener = close_on_exec(socket(AF_INET, SOCK_STREAM, 0));
#elif defined(__SWITCH__) /* nothing is ever exec'd there */
    listener = socket(AF_INET, SOCK_STREAM, 0);
#else
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
    /* A restart listens again at once, past the old connection's TIME_WAIT.
     * (Windows' SO_REUSEADDR would let another program take the port.) */
    if (listener != NO_SOCKET) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    if (listener == NO_SOCKET) {
        fprintf(stderr, "memories-pc: control: no socket (error %d)\n", last_error());
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* this machine only */
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0) {
        fprintf(stderr, "memories-pc: control: cannot listen on 127.0.0.1:%u (error %d)\n", port, last_error());
        close_socket(listener);
        listener = NO_SOCKET;
        return -1;
    }
    *bound = ntohs(address.sin_port);
    return 0;
}

int ControlNet_Accept(int timeout_ms)
{
    int yes = 1;
    if (client != NO_SOCKET) return 1;
    if (listener == NO_SOCKET || !readable(listener, timeout_ms)) return 0;
#ifdef _WIN32
    client = accept(listener, NULL, NULL);
    if (client != NO_SOCKET) SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
#elif defined(__APPLE__)
    client = close_on_exec(accept(listener, NULL, NULL));
#elif defined(__SWITCH__)
    client = accept(listener, NULL, NULL);
#else
    client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
#endif
    if (client == NO_SOCKET) return 0;
    if (nonblocking(client)) {
        close_socket(client);
        client = NO_SOCKET;
        return 0;
    }
    /* One short line each way per command: no waiting to fill a packet. */
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
    return 1;
}

void ControlNet_RefuseOthers(const char *reply)
{
    Socket other;
    if (listener == NO_SOCKET || client == NO_SOCKET || !readable(listener, 0)) return;
    other = accept(listener, NULL, NULL);
    if (other == NO_SOCKET) return;
    if (nonblocking(other)) {
        close_socket(other);
        return;
    }
#ifdef _WIN32
    send(other, reply, (int)strlen(reply), 0);
    shutdown(other, SD_SEND);
#else
    send(other, reply, strlen(reply), MSG_NOSIGNAL);
    shutdown(other, SHUT_WR);
#endif
    close_socket(other);
}

int ControlNet_Gone(void)
{
    char byte;
    int count;
    if (client == NO_SOCKET) return 1;
    if (!readable(client, 0)) return 0;
    /* Readable with nothing to read: the client closed its end. */
    count = (int)recv(client, &byte, 1, MSG_PEEK);
    return count == 0 || (count < 0 && !retryable());
}

long ControlNet_Receive(char *buffer, size_t size, int timeout_ms)
{
    long count;
    if (client == NO_SOCKET) return -1;
    if (!size || !readable(client, timeout_ms)) return 0;
    count = (long)recv(client, buffer, (int)size, 0);
    if (count < 0 && retryable()) return 0;
    return count > 0 ? count : -1; /* 0: the client closed its end */
}

int ControlNet_Send(const char *data, size_t size, int (*idle)(void))
{
    uint64_t deadline = milliseconds() + 1000;
    while (size && client != NO_SOCKET) {
        long sent;
        /* Bound each write so large replies regularly service the window. */
        int chunk = size > 65536 ? 65536 : (int)size;
        if ((idle && !idle()) || milliseconds() >= deadline) return -1;
#ifdef _WIN32
        sent = (long)send(client, data, chunk, 0);
#else
        sent = (long)send(client, data, (size_t)chunk, MSG_NOSIGNAL); /* a closed client is no SIGPIPE */
#endif
        if (sent < 0 && retryable()) {
            fd_set writable;
            struct timeval wait = {0, 10000};
            FD_ZERO(&writable);
            FD_SET(client, &writable);
            /* Ten milliseconds at most before servicing the window again.
             * The next nonblocking send handles readiness/errors, including
             * a wait interrupted by the game's timer. */
            select((int)client + 1, NULL, &writable, NULL, &wait);
            continue;
        }
        if (sent <= 0) return -1;
        data += sent;
        size -= (size_t)sent;
    }
    return client == NO_SOCKET ? -1 : 0;
}

void ControlNet_Drop(void)
{
    char rest[256];
    int waited;
    if (client == NO_SOCKET) return;
    /* The last reply first (a `quit`'s "ok", with the process about to
     * end): end the sending side, then wait a moment for the client to
     * close its own. */
#ifdef _WIN32
    shutdown(client, SD_SEND);
#else
    shutdown(client, SHUT_WR);
#endif
    for (waited = 0; waited < 500 && readable(client, 50); waited += 50) {
        if (recv(client, rest, sizeof(rest), 0) <= 0) break;
    }
    close_socket(client);
    client = NO_SOCKET;
}
