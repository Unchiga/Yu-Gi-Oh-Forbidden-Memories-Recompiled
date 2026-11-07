/* The art records made on demand (src/pc/cards/card_art_cache.c), with the making
 * replaced: what is asked for, how often, what the disk gives back, what a bad
 * PNG leaves, and the thread that reads ahead. */
#define _POSIX_C_SOURCE 200809L
#include "pc/cards/card_art_cache.h"
#include "pc/cards/art.h"
#include <assert.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int made, notes;

/* A "PNG" is a text file; a record is filled with its first byte. */
static int fill(const char *path, unsigned char *record, size_t at, size_t size)
{
    FILE *file = fopen(path, "rb");
    int c = file ? fgetc(file) : EOF;
    if (file) fclose(file);
    if (c == EOF || c == 'x') return 0;
    memset(record + at, c, size);
    __atomic_add_fetch(&made, 1, __ATOMIC_RELAXED);
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

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

static int files_in(const char *path)
{
    DIR *folder = opendir(path);
    struct dirent *item;
    int n = 0;
    assert(folder);
    while ((item = readdir(folder)) != NULL) n += item->d_name[0] != '.' && !strstr(item->d_name, ".tmp");
    closedir(folder);
    return n;
}

int main(void)
{
    char root[64] = "art-cache-test-XXXXXX", name[128], cache[128];
    const unsigned char *record;
    int parts = 0, one, two, again, bad, half, handles[1200];
    struct timespec gap = {0, 5000000};
    assert(mkdtemp(root));
    snprintf(name, sizeof(name), "%s/a.png", root);
    put(name, "A");
    one = ArtCache_Add("mod", 0, name, NULL);
    again = ArtCache_Add("mod", 1, name, NULL);
    assert(one && one == again); /* one PNG, one record */
    snprintf(name, sizeof(name), "%s/b.png", root);
    put(name, "B");
    {
        char art[128];
        snprintf(art, sizeof(art), "%s/a.png", root);
        two = ArtCache_Add("mod", 2, art, name);
    }
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
    /* A bad picture says so when it is read; its thumbnail still counts. */
    snprintf(name, sizeof(name), "%s/bad.png", root);
    put(name, "x");
    {
        char good[128];
        snprintf(good, sizeof(good), "%s/b.png", root);
        half = ArtCache_Add("mod", 3, name, good);
        bad = ArtCache_Add("mod", 4, name, NULL);
    }
    record = ArtCache_Record(half, &parts);
    assert(record && parts == ART_CACHE_THUMBNAIL && notes == 1);
    assert(!ArtCache_Record(bad, &parts) && notes == 2);
    assert(!ArtCache_Record(bad, &parts) && notes == 2);
    assert(!ArtCache_Record(0, &parts) && !ArtCache_Record(9999, &parts));
    /* More than memory holds: each comes back whole. */
    for (int i = 0; i < 1200; i++) {
        char text[2] = {(char)('a' + i % 20), 0};
        snprintf(name, sizeof(name), "%s/p%d.png", root, i);
        put(name, text);
        handles[i] = ArtCache_Add("mod", 10 + i, name, NULL);
        assert(handles[i]);
    }
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 1200; i++) {
            record = ArtCache_Record(handles[i], &parts);
            assert(record && record[0] == 'a' + i % 20);
        }
    /* The disk, filled by the thread for every record that is whole and not one of
     * the bad ones, and what it holds is what is read. */
    snprintf(cache, sizeof(cache), "%s/cache/card-art", root);
    ArtCache_Start(cache, 0x1234);
    for (int tries = 0; files_in(cache) < 1202 && tries < 4000; tries++) nanosleep(&gap, NULL);
    assert(files_in(cache) == 1202);
    {
        int before = __atomic_load_n(&made, __ATOMIC_RELAXED);
        for (int i = 0; i < 1200; i++) {
            record = ArtCache_Record(handles[i], &parts);
            assert(record && record[0] == 'a' + i % 20);
        }
        assert(__atomic_load_n(&made, __ATOMIC_RELAXED) == before);
    }
    /* A file of the mod's that changes is another record. */
    sleep(1);
    snprintf(name, sizeof(name), "%s/a.png", root);
    put(name, "Z!");
    again = ArtCache_Add("mod", 2000, name, NULL);
    assert(again != one);
    record = ArtCache_Record(again, &parts);
    assert(record && record[0] == 'Z');
    puts("Card art cache: made when asked, kept, read ahead");
    return 0;
}
