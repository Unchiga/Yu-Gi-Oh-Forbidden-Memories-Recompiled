#include "pc/compat/fs.h"
#include "profile.h"
#include "symbols.h"
#include "pc/compat/signal.h"
#ifdef _WIN32
#include "pc/platform/win32.h"
#endif
#ifdef __APPLE__
#include "pc/platform/darwin_image.h"
#endif
#include <stdio.h>
#include <stdlib.h>

typedef struct ProfileRange {
    uintptr_t first, last;
    uint32_t *samples;
    size_t count;
} ProfileRange;

typedef struct ProfileEntry {
    uintptr_t address;
    uint32_t count;
} ProfileEntry;

static ProfileRange ranges[2];
static const char *output_path;

static int compare_entries(const void *left, const void *right)
{
    const ProfileEntry *a = left, *b = right;
    return a->count < b->count ? 1 : a->count > b->count ? -1 :
           a->address < b->address ? -1 : a->address > b->address;
}

static void write_profile(void)
{
    ProfileEntry *entries;
    size_t used = 0, room = ranges[0].count + ranges[1].count, r, i;
    FILE *file;
    sigset_t set, previous;
    if (!output_path || !(entries = malloc(room * sizeof(*entries)))) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    for (r = 0; r < 2; r++) {
        for (i = 0; i < ranges[r].count; i++) {
            if (ranges[r].samples[i]) {
                entries[used].address = ranges[r].first + i * 16;
                entries[used++].count = ranges[r].samples[i];
            }
        }
    }
    qsort(entries, used, sizeof(*entries), compare_entries);
    file = fopen(output_path, "w");
    if (file) {
        for (i = 0; i < used; i++) {
            uintptr_t offset = 0;
            const char *name = Symbols_Lookup(entries[i].address, &offset);
            fprintf(file, "%08lx %u %s+0x%lx\n", (unsigned long)entries[i].address, entries[i].count,
                    name ? name : "?", (unsigned long)offset);
        }
        fclose(file);
    }
    free(entries);
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Profile_Init(void)
{
 #if !defined(__APPLE__) && !defined(MEMORIES_TRANSLATED)
    extern char __start_game_text[], __stop_game_text[];
#endif
 #ifndef __APPLE__
#ifndef _WIN32
    extern char __executable_start[], etext[];
#endif
 #endif
    output_path = getenv("MEMORIES_PROFILE");
    if (!output_path || !*output_path) return;
 #ifdef __APPLE__
    /* Translated game and SDK code currently share Mach-O __text. Sample
     * that real range once; no separate game section exists in this build. */
    if (!DarwinImage_TextRange(&ranges[0].first, &ranges[0].last)) {
        fprintf(stderr, "memories-pc: cannot find Mach-O executable text for profiling\n");
        return;
    }
    ranges[1].first = ranges[1].last = 0;
 #elif defined(MEMORIES_TRANSLATED)
    /* ELF translated (Android): game and port code share the object's text,
     * as on macOS; no separate game section. */
    ranges[0].first = (uintptr_t)__executable_start;
    ranges[0].last = (uintptr_t)etext;
    ranges[1].first = ranges[1].last = 0;
 #else
    ranges[0].first = (uintptr_t)__start_game_text;
    ranges[0].last = (uintptr_t)__stop_game_text;
#ifdef _WIN32
    Win32_ImageRange(&ranges[1].first, &ranges[1].last);
#else
    ranges[1].first = (uintptr_t)__executable_start;
    ranges[1].last = (uintptr_t)etext;
#endif
 #endif
    ranges[0].count = (ranges[0].last - ranges[0].first + 15) / 16;
    ranges[1].count = (ranges[1].last - ranges[1].first + 15) / 16;
    ranges[0].samples = calloc(ranges[0].count, sizeof(uint32_t));
    ranges[1].samples = ranges[1].count ? calloc(ranges[1].count, sizeof(uint32_t)) : NULL;
    if (!ranges[0].samples || (ranges[1].count && !ranges[1].samples)) {
        free(ranges[0].samples);
        free(ranges[1].samples);
        ranges[0].samples = ranges[1].samples = NULL;
        return;
    }
    atexit(write_profile);
}

void Profile_Sample(uintptr_t address)
{
    int r;
    for (r = 0; r < 2; r++) {
        if (ranges[r].samples && address >= ranges[r].first && address < ranges[r].last) {
            ranges[r].samples[(address - ranges[r].first) / 16]++;
            return;
        }
    }
}

void Profile_Flush(void) { write_profile(); }
