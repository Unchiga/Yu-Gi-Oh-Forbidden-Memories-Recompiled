/* The art records made on demand (src/pc/cards/card_art_cache.c), with the making
 * replaced: what is asked for, how often, what the disk gives back and what it does
 * not, what a bad PNG leaves, how much is kept, and the thread. Each run of the game
 * here is a process of its own, as it is out there: the cache's state starts empty and
 * the folder stays. */
#define _POSIX_C_SOURCE 200809L
#include "pc/cards/card_art_cache.h"
#include "pc/cards/art.h"
/* Tests must execute their checks in Release CI too. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_BYTES (1 + CARD_ART_RECORD + 12)
#define BUILD 0x1234

static int made, started_making, notes, order[64];
static char root[64] = "art-cache-test-XXXXXX", cache[128];

static void wait_ms(int ms)
{
    struct timespec gap = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&gap, NULL);
}

/* A "PNG" is a text file; a record is filled with its first byte. 'x' does not decode,
 * 'S' takes a while. */
static int fill(const char *path, unsigned char *record, size_t at, size_t size)
{
    FILE *file = fopen(path, "rb");
    int c = file ? fgetc(file) : EOF, n;
    if (file) fclose(file);
    if (c == EOF || c == 'x') return 0;
    n = __atomic_fetch_add(&started_making, 1, __ATOMIC_SEQ_CST);
    if (n < 64) {
        const char *name = strrchr(path, '/');
        __atomic_store_n(&order[n], atoi(name + 2), __ATOMIC_SEQ_CST);
    }
    if (c == 'S') wait_ms(200);
    memset(record + at, c, size);
    __atomic_add_fetch(&made, 1, __ATOMIC_SEQ_CST);
    return 1;
}
int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    snprintf(why, why_size, "bad");
    return fill(path, record, 0, CARD_ART_RECORD);
}
int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    snprintf(why, why_size, "bad");
    return fill(path, record, CARD_ART_RECORD - 1000, 1000);
}
void Mods_Note(const char *id, const char *format, ...)
{
    (void)id;
    (void)format;
    notes++;
}

static void put(const char *name, const char *text)
{
    char path[160];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

static int add(int index, const char *art, const char *thumbnail)
{
    char a[160], t[160];
    if (art) snprintf(a, sizeof(a), "%s/%s", root, art);
    if (thumbnail) snprintf(t, sizeof(t), "%s/%s", root, thumbnail);
    return ArtCache_Add("mod", index, art ? a : NULL, thumbnail ? t : NULL);
}

/* n PNGs named p<first>.png on, whose bytes are `c`; their handles in `handles`. */
static void add_many(int first, int n, int c, int *handles)
{
    for (int i = 0; i < n; i++) {
        char name[32], path[160], text[2] = {(char)c, 0};
        snprintf(name, sizeof(name), "p%d.png", first + i);
        snprintf(path, sizeof(path), "%s/%s", root, name);
        if (access(path, F_OK)) put(name, text); /* a run leaves a mod's files as they are */
        handles[i] = add(first + i, name, NULL);
        assert(handles[i]);
    }
}

static int files_in(const char *suffix)
{
    DIR *folder = opendir(cache);
    struct dirent *item;
    int n = 0;
    if (!folder) return 0;
    while ((item = readdir(folder)) != NULL) {
        size_t size = strlen(item->d_name), tail = suffix ? strlen(suffix) : 0;
        if (item->d_name[0] == '.') continue;
        if (suffix ? size >= tail && !strcmp(item->d_name + size - tail, suffix) : !strstr(item->d_name, ".tmp")) n++;
    }
    closedir(folder);
    return n;
}

/* The files settle: `wanted` of them, or as many as there will be. */
static void wait_files(int wanted)
{
    int tries, last = -1, same = 0;
    for (tries = 0; tries < 2000; tries++) {
        int now = files_in(NULL);
        if (wanted >= 0 && now == wanted) return;
        same = now == last ? same + 1 : 0;
        last = now;
        if (wanted < 0 && same > 40) return;
        wait_ms(5);
    }
    fprintf(stderr, "%d files, %d wanted\n", last, wanted);
    assert(0);
}

/* A run of the game, in a process of its own. */
static void run(void (*game)(void))
{
    pid_t child = fork();
    int status = 0;
    assert(child >= 0);
    if (!child) {
        game();
        fflush(NULL);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "a run failed: status 0x%x\n", status);
        exit(1);
    }
}

