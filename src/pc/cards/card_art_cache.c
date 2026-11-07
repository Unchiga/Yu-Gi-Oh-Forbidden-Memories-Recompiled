/* card_art_cache.h. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "card_art_cache.h"
#include "art.h"
#include "pc/mods/mods.h"
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SLOTS ((12u << 20) / CARD_ART_RECORD) /* what More Cards' 958 cards take */
#define FILE_SIZE (CARD_ART_RECORD + 1)       /* the parts, then the record */
#define PATH_ROOM 1400

typedef struct {
    uint64_t key;
    char *art, *thumbnail;
    const char *mod;
    int index, slot;
    unsigned char parts, failed, seen, done;
} Entry;

static Entry *entries;
static int count, room;
static int *table; /* the handles by key, open addressed, a power of two long */
static unsigned table_size;
static unsigned char *slot_record[SLOTS];
static int slot_owner[SLOTS]; /* a handle */
static unsigned char slot_recent[SLOTS];
static unsigned hand;
static char directory[1024];
static unsigned build_id;
static pthread_mutex_t making = PTHREAD_MUTEX_INITIALIZER; /* art.c, and one maker at a time */
static int next_ahead;

static uint64_t mix(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size--) hash = (hash ^ *bytes++) * 1099511628211ull;
    return hash;
}

static uint64_t mix_file(uint64_t hash, const char *path)
{
    struct stat info;
    int64_t size = -1, time = -1;
    hash = mix(hash, path ? path : "", path ? strlen(path) + 1 : 1);
    if (path && !stat(path, &info)) {
        size = (int64_t)info.st_size;
        time = (int64_t)info.st_mtime;
    }
    hash = mix(hash, &size, sizeof(size));
    return mix(hash, &time, sizeof(time));
}

static char *copied(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static int *table_place(uint64_t key)
{
    unsigned at = (unsigned)key & (table_size - 1);
    while (table[at] && entries[table[at] - 1].key != key) at = (at + 1) & (table_size - 1);
    return &table[at];
}

static int grow_table(void)
{
    unsigned size = table_size ? table_size * 2 : 4096;
    int *old = table;
    table = calloc(size, sizeof(*table));
    if (!table) {
        table = old;
        return 0;
    }
    table_size = size;
    for (int i = 0; i < count; i++) *table_place(entries[i].key) = i + 1;
    free(old);
    return 1;
}

int ArtCache_Add(const char *mod, int index, const char *art, const char *thumbnail)
{
    uint64_t key = mix_file(mix_file(14695981039346656037ull, art), thumbnail);
    Entry *entry;
    int *place;
    if ((unsigned)(count + 1) * 2 > table_size && !grow_table()) return 0;
    place = table_place(key);
    if (*place) return *place;
    if (count == room) {
        int wanted = room ? room * 2 : 256;
        Entry *bigger = realloc(entries, (size_t)wanted * sizeof(*bigger));
        if (!bigger) return 0;
        entries = bigger;
        room = wanted;
    }
    entry = &entries[count];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->art = art ? copied(art) : NULL;
    entry->thumbnail = thumbnail ? copied(thumbnail) : NULL;
    if ((art && !entry->art) || (thumbnail && !entry->thumbnail)) {
        free(entry->art);
        free(entry->thumbnail);
        return 0;
    }
    entry->mod = mod;
    entry->index = index;
    entry->slot = -1;
    *place = ++count;
    return count;
}

static int file_name(const Entry *entry, char *path, size_t size)
{
    return snprintf(path, size, "%s/%016llx-%08x", directory, (unsigned long long)entry->key, build_id) < (int)size;
}

static int wanted_parts(const Entry *entry)
{
    return entry->art ? ART_CACHE_PICTURE | ART_CACHE_THUMBNAIL : ART_CACHE_THUMBNAIL;
}

static int read_file(const Entry *entry, unsigned char *record, int *parts)
{
    char path[PATH_ROOM];
    unsigned char first;
    FILE *file;
    int ok;
    if (!directory[0] || !file_name(entry, path, sizeof(path)) || !(file = fopen(path, "rb"))) return 0;
    ok = fread(&first, 1, 1, file) == 1 && fread(record, 1, CARD_ART_RECORD, file) == CARD_ART_RECORD &&
         first == wanted_parts(entry);
    fclose(file);
    if (ok) *parts = first;
    return ok;
}

static void write_file(const Entry *entry, const unsigned char *record, int parts)
{
    char path[PATH_ROOM], partial[PATH_ROOM + 8];
    unsigned char first = (unsigned char)parts;
    FILE *file;
    int ok;
    if (!directory[0] || !file_name(entry, path, sizeof(path)) ||
        snprintf(partial, sizeof(partial), "%s.tmp", path) >= (int)sizeof(partial) || !(file = fopen(partial, "wb")))
        return;
    ok = fwrite(&first, 1, 1, file) == 1 && fwrite(record, 1, CARD_ART_RECORD, file) == CARD_ART_RECORD;
    if (fclose(file) || !ok || rename(partial, path)) remove(partial);
}

/* As add_entry did: the picture brings a thumbnail, and "thumbnail" takes its place.
 * `note` from the game's thread only. */
static int make(const Entry *entry, unsigned char *record, int note)
{
    char why[1300];
    int parts = 0;
    memset(record, 0, CARD_ART_RECORD);
    if (entry->art) {
        if (CardArt_FromImage(entry->art, record, why, sizeof(why))) parts |= ART_CACHE_PICTURE | ART_CACHE_THUMBNAIL;
        else if (note) Mods_Note(entry->mod, "cards[%d]: \"art\": %s", entry->index, why);
    }
    if (entry->thumbnail) {
        if (CardArt_ThumbnailFromImage(entry->thumbnail, record, why, sizeof(why))) parts |= ART_CACHE_THUMBNAIL;
        else if (note) Mods_Note(entry->mod, "cards[%d]: \"thumbnail\": %s", entry->index, why);
    }
    return parts;
}

static int take_slot(void)
{
    for (;;) {
        unsigned at = hand;
        hand = (hand + 1) % SLOTS;
        if (slot_recent[at]) {
            slot_recent[at] = 0;
            continue;
        }
        if (slot_owner[at]) entries[slot_owner[at] - 1].slot = -1;
        slot_owner[at] = 0;
        if (!slot_record[at]) slot_record[at] = malloc(CARD_ART_RECORD);
        return slot_record[at] ? (int)at : -1;
    }
}

const unsigned char *ArtCache_Record(int handle, int *parts)
{
    Entry *entry;
    int slot, got = 0;
    if (handle < 1 || handle > count) return NULL;
    entry = &entries[handle - 1];
    if (entry->failed) return NULL;
    if (entry->slot >= 0) {
        slot_recent[entry->slot] = 1;
        *parts = entry->parts;
        return slot_record[entry->slot];
    }
    if ((slot = take_slot()) < 0) return NULL;
    __atomic_store_n(&next_ahead, handle, __ATOMIC_RELAXED);
    pthread_mutex_lock(&making);
    if (!read_file(entry, slot_record[slot], &got)) {
        got = make(entry, slot_record[slot], 1);
        if (got == wanted_parts(entry)) write_file(entry, slot_record[slot], got);
    }
    pthread_mutex_unlock(&making);
    if (!got) {
        entry->failed = 1;
        return NULL;
    }
    entry->parts = (unsigned char)got;
    entry->slot = slot;
    slot_owner[slot] = handle;
    slot_recent[slot] = 1;
    *parts = got;
    return slot_record[slot];
}

int ArtCache_FirstSight(int handle)
{
    Entry *entry = handle >= 1 && handle <= count ? &entries[handle - 1] : NULL;
    if (!entry || entry->seen) return 0;
    entry->seen = 1;
    return 1;
}

void ArtCache_Paths(int handle, const char **art, const char **thumbnail)
{
    *art = entries[handle - 1].art;
    *thumbnail = entries[handle - 1].thumbnail;
}

/* A file no card of this run names, or an earlier build's. */
static void remove_stale(void)
{
    DIR *folder = opendir(directory);
    struct dirent *item;
    if (!folder) return;
    while ((item = readdir(folder)) != NULL) {
        unsigned long long key = 0;
        unsigned build = 0;
        int used = 0, *place;
        char path[PATH_ROOM];
        if (item->d_name[0] == '.') continue;
        if (sscanf(item->d_name, "%16llx-%8x%n", &key, &build, &used) == 2 && !item->d_name[used] && build == build_id) {
            place = table_place(key);
            if (*place) continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) < (int)sizeof(path)) remove(path);
    }
    closedir(folder);
}

