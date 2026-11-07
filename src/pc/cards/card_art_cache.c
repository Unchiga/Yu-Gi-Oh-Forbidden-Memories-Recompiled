/* card_art_cache.h. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* st_mtimespec */
#endif
#include "pc/compat/fs.h"
#include "card_art_cache.h"
#include "art.h"
#include "pc/compat/posix.h"
#include "pc/mods/mods.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(MEMORIES_TEST_HOOKS) && defined(MEMORIES_TRANSLATED)
void GuestRuntime_ForbidHeapOnThisThread(int forbid); /* translated_alloc.c */
#endif

#define SLOTS ((12u << 20) / CARD_ART_RECORD) /* what More Cards' 958 cards take */
/* The parts, the record, then its length and a hash of all of it. */
#define FILE_TRAILER 12
#define FILE_SIZE (1 + CARD_ART_RECORD + FILE_TRAILER)
#define FILE_ON_DISK ((FILE_SIZE + 4095) / 4096 * 4096) /* what a file takes on 4 KB blocks */
#define PATH_ROOM 1400
#define DEFAULT_MB 512
#define MOST_MB 1048576ull
#define WRITES 8 /* records the game's thread made, on their way to the disk */
#define AHEAD 80 /* a duel's two decks */

enum { TO_DO, TAKEN_BY_WORKER, TAKEN_BY_GAME, FINISHED };

typedef struct {
    uint64_t key;
    char *art, *thumbnail;
    const char *mod;
    int index;
    unsigned char no_disk;         /* a PNG's stat failed, so the key says nothing of its file */
    int slot;                      /* the game's thread: */
    unsigned char parts, failed, seen, thumbnail_bad;
    unsigned char work, suspect;   /* under `lock`; suspect: its file did not check out */
    unsigned char on_disk;         /* the worker's */
} Entry;

/* A file in the folder, to know how many there are and which goes next. */
typedef struct {
    uint64_t key;
    int64_t time;
    int handle; /* 0: no card of this run names it */
} Stored;

typedef struct {
    int handle, parts;
    unsigned char *record;
} Write;

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
static uint64_t most_files;
static int (*interrupted)(void);
static int started;

/* The game's thread and the worker, never while a record is made or a file is touched:
 * neither makes the other wait for that, but for one case. The game's thread wants the
 * record the worker is making: it waits for it, at most one record's time, and gets
 * the record from it. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, finished = PTHREAD_COND_INITIALIZER;
static int worker_running;
static Write writes[WRITES];
static int write_head, write_count;
static int ahead[AHEAD], ahead_count;
static int waiting_for, waiting_got;
static unsigned char *waiting_into;
static int writing, writing_parts; /* a handle: the record the worker has made and is writing */
static const unsigned char *writing_record;
static int last_seen; /* a handle; atomic */
/* The worker's. */
static Stored *stored;
static size_t stored_first, stored_count, stored_room;
static int cursor, cursor_seen, filling = 1;

static uint64_t mix(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size--) hash = (hash ^ *bytes++) * 1099511628211ull;
    return hash;
}

/* Its size and its times to the nanosecond, as far as the system tells them. The time
 * of the last change, not only of the last write: a tool that keeps a file's time
 * (an unzip, a copy) keeps the second and cannot keep the first. 0 when there is no
 * such file. */
static int file_stamp(const char *path, int64_t stamp[5])
{
#ifdef _WIN32
    long long size, written, changed;
    if (Memories_FileTimes(path, &size, &written, &changed)) return 0;
    stamp[0] = size;
    stamp[1] = written;
    stamp[2] = 0;
    stamp[3] = changed;
    stamp[4] = 0;
#else
    struct stat info;
    if (stat(path, &info)) return 0;
    stamp[0] = (int64_t)info.st_size;
 #ifdef __APPLE__ /* no st_mtim: a developer's macOS test build */
    stamp[1] = (int64_t)info.st_mtimespec.tv_sec;
    stamp[2] = (int64_t)info.st_mtimespec.tv_nsec;
    stamp[3] = (int64_t)info.st_ctimespec.tv_sec;
    stamp[4] = (int64_t)info.st_ctimespec.tv_nsec;
 #else
    stamp[1] = (int64_t)info.st_mtim.tv_sec;
    stamp[2] = (int64_t)info.st_mtim.tv_nsec;
    stamp[3] = (int64_t)info.st_ctim.tv_sec;
    stamp[4] = (int64_t)info.st_ctim.tv_nsec;
 #endif
#endif
    return 1;
}