static void clear(void)
{
    char command[256];
    snprintf(command, sizeof(command), "rm -rf %s && mkdir -p %s", root, root);
    assert(!system(command));
    snprintf(cache, sizeof(cache), "%s/cache/card-art", root);
}

static void path_of_nth(int n, char *out, size_t size)
{
    DIR *folder = opendir(cache);
    struct dirent *item;
    assert(folder);
    while ((item = readdir(folder)) != NULL) {
        if (item->d_name[0] == '.') continue;
        if (!n--) {
            snprintf(out, size, "%s/%s", cache, item->d_name);
            break;
        }
    }
    closedir(folder);
    assert(n < 0);
}

/* --- made when asked ----------------------------------------------------- */

static void made_when_asked(void)
{
    const unsigned char *record;
    const char *art, *thumbnail;
    int parts = 0, one, two, again, bad, half;
    put("a.png", "A");
    one = add(0, "a.png", NULL);
    again = add(1, "a.png", NULL);
    assert(one && one == again); /* one PNG, one record */
    put("b.png", "B");
    two = add(2, "a.png", "b.png");
    assert(two && two != one);
    /* Nothing is made until it is asked for, and once. */
    assert(!made);
    record = ArtCache_Record(one, &parts);
    assert(record && record[0] == 'A' && parts == (ART_CACHE_PICTURE | ART_CACHE_THUMBNAIL) && made == 1);
    record = ArtCache_Record(one, &parts);
    assert(record && made == 1);
    assert(ArtCache_FirstSight(one) && !ArtCache_FirstSight(one));
    /* The thumbnail over the picture's. */
    record = ArtCache_Record(two, &parts);
    assert(record && record[0] == 'A' && record[CARD_ART_RECORD - 1] == 'B' && made == 3);
    ArtCache_Paths(two, &art, &thumbnail);
    assert(art && thumbnail);
    /* A bad picture says so when it is read; its thumbnail still counts. */
    put("bad.png", "x");
    half = add(3, "bad.png", "b.png");
    bad = add(4, "bad.png", NULL);
    record = ArtCache_Record(half, &parts);
    assert(record && parts == ART_CACHE_THUMBNAIL && notes == 1);
    assert(!ArtCache_Record(bad, &parts) && notes == 2);
    assert(!ArtCache_Record(bad, &parts) && notes == 2);
    assert(!ArtCache_Record(0, &parts) && !ArtCache_Record(9999, &parts));
}

static void more_than_memory(void)
{
    static int handles[1200];
    add_many(0, 1200, 'a', handles);
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 1200; i++) {
            int parts = 0;
            const unsigned char *record = ArtCache_Record(handles[i], &parts);
            assert(record && record[0] == 'a');
        }
}

/* --- the disk ------------------------------------------------------------ */

static int fifty[50];

static void fill_fifty(void)
{
    add_many(0, 50, 'A', fifty);
    ArtCache_Start(cache, BUILD, NULL);
    wait_files(50);
    wait_ms(50);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 50);
}

static void from_disk(void)
{
    add_many(0, 50, 'A', fifty);
    ArtCache_Start(cache, BUILD, NULL);
    for (int i = 0; i < 50; i++) {
        int parts = 0;
        const unsigned char *record = ArtCache_Record(fifty[i], &parts);
        assert(record && record[0] == 'A' && record[CARD_ART_RECORD - 1] == 'A');
    }
    wait_ms(50);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 0);
    assert(files_in(NULL) == 50);
}