static void *read_ahead(void *unused)
{
    unsigned char *record = malloc(CARD_ART_RECORD);
    struct timespec gap = {0, 2000000};
    int left = count;
    (void)unused;
    while (record && left) {
        int handle = __atomic_load_n(&next_ahead, __ATOMIC_RELAXED), parts;
        char path[PATH_ROOM];
        struct stat info;
        Entry *entry = NULL;
        /* From the last card seen, round and round. */
        for (int i = 0; i < count && !entry; i++) {
            Entry *candidate = &entries[(handle + i) % count];
            if (!candidate->done) entry = candidate;
        }
        if (!entry) break;
        entry->done = 1;
        left--;
        if (!file_name(entry, path, sizeof(path)) || !stat(path, &info)) continue;
        pthread_mutex_lock(&making);
        parts = make(entry, record, 0);
        pthread_mutex_unlock(&making);
        if (parts == wanted_parts(entry)) write_file(entry, record, parts);
        nanosleep(&gap, NULL);
    }
    free(record);
    if (!left) remove_stale();
    return NULL;
}

static void make_folders(char *path)
{
    for (char *at = path + 1; *at; at++) {
        if (*at != '/') continue;
        *at = 0;
        mkdir(path, 0755);
        *at = '/';
    }
    mkdir(path, 0755);
}

void ArtCache_Start(const char *folder, unsigned build)
{
    pthread_t thread;
    int started;
#ifndef _WIN32
    sigset_t all, previous;
#endif
    if (snprintf(directory, sizeof(directory), "%s", folder) >= (int)sizeof(directory)) {
        directory[0] = 0;
        return;
    }
    build_id = build;
    make_folders(directory);
    if (!count) return;
#ifndef _WIN32
    /* Blocked from its first instruction: the game's clock signal may come to the
     * whole process (platform_common.c's setitimer). */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
#endif
    started = !pthread_create(&thread, NULL, read_ahead, NULL);
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
#endif
    if (started) pthread_detach(thread);
}