static uint64_t mix_file(uint64_t hash, const char *path, unsigned char *no_stamp)
{
    int64_t stamp[5] = {0, 0, 0, 0, 0};
    hash = mix(hash, path ? path : "", path ? strlen(path) + 1 : 1);
    if (path && !file_stamp(path, stamp)) *no_stamp = 1;
    return mix(hash, stamp, sizeof(stamp));
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
    unsigned char no_disk = 0;
    uint64_t key = mix_file(mix_file(14695981039346656037ull, art, &no_disk), thumbnail, &no_disk);
    Entry *entry;
    int *place;
    if (started) return 0; /* the worker reads the list */
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
    entry->no_disk = no_disk;
    entry->slot = -1;
    *place = ++count;
    return count;
}

static int name_of(uint64_t key, char *path, size_t size)
{
    return snprintf(path, size, "%s/%016llx-%08x", directory, (unsigned long long)key, build_id) < (int)size;
}

static int wanted_parts(const Entry *entry)
{
    return entry->art ? ART_CACHE_PICTURE | ART_CACHE_THUMBNAIL : ART_CACHE_THUMBNAIL;
}

/* Of a whole file: its key says whose it is, so a file copied over another fails too. */
static uint64_t file_hash(const Entry *entry, unsigned char parts, const unsigned char *record)
{
    uint64_t hash = mix(14695981039346656037ull, &entry->key, sizeof(entry->key));
    hash = mix(hash, &build_id, sizeof(build_id));
    hash = mix(hash, &parts, 1);
    return mix(hash, record, CARD_ART_RECORD);
}

/* 1 with the record and *parts. 0 if it cannot be used; a file that is there but does
 * not check out says so in *bad, and the next write replaces it. */
static int read_file(const Entry *entry, unsigned char *record, int *parts, int *bad)
{
    char path[PATH_ROOM];
    unsigned char first, tail[FILE_TRAILER];
    uint32_t length;
    uint64_t hash;
    FILE *file;
    int ok;
    *bad = 0;
    if (entry->no_disk || !directory[0] || !name_of(entry->key, path, sizeof(path)) || !(file = fopen(path, "rb"))) return 0;
    ok = fread(&first, 1, 1, file) == 1 && fread(record, 1, CARD_ART_RECORD, file) == CARD_ART_RECORD &&
         fread(tail, 1, FILE_TRAILER, file) == FILE_TRAILER && fgetc(file) == EOF;
    fclose(file);
    if (ok) {
        memcpy(&length, tail, sizeof(length));
        memcpy(&hash, tail + sizeof(length), sizeof(hash));
        ok = length == FILE_SIZE && hash == file_hash(entry, first, record) && first == wanted_parts(entry);
    }
    if (ok) *parts = first;
    else *bad = 1;
    return ok;
}

/* On the disk before it takes the name, so a power cut leaves the old file or none and
 * never a good name on half a picture (src/pc/saves/save_slots.c). The worker's alone;
 * the process id is for a second game running from the same user folder. 0 if it failed. */
static int write_file(const Entry *entry, const unsigned char *record, int parts)
{
    char path[PATH_ROOM], partial[PATH_ROOM + 32];
    unsigned char first = (unsigned char)parts, tail[FILE_TRAILER];
    uint32_t length = FILE_SIZE;
    uint64_t hash = file_hash(entry, first, record);
    FILE *file;
    int ok;
    if (!directory[0] || !name_of(entry->key, path, sizeof(path)) ||
        snprintf(partial, sizeof(partial), "%s.%ld.tmp", path, (long)getpid()) >= (int)sizeof(partial) ||
        !(file = fopen(partial, "wb")))
        return 0;
    memcpy(tail, &length, sizeof(length));
    memcpy(tail + sizeof(length), &hash, sizeof(hash));
    ok = fwrite(&first, 1, 1, file) == 1 && fwrite(record, 1, CARD_ART_RECORD, file) == CARD_ART_RECORD &&
         fwrite(tail, 1, FILE_TRAILER, file) == FILE_TRAILER;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok || rename(partial, path) != 0) {
        remove(partial);
        return 0;
    }
    return 1;
}

/* As add_entry did: the picture brings a thumbnail, and "thumbnail" takes its place.
 * *failed: the PNGs that did not decode (ART_CACHE_*). Only with none may a record go on
 * disk: a bad one is noted when a record is made, so its record must be made again next
 * run to say so. `note` from the game's thread only. No lock: art.c's picture paths hold
 * no state of their own. */