/* Six files spoiled, in the ways a file goes wrong: asked for they are made again and
 * put right, and no spoiled byte shows. */
static void spoiled_read(void)
{
    add_many(0, 50, 'A', fifty);
    ArtCache_Start(cache, BUILD, NULL);
    for (int i = 0; i < 50; i++) {
        int parts = 0;
        const unsigned char *record = ArtCache_Record(fifty[i], &parts);
        assert(record && parts == 3);
        for (int k = 0; k < CARD_ART_RECORD; k += 251) assert(record[k] == 'A');
    }
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 6);
    /* The worker puts them right: no file is short, and the next run reads them. */
    wait_ms(100);
    assert(files_in(NULL) == 50);
}

static void spoil(void)
{
    char path[320], other[320], command[700];
    struct stat info;
    unsigned char c;
    FILE *file;
    /* Cut short: the power went before the data did. */
    path_of_nth(0, path, sizeof(path));
    assert(!truncate(path, 5000));
    /* Full size, the rest of it never written. */
    path_of_nth(1, path, sizeof(path));
    assert(!truncate(path, 4096) && !truncate(path, FILE_BYTES));
    /* One bit. */
    path_of_nth(2, path, sizeof(path));
    file = fopen(path, "r+b");
    assert(file && !fseek(file, 7000, SEEK_SET) && fread(&c, 1, 1, file) == 1);
    c ^= 1;
    assert(!fseek(file, 7000, SEEK_SET) && fwrite(&c, 1, 1, file) == 1 && !fclose(file));
    /* The parts byte. */
    path_of_nth(3, path, sizeof(path));
    file = fopen(path, "r+b");
    c = 1;
    assert(file && fwrite(&c, 1, 1, file) == 1 && !fclose(file));
    /* Another card's file under this name: a whole file, with a good length. */
    path_of_nth(4, path, sizeof(path));
    path_of_nth(5, other, sizeof(other));
    snprintf(command, sizeof(command), "cp %s %s", other, path);
    assert(!system(command));
    /* One too long. */
    path_of_nth(6, path, sizeof(path));
    assert(!stat(path, &info) && info.st_size == FILE_BYTES);
    file = fopen(path, "ab");
    assert(file && fputc(0, file) == 0 && !fclose(file));
}

/* A file no read has reached and whose size is wrong is made again by the thread. */
static void short_file_replaced(void)
{
    add_many(0, 50, 'A', fifty);
    ArtCache_Start(cache, BUILD, NULL);
    for (int tries = 0; tries < 400 && __atomic_load_n(&made, __ATOMIC_SEQ_CST) < 1; tries++) wait_ms(5);
    wait_ms(50);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 1);
}

/* The file of a card whose PNG changed is not the card's, though the size and the
 * time say so: only the change time tells. */
static void png_a(void)
{
    int parts = 0, handle;
    const unsigned char *record;
    put("c.png", "C");
    handle = add(0, "c.png", NULL);
    ArtCache_Start(cache, BUILD, NULL);
    record = ArtCache_Record(handle, &parts);
    assert(record && record[0] == 'C');
    wait_files(1);
}

static void png_b(void)
{
    int parts = 0, handle = add(0, "c.png", NULL);
    const unsigned char *record;
    ArtCache_Start(cache, BUILD, NULL);
    record = ArtCache_Record(handle, &parts);
    assert(record && record[0] == 'D');
}

static void same_size_same_time(void)
{
    char path[160];
    struct stat before, after;
    struct timespec times[2];
    snprintf(path, sizeof(path), "%s/c.png", root);
    run(png_a);
    assert(!stat(path, &before));
    wait_ms(20);
    put("c.png", "D");
    times[0] = before.st_atim;
    times[1] = before.st_mtim;
    assert(!utimensat(AT_FDCWD, path, times, 0));
    assert(!stat(path, &after));
    assert(after.st_size == before.st_size && after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
           after.st_mtim.tv_nsec == before.st_mtim.tv_nsec);
    run(png_b);
}

