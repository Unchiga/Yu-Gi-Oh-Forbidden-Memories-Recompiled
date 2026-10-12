/* HTTP(S) GETs for the update check (update_net.h), without a TLS library
 * in the executable: Windows has WinHTTP, and on Linux curl is part of
 * practically every installation, so the game runs it and reads what it
 * prints. Called on the check's own thread only. */
#define _GNU_SOURCE
#include "update_net.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include "pc/compat/fs.h"
#else
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
extern char **environ;
#endif

#define USER_AGENT "yfm-redecomp-updater"

static void say(char *why, size_t size, const char *text)
{
    if (why && size) snprintf(why, size, "%s", text);
}

#ifdef _WIN32

int UpdateNet_Get(const char *url, int timeout_seconds, UpdateNetSink sink, void *context, char *why, size_t why_size)
{
    wchar_t *wide = Memories_Utf8ToWide(url), host[256], *path;
    URL_COMPONENTS parts;
    HINTERNET session = NULL, connection = NULL, request = NULL;
    DWORD status = 0, length = sizeof(status), protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    int result = -1, milliseconds = timeout_seconds * 1000;
    if (!wide) { say(why, why_size, "Bad address."); return -1; }
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = sizeof(host) / sizeof(host[0]);
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide, 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
        free(wide);
        say(why, why_size, "Bad address.");
        return -1;
    }
    path = parts.lpszUrlPath; /* the path runs on into the query string */
    session = WinHttpOpen(L"" USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) {
        /* Windows 7 and 8 leave TLS 1.2 off unless asked; GitHub needs it. */
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        WinHttpSetTimeouts(session, milliseconds, milliseconds, milliseconds, milliseconds);
        connection = WinHttpConnect(session, host, parts.nPort, 0);
    }
    if (connection)
        request = WinHttpOpenRequest(connection, L"GET", path && *path ? path : L"/", NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json, */*\r\n", (DWORD)-1L,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &length, WINHTTP_NO_HEADER_INDEX)) {
        if (status == 200) {
            char buffer[65536];
            DWORD got = 0;
            result = 0;
            for (;;) {
                if (!WinHttpReadData(request, buffer, sizeof(buffer), &got)) { result = -1; break; }
                if (!got) break;
                if (sink(buffer, got, context)) { result = -1; say(why, why_size, "Stopped."); break; }
            }
            if (result && why && !*why) say(why, why_size, "The download was interrupted.");
        } else {
            char text[64];
            snprintf(text, sizeof(text), "The server answered %lu.", (unsigned long)status);
            say(why, why_size, text);
        }
    } else {
        say(why, why_size, "Could not reach the server.");
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(wide);
    return result;
}

#elif defined(__ANDROID__) || defined(__SWITCH__)

/* No curl to run in an app (android.c turns the check off) or on the
 * Switch; an HTTP client there is for later. */
int UpdateNet_Get(const char *url, int timeout_seconds, UpdateNetSink sink, void *context, char *why, size_t why_size)
{
    (void)url; (void)timeout_seconds; (void)sink; (void)context;
    say(why, why_size, "no update check on this system yet");
    return -1;
}

#else

int UpdateNet_Get(const char *url, int timeout_seconds, UpdateNetSink sink, void *context, char *why, size_t why_size)
{
    char seconds[16], buffer[65536];
    /* --disable (first) ignores the player's .curlrc, which could send the
     * body elsewhere or mix headers into it. */
    char *argv[] = {"curl", "--disable", "--silent", "--show-error", "--fail", "--location", "--proto", "=https,http",
                    "--connect-timeout", "10", "--max-time", seconds, "--user-agent", USER_AGENT,
                    "--header", "Accept: application/vnd.github+json, */*", "--", (char *)url, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t none, defaults;
    int pipe_ends[2], status = 0, result = 0, stopped = 0;
    pid_t child;
    ssize_t got;
    snprintf(seconds, sizeof(seconds), "%d", timeout_seconds);
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) { say(why, why_size, "Bad address."); return -1; }
    /* Both ends close-on-exec (the dup2 onto curl's stdout clears it there):
     * a program the main thread starts meanwhile (the browser, a restart)
     * must not inherit the write end, or the read below never sees EOF. */
 #ifdef __APPLE__
    if (pipe(pipe_ends)) { say(why, why_size, "Could not start curl."); return -1; }
    if (fcntl(pipe_ends[0], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(pipe_ends[1], F_SETFD, FD_CLOEXEC) == -1) {
        int saved_error = errno;
        close(pipe_ends[0]); close(pipe_ends[1]); errno = saved_error;
        say(why, why_size, "Could not start curl."); return -1;
    }
 #else
    if (pipe2(pipe_ends, O_CLOEXEC)) { say(why, why_size, "Could not start curl."); return -1; }
 #endif
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], 1);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    /* This thread holds every signal off (the game's clock is SIGALRM on the
     * main thread); curl gets the ordinary mask and default handlers. */
    posix_spawnattr_init(&attributes);
    sigemptyset(&none);
    sigfillset(&defaults);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    result = posix_spawnp(&child, "curl", &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipe_ends[1]);
    if (result) {
        close(pipe_ends[0]);
        say(why, why_size, result == ENOENT ? "curl is not installed." : "Could not start curl.");
        return -1;
    }
    while ((got = read(pipe_ends[0], buffer, sizeof(buffer))) != 0) {
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!stopped && sink(buffer, (size_t)got, context)) {
            stopped = 1;
            kill(child, SIGTERM);
        }
    }
    close(pipe_ends[0]);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (stopped) { say(why, why_size, "Stopped."); return -1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        say(why, why_size, code == 127 ? "curl is not installed." : code == 22 ? "The server refused the request."
                         : code == 28 ? "The server took too long to answer." : "Could not reach the server.");
        return -1;
    }
    return 0;
}

#endif
