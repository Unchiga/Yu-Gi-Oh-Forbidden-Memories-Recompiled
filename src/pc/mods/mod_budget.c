/* mod_budget.h. refused-mods.txt: a line "id<TAB>size<TAB>time<TAB>build<TAB>why" per
 * mod, and "restarted" alone. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE /* _SC_PHYS_PAGES */
#include "pc/compat/fs.h"
#include "mod_budget.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Measured on a Galaxy A22 (4 GB): the game with More Cards takes 200 MB; 8.56 M
 * fusions from 789 MB of JSON peak at 150 MB of rows while sorted, and the rest of
 * that mod's data is its cards. A quarter of the file covers it. */
#define GAME_BYTES (200ll << 20)

long long ModBudget_Bytes(void)
{
    const char *named = getenv("MEMORIES_MOD_BUDGET_MB");
    long long total = 0;
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) total = (long long)status.ullTotalPhys;
#else
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0) total = (long long)pages * size;
#endif
    if (named && *named) {
        char *end;
        long long megabytes = strtoll(named, &end, 10);
        /* A value that makes no sense refuses nothing. */
        if (!*end && megabytes > 0 && megabytes < 1ll << 40) return megabytes << 20;
    }
    if (sizeof(void *) == 4 && total > (1ll << 31)) total = 1ll << 31; /* what a 32-bit game can reach */
    if (!total) return 1ll << 40; /* cannot tell: nothing is refused for it */
    total = total / 5 * 3 - GAME_BYTES;
    return total > 0 ? total : 0;
}

long long ModBudget_Need(long long manifest_bytes)
{
    return manifest_bytes / 4;
}

static int same(const char *text, const char *id, long long size, long long time, unsigned build, char *why,
                size_t why_size)
{
    size_t length = strlen(id);
    long long saved_size, saved_time;
    unsigned saved_build;
    int used = 0;
    if (strncmp(text, id, length) || text[length] != '\t') return 0;
    if (sscanf(text + length + 1, "%lld\t%lld\t%u\t%n", &saved_size, &saved_time, &saved_build, &used) != 3 || !used)
        return 0;
    if (saved_size != size || saved_time != time || saved_build != build) return 0;
    if (why && why_size) snprintf(why, why_size, "%s", text + length + 1 + used);
    return 1;
}

int ModBudget_Refused(const char *path, const char *id, long long size, long long time, unsigned build, char *why,
                      size_t why_size)
{
    FILE *file = fopen(path, "rb");
    char line[1024];
    int found = 0;
    if (!file) return 0;
    while (!found && fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        found = same(line, id, size, time, build, why, why_size);
    }
    fclose(file);
    return found;
}

/* The file again, without `skip` lines; then `add`. Whether it was written. */
static int rewrite(const char *path, int (*skip)(const char *line, const void *what), const void *what, const char *add)
{
    char partial[1100], line[1024];
    FILE *in, *out;
    int ok;
    if (snprintf(partial, sizeof(partial), "%s.tmp", path) >= (int)sizeof(partial)) return 0;
    if (!(out = fopen(partial, "wb"))) return 0;
    if ((in = fopen(path, "rb")) != NULL) {
        while (fgets(line, sizeof(line), in))
            if (!skip(line, what)) {
                fputs(line, out);
                if (!strchr(line, '\n')) fputc('\n', out); /* a last line cut short */
            }
        fclose(in);
    }
    if (add) fprintf(out, "%s\n", add);
    ok = !ferror(out);
    if (fclose(out)) ok = 0;
    if (!ok) {
        remove(partial);
        return 0;
    }
    if (rename(partial, path)) {
        remove(path);
        if (rename(partial, path)) {
            remove(partial);
            return 0;
        }
    }
    return 1;
}

static int same_id(const char *line, const void *what)
{
    size_t length = strlen(what);
    return !strncmp(line, what, length) && line[length] == '\t';
}

int ModBudget_Refuse(const char *path, const char *id, long long size, long long time, unsigned build, const char *why)
{
    char line[1024];
    snprintf(line, sizeof(line), "%s\t%lld\t%lld\t%u\t%s", id, size, time, build, why);
    line[strcspn(line, "\r\n")] = 0;
    return rewrite(path, same_id, id, line);
}

int ModBudget_Forget(const char *path, const char *id)
{
    return rewrite(path, same_id, id, NULL);
}

static int restarted_line(const char *line, const void *what)
{
    (void)what;
    return !strncmp(line, "restarted", 9) && (line[9] == '\r' || line[9] == '\n' || !line[9]);
}

int ModBudget_Restarted(const char *path)
{
    FILE *file = fopen(path, "rb");
    char line[1024];
    int found = 0;
    if (!file) return 0;
    while (!found && fgets(line, sizeof(line), file)) found = restarted_line(line, NULL);
    fclose(file);
    return found;
}

int ModBudget_SetRestarted(const char *path, int restarted)
{
    if (restarted == ModBudget_Restarted(path)) return 1;
    return rewrite(path, restarted_line, NULL, restarted ? "restarted" : NULL);
}