/* A PNG that stat cannot read: the key says nothing of it, so nothing goes on disk. */
static void no_stat(void)
{
    int parts = 0, handle;
    const unsigned char *record;
    put("t.png", "T");
    handle = add(0, "gone.png", "t.png");
    ArtCache_Start(cache, BUILD, NULL);
    record = ArtCache_Record(handle, &parts);
    assert(record && parts == ART_CACHE_THUMBNAIL && notes == 1);
    wait_ms(60);
    assert(files_in(NULL) == 0);
}

/* A good picture and a thumbnail that does not decode: never on disk, so the note
 * comes in every run, when the card is read. */
static void bad_thumbnail(void)
{
    int parts = 0, handle;
    const unsigned char *record;
    const char *art, *thumbnail;
    put("g.png", "G");
    put("bad.png", "x");
    handle = add(0, "g.png", "bad.png");
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(60);
    assert(files_in(NULL) == 0);
    assert(!notes);
    record = ArtCache_Record(handle, &parts);
    assert(record && record[0] == 'G' && notes == 1);
    /* The thumbnail in the record is the picture's, so that is the PNG to draw it from. */
    ArtCache_Paths(handle, &art, &thumbnail);
    assert(art && !thumbnail);
    wait_ms(60);
    assert(files_in(NULL) == 0);
}

/* What an earlier build left, or a killed run, goes at once, long before the list is done;
 * a file no card names stays until the room is wanted. */
static void stale_files(void)
{
    static int handles[40];
    char path[320];
    FILE *file;
    add_many(0, 40, 'S', handles); /* 200 ms each */
    snprintf(path, sizeof(path), "%s/cache", root);
    mkdir(path, 0755);
    mkdir(cache, 0755);
    snprintf(path, sizeof(path), "%s/%016llx-%08x", cache, 0x77ull, BUILD + 1);
    assert((file = fopen(path, "wb")) && !fclose(file));
    snprintf(path, sizeof(path), "%s/%016llx-%08x.99.tmp", cache, 0x77ull, BUILD);
    assert((file = fopen(path, "wb")) && !fclose(file));
    snprintf(path, sizeof(path), "%s/junk", cache);
    assert((file = fopen(path, "wb")) && !fclose(file));
    snprintf(path, sizeof(path), "%s/%016llx-%08x", cache, 0x78ull, BUILD);
    file = fopen(path, "wb");
    assert(file && !fseek(file, FILE_BYTES - 1, SEEK_SET) && fputc(0, file) == 0 && !fclose(file));
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(100);
    assert(__atomic_load_n(&started_making, __ATOMIC_SEQ_CST) < 40);
    assert(files_in(NULL) == 1 && !files_in(".tmp"));
    assert(access(path, F_OK) == 0);
}

/* --- how much is kept ---------------------------------------------------- */

static int hundred[100];

static void capped(void)
{
    add_many(0, 100, 'A', hundred);
    ArtCache_Start(cache, BUILD, NULL);
    wait_files(64); /* 1 MB of 16 KB files */
    wait_ms(100);
    assert(files_in(NULL) == 64 && __atomic_load_n(&made, __ATOMIC_SEQ_CST) == 64);
}

/* Full of this run's cards: no decoding at every start. A card read takes the oldest's
 * place, and all still reads right. */
static void capped_again(void)
{
    int parts = 0, on_disk = 0;
    add_many(0, 100, 'A', hundred);
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(100);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 0);
    for (int i = 0; i < 100; i++) {
        const unsigned char *record = ArtCache_Record(hundred[i], &parts);
        assert(record && record[0] == 'A');
    }
    on_disk = __atomic_load_n(&made, __ATOMIC_SEQ_CST);
    assert(on_disk == 36);
    wait_ms(150);
    assert(files_in(NULL) == 64);
}