static int make(const Entry *entry, unsigned char *record, int note, int *failed)
{
    char why[1300];
    int parts = 0;
    *failed = 0;
    memset(record, 0, CARD_ART_RECORD);
    if (entry->art) {
        if (CardArt_FromImage(entry->art, record, why, sizeof(why))) {
            parts |= ART_CACHE_PICTURE | ART_CACHE_THUMBNAIL;
        } else {
            *failed |= ART_CACHE_PICTURE;
            if (note) Mods_Note(entry->mod, "cards[%d]: \"art\": %s", entry->index, why);
        }
    }
    if (entry->thumbnail) {
        if (CardArt_ThumbnailFromImage(entry->thumbnail, record, why, sizeof(why))) {
            parts |= ART_CACHE_THUMBNAIL;
        } else {
            *failed |= ART_CACHE_THUMBNAIL;
            if (note) Mods_Note(entry->mod, "cards[%d]: \"thumbnail\": %s", entry->index, why);
        }
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

/* What the game's thread does for a record neither memory nor the disk has: 1 with it
 * from the worker, if that was making it, else 0 and the game's thread makes it. */
static int from_worker(Entry *entry, int handle, unsigned char *record)
{
    int got = 0;
    pthread_mutex_lock(&lock);
    if (writing == handle) {
        memcpy(record, writing_record, CARD_ART_RECORD);
        got = writing_parts;
    } else if (entry->work == TAKEN_BY_WORKER) {
        waiting_for = handle;
        waiting_into = record;
        waiting_got = 0;
        while (waiting_for) pthread_cond_wait(&finished, &lock);
        got = waiting_got;
    }
    if (!got) entry->work = TAKEN_BY_GAME;
    pthread_mutex_unlock(&lock);
    return got;
}

/* A record the game's thread made goes to the worker to write, and when there is no room
 * for it the worker makes it again when it comes to it. */
static void hand_over(Entry *entry, int handle, const unsigned char *record, int parts, int whole)
{
    pthread_mutex_lock(&lock);
    entry->work = FINISHED;
    if (worker_running && whole && parts == wanted_parts(entry) && !entry->no_disk) {
        if (write_count < WRITES) {
            Write *write = &writes[(write_head + write_count++) % WRITES];
            write->handle = handle;
            write->parts = parts;
            memcpy(write->record, record, CARD_ART_RECORD);
            pthread_cond_signal(&wake);
        } else {
            entry->work = TO_DO;
        }
    }
    pthread_mutex_unlock(&lock);
}

const unsigned char *ArtCache_Record(int handle, int *parts)
{
    Entry *entry;
    int slot, got = 0, bad = 0, failed = 0;
    if (handle < 1 || handle > count) return NULL;
    entry = &entries[handle - 1];
    if (entry->failed) return NULL;
    if (entry->slot >= 0) {
        slot_recent[entry->slot] = 1;
        *parts = entry->parts;
        return slot_record[entry->slot];
    }
    if (interrupted && interrupted()) {
        *parts = -1;
        return NULL;
    }
    if ((slot = take_slot()) < 0) return NULL;
    __atomic_store_n(&last_seen, handle, __ATOMIC_RELAXED);
    if (!read_file(entry, slot_record[slot], &got, &bad)) {
        if (bad) {
            pthread_mutex_lock(&lock);
            entry->suspect = 1;
            pthread_mutex_unlock(&lock);
        }
        got = from_worker(entry, handle, slot_record[slot]);
        if (!got) {
            got = make(entry, slot_record[slot], 1, &failed);
            entry->thumbnail_bad = (failed & ART_CACHE_THUMBNAIL) != 0;
            hand_over(entry, handle, slot_record[slot], got, !failed);
        }
    }
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
    if (!entry || entry->seen || (interrupted && interrupted())) return 0;
    entry->seen = 1;
    return 1;
}

void ArtCache_Paths(int handle, const char **art, const char **thumbnail)
{
    *art = entries[handle - 1].art;
    *thumbnail = entries[handle - 1].thumbnail_bad ? NULL : entries[handle - 1].thumbnail;
}

void ArtCache_Prefer(const int *handles, int wanted)
{
    if (interrupted && interrupted()) return;
    pthread_mutex_lock(&lock);
    ahead_count = 0;
    for (int i = 0; i < wanted && ahead_count < AHEAD; i++)
        if (handles[i] >= 1 && handles[i] <= count) ahead[ahead_count++] = handles[i];
    if (ahead_count) pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

/* --- the worker ---------------------------------------------------------- */

static void store(uint64_t key, int64_t when, int handle)
{
    if (stored_count == stored_room) {
        size_t wanted = stored_room ? stored_room * 2 : 1024;
        Stored *bigger = realloc(stored, wanted * sizeof(*bigger));
        if (!bigger) return; /* one file not counted */
        stored = bigger;
        stored_room = wanted;
    }
    stored[stored_count++] = (Stored){key, when, handle};
}

/* Unnamed files first, oldest first. */
static int compare_stored(const void *a, const void *b)
{
    const Stored *x = a, *y = b;
    if ((x->handle != 0) != (y->handle != 0)) return x->handle ? 1 : -1;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return x->key < y->key ? -1 : x->key > y->key;
}

static void remove_oldest(void)
{
    const Stored *oldest = &stored[stored_first++];
    char path[PATH_ROOM];
    if (name_of(oldest->key, path, sizeof(path))) remove(path);
    if (oldest->handle) entries[oldest->handle - 1].on_disk = 0;
}

/* What is in the folder, at once and not when the list is done, so a short session does
 * not keep an earlier build's files: those go, as does what a killed run left half
 * written or anything else; a file of the right name and size stays (reading it would
 * be a read of everything at every start, and a bad one fails when it is read), the
 * ones no card of this run names go first when there is no room. 0: no folder. */
static int scan(void)
{
    DIR *folder = opendir(directory);
    struct dirent *item;
    if (!folder) return 0;
    while ((item = readdir(folder)) != NULL) {
        char path[PATH_ROOM];
        unsigned long long key = 0;
        unsigned build = 0;
        int used = 0, handle = 0;
        struct stat info;
        if (item->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) >= (int)sizeof(path)) continue;
        if (sscanf(item->d_name, "%16llx-%8x%n", &key, &build, &used) != 2 || item->d_name[used] || build != build_id ||
            stat(path, &info) || info.st_size != FILE_SIZE) {
            remove(path);
            continue;
        }
        if (table) handle = *table_place(key);
        store(key, (int64_t)info.st_mtime, handle);
        if (handle) entries[handle - 1].on_disk = 1;
    }
    closedir(folder);
    if (stored_count) qsort(stored, stored_count, sizeof(*stored), compare_stored);
    while (stored_count - stored_first > most_files) remove_oldest();
    return 1;
}

/* Room for one file more, by the place of the oldest: read-ahead only by the place of
 * one no card of this run names, or it would trade a card's file for another's at every
 * start. */
static int make_room(int asked)
{
    while (stored_count - stored_first >= most_files) {
        if (stored_first == stored_count || (stored[stored_first].handle && !asked)) return 0;
        remove_oldest();
    }
    return 1;
}

/* Whether read-ahead has anywhere to put another: asked before a record is made. */
static int room_to_fill(void)
{
    return stored_count - stored_first < most_files || (stored_first < stored_count && !stored[stored_first].handle);
}

/* 1 kept, 0 no room for it, -1 the write failed. */
static int keep(int handle, const unsigned char *record, int parts, int asked)
{
    Entry *entry = &entries[handle - 1];
    if (!entry->on_disk && !make_room(asked)) return 0;
    if (!write_file(entry, record, parts)) return -1;
    if (!entry->on_disk) store(entry->key, (int64_t)time(NULL), handle);
    entry->on_disk = 1;
    pthread_mutex_lock(&lock);
    entry->suspect = 0;
    pthread_mutex_unlock(&lock);
    return 1;
}

static int worth_making(const Entry *entry)
{
    return entry->work == TO_DO && !entry->no_disk && (!entry->on_disk || entry->suspect);
}

/* The handle to make next, 0 none; with `lock` held. The decks first, then on from the
 * last card seen, round and round. */
static int next_work(int *asked)
{
    int seen, i;
    *asked = 1;
    while (ahead_count) {
        int handle = ahead[--ahead_count];
        if (worth_making(&entries[handle - 1])) {
            entries[handle - 1].work = TAKEN_BY_WORKER;
            return handle;
        }
    }
    *asked = 0;
    if (filling && !room_to_fill()) filling = 0; /* full of this run's cards */
    if (!filling) return 0;
    seen = __atomic_load_n(&last_seen, __ATOMIC_RELAXED);
    if (seen != cursor_seen) {
        cursor_seen = seen;
        cursor = seen % count;
    }
    for (i = 0; i < count; i++) {
        int at = (cursor + i) % count;
        if (worth_making(&entries[at])) {
            entries[at].work = TAKEN_BY_WORKER;
            cursor = (at + 1) % count;
            return at + 1;
        }
    }
    return 0;
}

/* Made: to whoever waits for it, if it can be used, and to whoever asks while the worker
 * writes it, the slowest part of its work. */
static void made_one(Entry *entry, int handle, const unsigned char *record, int parts, int usable)
{
    pthread_mutex_lock(&lock);
    entry->work = FINISHED;
    if (usable) {
        writing = handle;
        writing_parts = parts;
        writing_record = record;
    }
    if (waiting_for == handle) {
        if (usable) {
            memcpy(waiting_into, record, CARD_ART_RECORD);
            waiting_got = parts;
        }
        waiting_for = 0;
        pthread_cond_broadcast(&finished);
    }
    pthread_mutex_unlock(&lock);
}

static void *work(void *unused)
{
    unsigned char *record = malloc(CARD_ART_RECORD);
    struct timespec gap = {0, 2000000};
    int going;
    (void)unused;
#if defined(MEMORIES_TEST_HOOKS) && defined(MEMORIES_TRANSLATED)
    GuestRuntime_ForbidHeapOnThisThread(1);
#endif
    going = scan() && count && record && most_files;
    while (going) {
        int handle, parts = 0, asked = 0, failed = 0, whole, written = 0;
        Entry *entry;
        pthread_mutex_lock(&lock);
        for (;;) {
            if (write_count) {
                const Write *write = &writes[write_head];
                handle = write->handle;
                parts = write->parts;
                memcpy(record, write->record, CARD_ART_RECORD);
                write_head = (write_head + 1) % WRITES;
                write_count--;
                written = 1;
                break;
            }
            if ((handle = next_work(&asked)) != 0) break;
            pthread_cond_wait(&wake, &lock);
        }
        pthread_mutex_unlock(&lock);
        entry = &entries[handle - 1];
        if (written) {
            /* A card the game read. */
            going = keep(handle, record, parts, 1) >= 0;
            continue;
        }
        parts = make(entry, record, 0, &failed);
        whole = !failed && parts == wanted_parts(entry);
        made_one(entry, handle, record, parts, whole);
        if (whole) {
            int kept = keep(handle, record, parts, asked);
            pthread_mutex_lock(&lock);
            writing = 0;
            pthread_mutex_unlock(&lock);
            going = kept >= 0;
        }
        nanosleep(&gap, NULL);
    }
    free(record);
    pthread_mutex_lock(&lock);
    worker_running = 0;
    write_count = ahead_count = 0;
    pthread_mutex_unlock(&lock);
#if defined(MEMORIES_TEST_HOOKS) && defined(MEMORIES_TRANSLATED)
    GuestRuntime_ForbidHeapOnThisThread(0);
#endif
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

/* How many files the folder may hold: MEMORIES_ART_CACHE_MB, a whole number. */
static uint64_t files_allowed(void)
{
    const char *text = getenv("MEMORIES_ART_CACHE_MB");
    unsigned long long megabytes = DEFAULT_MB;
    if (text && *text) {
        char *end;
        unsigned long long value;
        errno = 0;
        value = strtoull(text, &end, 10);
        if (*text >= '0' && *text <= '9' && !*end && !errno && value <= MOST_MB) megabytes = value;
        else fprintf(stderr, "memories-pc: MEMORIES_ART_CACHE_MB=%s is not a size: %d\n", text, DEFAULT_MB);
    }
    return (uint64_t)megabytes * 1048576u / FILE_ON_DISK;
}

void ArtCache_Start(const char *folder, unsigned build, int (*in_interrupt)(void))
{
    pthread_t thread;
    struct stat info;
    int running;
#ifndef _WIN32
    sigset_t all, previous;
#endif
    interrupted = in_interrupt;
    started = 1;
    if (!folder[0] || snprintf(directory, sizeof(directory), "%s", folder) >= (int)sizeof(directory)) {
        directory[0] = 0;
        return;
    }
    build_id = build;
    most_files = files_allowed();
    if (count) make_folders(directory);
    else if (stat(directory, &info)) return; /* no card, and nothing left to clear */
    for (int i = 0; i < WRITES; i++) {
        if (!(writes[i].record = malloc(CARD_ART_RECORD))) {
            directory[0] = 0;
            return;
        }
    }
    worker_running = 1;
#ifndef _WIN32
    /* Blocked from its first instruction: the game's clock signal may come to the
     * whole process (platform_common.c's setitimer). */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
#endif
    running = !pthread_create(&thread, NULL, work, NULL);
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
#endif
    if (running) pthread_detach(thread);
    else worker_running = 0;
}