static void second_list(void)
{
    static int handles[64];
    add_many(1000, 64, 'B', handles);
    ArtCache_Start(cache, BUILD, NULL);
    for (int tries = 0; tries < 800 && __atomic_load_n(&made, __ATOMIC_SEQ_CST) < 64; tries++) wait_ms(5);
    wait_ms(80);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 64);
    assert(files_in(NULL) == 64);
}

static void second_list_reads(void)
{
    static int handles[64];
    add_many(1000, 64, 'B', handles);
    ArtCache_Start(cache, BUILD, NULL);
    for (int i = 0; i < 64; i++) {
        int parts = 0;
        assert(ArtCache_Record(handles[i], &parts));
    }
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 0);
}

static void cap_of_nothing(void)
{
    add_many(0, 50, 'A', fifty);
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(100);
    assert(files_in(NULL) == 0);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 0);
    for (int i = 0; i < 50; i++) {
        int parts = 0;
        assert(ArtCache_Record(fifty[i], &parts));
    }
    wait_ms(50);
    assert(files_in(NULL) == 0);
}

static void cap(const char *text)
{
    if (text) setenv("MEMORIES_ART_CACHE_MB", text, 1);
    else unsetenv("MEMORIES_ART_CACHE_MB");
}

/* --- when the disk says no ----------------------------------------------- */

static void write_fails(void)
{
    struct rlimit one = {100, 100};
    add_many(0, 100, 'A', hundred);
    signal(SIGXFSZ, SIG_IGN);
    assert(!setrlimit(RLIMIT_FSIZE, &one));
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(120);
    /* One record to find out, and the list is left alone. */
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 1);
    assert(files_in(NULL) == 0 && !files_in(".tmp"));
    for (int i = 0; i < 100; i++) {
        int parts = 0;
        const unsigned char *record = ArtCache_Record(hundred[i], &parts);
        assert(record && record[0] == 'A' && parts == 3);
    }
}

static void no_folder(void)
{
    char path[160];
    FILE *file;
    add_many(0, 100, 'A', hundred);
    /* "cache" is a file. */
    snprintf(path, sizeof(path), "%s/cache", root);
    assert((file = fopen(path, "wb")) && !fclose(file));
    ArtCache_Start(cache, BUILD, NULL);
    wait_ms(60);
    assert(__atomic_load_n(&made, __ATOMIC_SEQ_CST) == 0);
    for (int i = 0; i < 100; i++) {
        int parts = 0;
        const unsigned char *record = ArtCache_Record(hundred[i], &parts);
        assert(record && record[0] == 'A');
    }
}

/* --- the thread ---------------------------------------------------------- */

/* The game's thread wants what the worker is making: it waits, and makes nothing. */
static void asks_for_what_is_being_made(void)
{
    int parts = 0, handles[1];
    const unsigned char *record;
    add_many(0, 1, 'S', handles);
    ArtCache_Start(cache, BUILD, NULL);
    for (int tries = 0; tries < 400 && !__atomic_load_n(&started_making, __ATOMIC_SEQ_CST); tries++) wait_ms(1);
    record = ArtCache_Record(handles[0], &parts);
    assert(record && record[0] == 'S' && parts == 3);
    assert(__atomic_load_n(&started_making, __ATOMIC_SEQ_CST) == 1);
}

/* Faster than the worker writes: all reach the disk, by way of the worker. */
static void game_makes_many(void)
{
    static int handles[200];
    add_many(0, 200, 'A', handles);
    ArtCache_Start(cache, BUILD, NULL);
    for (int i = 0; i < 200; i++) {
        int parts = 0;
        assert(ArtCache_Record(handles[i], &parts));
    }
    wait_files(200);
}

/* The decks first, the last of them first; then on from the last card seen. */
static void decks_first(void)
{
    static int handles[30];
    int wanted[3], first;
    add_many(0, 30, 'S', handles);
    wanted[0] = handles[20];
    wanted[1] = handles[5];
    wanted[2] = handles[11];
    ArtCache_Prefer(wanted, 3);
    ArtCache_Start(cache, BUILD, NULL);
    for (int tries = 0; tries < 2000 && __atomic_load_n(&started_making, __ATOMIC_SEQ_CST) < 4; tries++) wait_ms(5);
    assert(__atomic_load_n(&order[0], __ATOMIC_SEQ_CST) == 11);
    assert(__atomic_load_n(&order[1], __ATOMIC_SEQ_CST) == 5);
    assert(__atomic_load_n(&order[2], __ATOMIC_SEQ_CST) == 20);
    first = __atomic_load_n(&order[3], __ATOMIC_SEQ_CST);
    assert(first == 0);
}

/* --- in an interrupt ----------------------------------------------------- */

static volatile int in_tick;
static int ticking(void) { return in_tick; }

static void interrupted(void)
{
    int parts = 0, handles[2], again;
    const unsigned char *record;
    add_many(0, 2, 'A', handles);
    ArtCache_Start(cache, BUILD, ticking);
    /* The worker is not to get there first. */
    in_tick = 1;
    again = __atomic_load_n(&made, __ATOMIC_SEQ_CST);
    record = ArtCache_Record(handles[0], &parts);
    assert(!record && parts == -1);
    assert(!ArtCache_FirstSight(handles[0]));
    ArtCache_Prefer(handles, 2);
    in_tick = 0;
    (void)again;
    record = ArtCache_Record(handles[0], &parts);
    assert(record && parts == 3);
    /* What memory holds, the interrupt may have. */
    in_tick = 1;
    parts = 0;
    record = ArtCache_Record(handles[0], &parts);
    assert(record && record[0] == 'A' && parts == 3);
    assert(!ArtCache_FirstSight(handles[0]));
    in_tick = 0;
    assert(ArtCache_FirstSight(handles[0]));
}

int main(void)
{
    assert(mkdtemp(root));
    snprintf(cache, sizeof(cache), "%s/cache/card-art", root);
    cap(NULL);

    run(made_when_asked);
    clear();
    run(more_than_memory);

    /* Filled in one run, read in the next; spoiled, read right, and right again. */
    clear();
    run(fill_fifty);
    run(from_disk);
    spoil();
    run(spoiled_read);
    run(from_disk);
    {
        char path[320];
        clear();
        run(fill_fifty);
        path_of_nth(9, path, sizeof(path));
        assert(!truncate(path, 100));
        run(short_file_replaced);
        run(from_disk);
    }
    clear();
    same_size_same_time();
    clear();
    run(no_stat);
    clear();
    run(bad_thumbnail);
    run(bad_thumbnail);
    clear();
    run(stale_files);

    /* The cap, and what is not a cap. */
    clear();
    cap("1");
    run(capped);
    run(capped_again);
    run(second_list);
    run(second_list_reads);
    {
        static const char *const bad[] = {"abc", "-1", "12x", "", " 1", "1048577", "99999999999999999999"};
        for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
            clear();
            cap(bad[i]);
            run(fill_fifty);
        }
    }
    clear();
    cap(NULL);
    run(fill_fifty);
    cap("0");
    run(cap_of_nothing);
    cap(NULL);

    clear();
    run(write_fails);
    clear();
    run(no_folder);

    clear();
    run(asks_for_what_is_being_made);
    clear();
    run(game_makes_many);
    clear();
    run(decks_first);
    clear();
    run(interrupted);

    {
        char command[128];
        snprintf(command, sizeof(command), "rm -rf %s", root);
        assert(!system(command));
    }
    puts("Card art cache: made when asked, kept, checked, capped, read ahead");
    return 0;
}
