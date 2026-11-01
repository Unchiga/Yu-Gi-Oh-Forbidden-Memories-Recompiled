/* The mod .zip import (src/pc/mods/import.h): .zip files written here, entry
 * by entry, then opened and installed into a scratch mods folder. Checks
 * the layouts a mod comes in, what counts as code, Replace, and that a
 * bad .zip (names leaving the folder, links, damage) writes nothing. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "pc/mods/import.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#ifndef _WIN32
#include <dirent.h>
#endif

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);                             \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "zip_writer.h"

/* --- the scratch folders ---------------------------------------------- */

static char root[1024], mods[1100], zip[1100];

static int exists(const char *relative)
{
    char path[2048];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return !stat(path, &info);
}
static int file_is(const char *relative, const char *text)
{
    char path[2048], got[256];
    FILE *f;
    size_t n;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (!(f = fopen(path, "rb")))
        return 0;
    n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    got[n] = 0;
    return !strcmp(got, text);
}
/* What the mods folder holds, names joined by spaces, sorted. */
static const char *listing(const char *relative)
{
    static char out[1024];
    char path[2048], names[32][128];
    int n = 0;
    DIR *d;
    struct dirent *e;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    out[0] = 0;
    if (!(d = opendir(path)))
        return out;
    while ((e = readdir(d)) && n < 32)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            {
            size_t k = strlen(e->d_name) < sizeof(names[0]) ? strlen(e->d_name) : sizeof(names[0]) - 1;
            memcpy(names[n], e->d_name, k);
            names[n++][k] = 0;
        }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[128];
            memcpy(t, names[j], sizeof(t));
            memcpy(names[j], names[j - 1], sizeof(t));
            memcpy(names[j - 1], t, sizeof(t));
        }
    for (int i = 0; i < n; i++)
        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", i ? " " : "", names[i]);
    return out;
}
static void reset(void)
{
    Mods_ImportRemoveTree(mods);
    mkdir(mods, 0777);
}
/* A folder below the scratch root (Paths_MakeDirs is not used: on Windows
 * it cannot start at a drive letter). */
static void make(const char *relative)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    for (char *c = path + strlen(root) + 1;; c++)
        if (*c == '/' || !*c) {
            char kept = *c;
            *c = 0;
            mkdir(path, 0777);
            if (!(*c = kept))
                break;
        }
}

static void file_write(const char *relative, const char *text)
{
    char path[2048];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* Opens and installs `items`; 1 when both worked. `why` gets the reason. */
static int import(const ZipItem *items, int n, int *count, char *why, size_t size)
{
    ModsImport *imp;
    int ok;
    write_zip(zip, items, n);
    why[0] = 0;
    imp = Mods_ImportOpen(zip, why, size);
    *count = imp ? Mods_ImportCount(imp) : -1;
    if (!imp)
        return 0;
    ok = Mods_ImportInstall(imp, mods, why, size);
    if (!ok && getenv("MOD_IMPORT_VERBOSE"))
        fprintf(stderr, "install: %s\n", why);
    Mods_ImportClose(imp);
    return ok;
}

static const char *const MANIFEST_A = "{\"id\": \"alpha\", \"name\": \"Alpha mod\"}";

/* Mods_ImportStepped's hook: the chunk count, and a cancel at one of them. */

static int *step_cancel, step_calls, step_cancel_at;
static void step(void)
{
    if (++step_calls == step_cancel_at)
        __atomic_store_n(step_cancel, 1, __ATOMIC_RELEASE);
}

int main(void)
{
    char why[512];
    int count;
    const char *base = getenv("TMPDIR");
#ifdef _WIN32
    if (!base)
        base = getenv("TEMP");
#endif
    snprintf(root, sizeof(root), "%s/mod-import-XXXXXX", base ? base : "/tmp");
    for (char *c = root; *c; c++)
        if (*c == '\\')
            *c = '/';
    if (!mkdtemp(root)) {
        perror(root);
        return 2;
    }
    snprintf(mods, sizeof(mods), "%s/mods", root);
    snprintf(zip, sizeof(zip), "%s/in.zip", root);

    /* A mod in a folder at the top: its files, its subfolders. */
    reset();
    {
        ZipItem items[] = {{"alpha/", NULL, 0, 040755, 0, 0, 0, 0}, {"alpha/mod.json", MANIFEST_A, 1, 0100644, 0, 0, 0, 0},
                        {"alpha/text/name.txt", "hello", 0, 0, 0, 0, 0, 0}};
        CHECK(import(items, 3, &count, why, sizeof(why)));
        CHECK(count == 1);
        CHECK(file_is("mods/alpha/text/name.txt", "hello"));
        CHECK(file_is("mods/alpha/mod.json", MANIFEST_A));
        CHECK(!strcmp(listing("mods"), "alpha")); /* no .import- left */
    }
    /* The .zip's root is the mod: the folder is the manifest's id. */
    reset();
    {
        ZipItem items[] = {{"mod.json", "{\"id\": \"rooted\", \"name\": \"At the root\"}", 1, 0, 0, 0, 0, 0},
                        {"name.txt", "root", 0, 0, 0, 0, 0, 0}};
        ModsImport *imp;
        write_zip(zip, items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp && Mods_ImportCount(imp) == 1);
        if (imp) {
            CHECK(!strcmp(Mods_ImportMod(imp, 0)->folder, "rooted"));
            CHECK(!strcmp(Mods_ImportMod(imp, 0)->name, "At the root"));
            CHECK(!Mods_ImportMod(imp, 0)->code);
            CHECK(Mods_ImportInstall(imp, mods, why, sizeof(why)));
            Mods_ImportClose(imp);
        }
        CHECK(file_is("mods/rooted/name.txt", "root"));
    }
    /* One wrapper folder; what is beside the mod there stays out. */
    reset();
    {
        ZipItem items[] = {{"Download/README.txt", "read me", 0, 0, 0, 0, 0, 0},
                        {"Download/alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
                        {"__MACOSX/Download/alpha/._mod.json", "x", 0, 0, 0, 0, 0, 0},
                        {"__MACOSX/beta/mod.json", "{}", 0, 0, 0, 0, 0, 0}};
        CHECK(import(items, 4, &count, why, sizeof(why)));
        CHECK(count == 1);
        CHECK(!strcmp(listing("mods"), "alpha"));
        CHECK(!strcmp(listing("mods/alpha"), "mod.json"));
    }
    /* Several mods, one with a mod.json of its own inside (its file). */
    reset();
    {
        ZipItem items[] = {{"alpha/mod.json", MANIFEST_A, 1, 0, 0, 0, 0, 0},
                        {"alpha/extra/mod.json", "{\"id\": \"inner\"}", 0, 0, 0, 0, 0, 0},
                        {"beta/mod.json", "{\"id\": \"beta\"}", 1, 0, 0, 0, 0, 0},
                        {"gamma/mod.json", "not json", 0, 0, 0, 0, 0, 0}};
        CHECK(import(items, 4, &count, why, sizeof(why)));
        CHECK(count == 3);
        CHECK(!strcmp(listing("mods"), "alpha beta gamma"));
        CHECK(exists("mods/alpha/extra/mod.json"));
    }
    /* Too deep, or no mod.json: no mod. */
    reset();
    {
        ZipItem items[] = {{"readme.txt", "nothing", 0, 0, 0, 0, 0, 0}, {"a/b/c/mod.json", "{}", 0, 0, 0, 0, 0, 0}};
        CHECK(!import(items, 2, &count, why, sizeof(why)));
        CHECK(count == 0);
        CHECK(strstr(why, "no mod") != NULL);
        CHECK(!strcmp(listing("mods"), ""));
    }
    /* Code: as the loader reads it (mods.c), a "library" or a "libraries"
     * object with any entry, even one without this game's target; what
     * objects the .zip has beside it does not change that. */
    reset();
    {
        static const struct {
            const char *manifest, *file;
            int code;
        } cases[] = {
            {"{\"id\": \"cm\", \"library\": \"cm\"}", "c/cm.o", 1},
            {"{\"id\": \"cm\", \"library\": \"cm\"}", "c/cm.aarch64.o", 1},
            {"{\"id\": \"cm\", \"library\": \"cm.o\"}", "c/cm.aarch64.o", 1},
            {"{\"id\": \"cm\", \"library\": \"\"}", "c/cm.o", 0},
            {"{\"id\": \"cm\", \"libraries\": {\"aarch64\": \"arm/x.o\"}}", "c/arm/x.o", 1},
            {"{\"id\": \"cm\", \"libraries\": {\"x86_64-windows\": \"w.o\"}}", "c/cm.aarch64.o", 1},
            {"{\"id\": \"cm\", \"libraries\": {}}", "c/cm.o", 0},
            {"{\"id\": \"cm\"}", "c/cm.o", 0},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            ZipItem items[] = {{"c/mod.json", cases[i].manifest, 0, 0, 0, 0, 0, 0},
                            {cases[i].file, "\x7f" "ELF", 0, 0, 0, 0, 0, 0}};
            ModsImport *imp;
            write_zip(zip, items, 2);
            imp = Mods_ImportOpen(zip, why, sizeof(why));
            CHECK(imp && Mods_ImportCount(imp) == 1);
            if (imp) {
                CHECK(Mods_ImportMod(imp, 0)->code == cases[i].code);
                Mods_ImportClose(imp);
            }
        }
    }
    /* Two names that differ only beyond ASCII: one file where the storage
     * folds them (NTFS, APFS, Android's shared storage; probed here), so the
     * import is refused there with nothing written; two files elsewhere. */
    reset();
    {
        ZipItem items[] = {{"fold/mod.json", "{}", 0, 0, 0, 0, 0, 0},
                           {"fold/\xc3\x89.txt", "upper", 0, 0, 0, 0, 0, 0},
                           {"fold/\xc3\xa9.txt", "lower", 0, 0, 0, 0, 0, 0}};
        char probe[1200];
        FILE *f;
        int done, folds;
        snprintf(probe, sizeof(probe), "%s/probe-\xc3\x89", root);
        f = fopen(probe, "wb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
        folds = exists("probe-\xc3\xa9");
        remove(probe);
        done = import(items, 3, &count, why, sizeof(why));
        if (folds) {
            CHECK(!done && strstr(why, "are one file on this storage") != NULL);
            CHECK(!strcmp(listing("mods"), ""));
        } else
            CHECK(done && file_is("mods/fold/\xc3\x89.txt", "upper") && file_is("mods/fold/\xc3\xa9.txt", "lower"));
#ifdef _WIN32
        CHECK(folds); /* NTFS folds them: the refusal is tested there */
#endif
    }
    /* Replace: the old folder goes; without it the import is refused and
     * the old one stays as it was. */
    reset();
    {
        ZipItem old_items[] = {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
                            {"alpha/old.txt", "old", 0, 0, 0, 0, 0, 0}};
        ZipItem new_items[] = {{"alpha/mod.json", MANIFEST_A, 1, 0, 0, 0, 0, 0},
                            {"alpha/new.txt", "new", 1, 0, 0, 0, 0, 0}};
        ModsImport *imp;
        CHECK(import(old_items, 2, &count, why, sizeof(why)));
        write_zip(zip, new_items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            CHECK(!Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(strstr(why, "already") != NULL);
            CHECK(file_is("mods/alpha/old.txt", "old") && !exists("mods/alpha/new.txt"));
            strcat(strcpy(Mods_ImportMod(imp, 0)->replace, mods), "/alpha");
            CHECK(Mods_ImportInstall(imp, mods, why, sizeof(why)));
            Mods_ImportClose(imp);
        }
        CHECK(file_is("mods/alpha/new.txt", "new") && !exists("mods/alpha/old.txt"));
        CHECK(!strcmp(listing("mods"), "alpha"));
    }
    /* Replace of a folder with another name (the same id installed as
     * alpha-2): the new mod lands as alpha, the old folder is gone, and
     * nothing is left. */
    reset();
    {
        ZipItem old_items[] = {{"alpha-2/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
                               {"alpha-2/old.txt", "old", 0, 0, 0, 0, 0, 0}};
        ZipItem new_items[] = {{"alpha/mod.json", MANIFEST_A, 1, 0, 0, 0, 0, 0},
                               {"alpha/new.txt", "new", 0, 0, 0, 0, 0, 0}};
        ModsImport *imp;
        CHECK(import(old_items, 2, &count, why, sizeof(why)));
        write_zip(zip, new_items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            strcat(strcpy(Mods_ImportMod(imp, 0)->replace, mods), "/alpha-2");
            CHECK(Mods_ImportInstall(imp, mods, why, sizeof(why)));
            Mods_ImportClose(imp);
        }
        CHECK(file_is("mods/alpha/new.txt", "new") && !strcmp(listing("mods"), "alpha"));
    }
    /* A second mod failing puts the first one's old folder back. */
    reset();
    {
        ZipItem old_items[] = {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
                            {"alpha/old.txt", "old", 0, 0, 0, 0, 0, 0}};
        ZipItem new_items[] = {{"alpha/mod.json", MANIFEST_A, 1, 0, 0, 0, 0, 0},
                            {"beta/mod.json", "{\"id\": \"beta\"}", 0, 0, 0, 0, 0, 0}};
        ModsImport *imp;
        CHECK(import(old_items, 2, &count, why, sizeof(why)));
        make("mods/beta"); /* there, and not marked to be replaced */
        write_zip(zip, new_items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            strcat(strcpy(Mods_ImportMod(imp, 0)->replace, mods), "/alpha");
            CHECK(!Mods_ImportInstall(imp, mods, why, sizeof(why)));
            Mods_ImportClose(imp);
        }
        CHECK(file_is("mods/alpha/old.txt", "old"));
        CHECK(!strcmp(listing("mods"), "alpha beta"));
        CHECK(!strcmp(listing("mods/beta"), ""));
    }
    /* Refused before anything is written: names that leave the folder,
     * links, encryption, other compressions, two entries for one file. */
    {
        static const ZipItem bad[][2] = {
            {{"../evil.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/../../evil.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha\\..\\..\\evil.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"/evil.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"C:/evil.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/./x.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha//x.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/\x01x.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/\xff.txt", "x", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/link", "../../evil", 0, 0120777, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/fifo", "", 0, 0010644, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/x.txt", "x", 0, 0, 1, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/x.txt", "x", 0, 0, 0, 12, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}, {"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/Name.txt", "a", 0, 0, 0, 0, 0, 0}, {"alpha/name.TXT", "b", 0, 0, 0, 0, 0, 0}},
            {{"a/mod.json", "{\"id\": \"same\"}", 0, 0, 0, 0, 0, 0}, {"b/mod.json", "{\"id\": \"same\"}", 0, 0, 0, 0, 0, 0}},
            {{"alpha/mod.json", "{}", 0, 0, 0, 0, 0, 0},
             {"alpha/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/21/22/23/24/25/26/27/28/29/30/31/32/x", "deep",
              0, 0, 0, 0, 0, 0}},
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            reset();
            write_zip(zip, bad[i], 2);
            CHECK(Mods_ImportOpen(zip, why, sizeof(why)) == NULL);
            CHECK(why[0] != 0);
            if (getenv("MOD_IMPORT_VERBOSE"))
                fprintf(stderr, "refused %u: %s\n", (unsigned)i, why);
            CHECK(!strcmp(listing("mods"), ""));
            CHECK(!exists("evil.txt") && !exists("mods/evil.txt"));
        }
        CHECK(!strcmp(listing(""), "in.zip mods"));
    }
    /* Damage found while unpacking: a wrong CRC, a size the data does not
     * keep to. Nothing is left in the mods folder. */
    {
        static const ZipItem damaged[][2] = {
            {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0}, {"alpha/x.txt", "data", 0, 0, 0, 0, 1, 0}},
            {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0}, {"alpha/x.txt", "data", 1, 0, 0, 0, 1, 0}},
            {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
             {"alpha/x.txt", "a long text that inflates past the size given", 1, 0, 0, 0, 0, 4}},
        };
        for (size_t i = 0; i < sizeof(damaged) / sizeof(damaged[0]); i++) {
            reset();
            CHECK(!import(damaged[i], 2, &count, why, sizeof(why)));
            CHECK(count == 1);
            CHECK(strstr(why, "damaged") != NULL);
            CHECK(!strcmp(listing("mods"), ""));
        }
    }
    /* Not a .zip at all. */
    {
        FILE *f = fopen(zip, "wb");
        fputs("This is a text file, long enough to be looked at as a .zip.", f);
        fclose(f);
        CHECK(Mods_ImportOpen(zip, why, sizeof(why)) == NULL);
        CHECK(strstr(why, "not a .zip") != NULL);
    }
    /* A mod's folder in two letter cases is one folder (a case-blind file
     * system): both halves are unpacked into it. */
    reset();
    {
        ZipItem items[] = {{"Alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0},
                           {"alpha/data.bin", "data", 0, 0, 0, 0, 0, 0}};
        CHECK(import(items, 2, &count, why, sizeof(why)));
        CHECK(file_is("mods/Alpha/data.bin", "data"));
    }
    /* Watched (the HD pack's download unpacks on a thread of its own): the
     * bytes so far; a cancel before the next file leaves nothing. */
    reset();
    {
        ZipItem items[] = {{"alpha/mod.json", MANIFEST_A, 1, 0, 0, 0, 0, 0},
                           {"alpha/a.txt", "0123456789", 1, 0, 0, 0, 0, 0}};
        unsigned done = 0;
        int cancel = 0;
        ModsImport *imp;
        write_zip(zip, items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            CHECK(Mods_ImportBytes(imp) == strlen(MANIFEST_A) + 10);
            Mods_ImportWatch(imp, &done, &cancel);
            CHECK(Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(done == Mods_ImportBytes(imp));
            Mods_ImportClose(imp);
        }
        reset();
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            done = 0;
            cancel = 1;
            Mods_ImportWatch(imp, &done, &cancel);
            CHECK(!Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(!strcmp(why, "Cancelled.") && done == 0);
            CHECK(!strcmp(listing("mods"), ""));
            Mods_ImportClose(imp);
        }
    }
    /* The progress is the bytes written chunk by chunk, and a cancel stops in
     * the middle of a big file (Mods_ImportStepped: the test's hook sees
     * each chunk): the chunks before it are counted, the half-written file
     * and its folder are gone. */
    reset();
    {
        static char body[300001];
        ZipItem items[] = {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0}, {"alpha/big.bin", body, 0, 0, 0, 0, 0, 0}};
        unsigned done = 0;
        int cancel = 0;
        ModsImport *imp;
        memset(body, 'B', sizeof(body) - 1);
        write_zip(zip, items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            step_cancel = &cancel;
            step_calls = 0;
            step_cancel_at = 1000;
            Mods_ImportWatch(imp, &done, &cancel);
            Mods_ImportStepped(imp, step);
            CHECK(Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(step_calls == 1 + 5 && done == Mods_ImportBytes(imp)); /* the manifest, and 300000 bytes in 5 chunks */
            Mods_ImportClose(imp);
        }
        reset();
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            done = 0;
            cancel = 0;
            step_calls = 0;
            step_cancel_at = 4; /* the manifest, then the big file's third chunk */
            Mods_ImportWatch(imp, &done, &cancel);
            Mods_ImportStepped(imp, step);
            CHECK(!Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(!strcmp(why, "Cancelled.") && step_calls == 4 && done == strlen(MANIFEST_A) + 3 * 65536);
            CHECK(!strcmp(listing("mods"), ""));
            Mods_ImportClose(imp);
        }
    }
    /* The same, with the big file deflated: its 300000 bytes are one chunk of
     * the .zip, so the cancel is seen between the chunks it unpacks to. */
    reset();
    {
        static char body[300001];
        ZipItem items[] = {{"alpha/mod.json", MANIFEST_A, 0, 0, 0, 0, 0, 0}, {"alpha/big.bin", body, 1, 0, 0, 0, 0, 0}};
        unsigned done = 0;
        int cancel = 0;
        ModsImport *imp;
        memset(body, 'B', sizeof(body) - 1);
        write_zip(zip, items, 2);
        imp = Mods_ImportOpen(zip, why, sizeof(why));
        CHECK(imp != NULL);
        if (imp) {
            step_cancel = &cancel;
            step_calls = 0;
            step_cancel_at = 4; /* the manifest, then the big file's third chunk */
            Mods_ImportWatch(imp, &done, &cancel);
            Mods_ImportStepped(imp, step);
            CHECK(!Mods_ImportInstall(imp, mods, why, sizeof(why)));
            CHECK(!strcmp(why, "Cancelled.") && step_calls == 4 && done == strlen(MANIFEST_A) + 3 * 65536);
            CHECK(!strcmp(listing("mods"), ""));
            Mods_ImportClose(imp);
        }
    }
    /* The process ended between a Replace's two renames: the old mod is in
     * the hidden folder's old/ and nothing is in its place, so it goes back
     * (and the unfinished new one goes). Where something is already there
     * (the new mod got in), the old copy is deleted; a removed mod's folder
     * has no old/ to restore. */
    reset();
    {
        make("mods/.import-abc123/0");
        file_write("mods/.import-abc123/0/mod.json", "new alpha");
        make("mods/.import-abc123/old/alpha/alpha");
        file_write("mods/.import-abc123/old/alpha/alpha/mod.json", "old alpha");
        make("mods/.import-def456/old/beta/beta");
        file_write("mods/.import-def456/old/beta/beta/mod.json", "old beta");
        make("mods/beta");
        file_write("mods/beta/mod.json", "new beta");
        make("mods/.import-removing-gamma/old/gamma/gamma");
        file_write("mods/.import-removing-gamma/old/gamma/gamma/mod.json", "removed gamma");
        Mods_ImportCleanup(mods);
        CHECK(file_is("mods/alpha/mod.json", "old alpha") && file_is("mods/beta/mod.json", "new beta"));
        CHECK(!strcmp(listing("mods"), "alpha beta"));
    }
    /* A Replace can replace a folder of another name (the same id installed
     * as alpha-2, the new mod's folder alpha): once the new one landed, the
     * old alpha-2 is not put back beside it. In a .zip of two mods the first
     * one landed and the process ended in the second's Replace: the first's
     * old copy stays gone, the second's (nothing in its place) goes back. */
    reset();
    {
        make("mods/alpha");
        file_write("mods/alpha/mod.json", "new alpha");
        make("mods/.import-abc123/old/alpha/alpha-2");
        file_write("mods/.import-abc123/old/alpha/alpha-2/mod.json", "old alpha-2");
        Mods_ImportCleanup(mods);
        CHECK(file_is("mods/alpha/mod.json", "new alpha") && !strcmp(listing("mods"), "alpha"));
        make("mods/m0");
        file_write("mods/m0/mod.json", "new m0");
        make("mods/.import-def456/old/m0/m0");
        file_write("mods/.import-def456/old/m0/m0/mod.json", "old m0");
        make("mods/.import-def456/old/m1/m1");
        file_write("mods/.import-def456/old/m1/m1/mod.json", "old m1");
        make("mods/.import-def456/1");
        Mods_ImportCleanup(mods);
        CHECK(file_is("mods/m0/mod.json", "new m0") && file_is("mods/m1/mod.json", "old m1"));
        CHECK(!strcmp(listing("mods"), "alpha m0 m1"));
    }
    /* What an interrupted import leaves is cleared. */
    reset();
    {
        char path[2048];
        FILE *f;
        make("mods/.import-abc123/0/x");
        snprintf(path, sizeof(path), "%s/.incoming.zip", mods);
        f = fopen(path, "wb");
        fclose(f);
        make("mods/kept");
        Mods_ImportCleanup(mods);
        CHECK(!strcmp(listing("mods"), "kept"));
    }
    /* The Mods panel's Remove (Mods_ImportRemoveInstalled): a mod's own folder
     * in the mods folder goes, with what is in it; nothing else does. */
    reset();
    {
        char why[200], path[2048], slashed[1200];
        make("mods/gone/deep/er");
        file_write("mods/gone/mod.json", "{}");
        file_write("mods/gone/deep/er/x.txt", "x");
        make("mods/kept");
        make("outside/victim");
        file_write("outside/victim/mod.json", "{}");
        snprintf(path, sizeof(path), "%s/gone", mods);
        CHECK(!Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)));
        CHECK(!exists("mods/gone") && exists("mods/kept") && exists("mods"));
        /* The folder leaves the mods folder in one rename, to a hidden folder
         * that is then deleted; one an earlier remove left under that name
         * is no obstacle, and what is left of any is swept by the cleanup. */
        make("mods/again/deep");
        file_write("mods/again/deep/f.txt", "x");
        make("mods/.import-removing-again/old");
        snprintf(path, sizeof(path), "%s/again", mods);
        CHECK(!Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)));
        CHECK(!exists("mods/again") && !exists("mods/.import-removing-again"));
        make("mods/.import-removing-left/deep");
        file_write("mods/.import-removing-left/deep/f.txt", "x");
        make("mods/kept2");
        Mods_ImportCleanup(mods);
        CHECK(!exists("mods/.import-removing-left") && exists("mods/kept2") && exists("mods/kept"));
        /* already gone: nothing to do */
        snprintf(path, sizeof(path), "%s/gone", mods);
        CHECK(!Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)));
        /* a trailing slash on the mods folder is the same folder */
        make("mods/slash");
        snprintf(path, sizeof(path), "%s/slash", mods);
        snprintf(slashed, sizeof(slashed), "%s/", mods);
        CHECK(!Mods_ImportRemoveInstalled(slashed, path, why, sizeof(why)));
        CHECK(!exists("mods/slash"));
        /* and so is a doubled separator (a MEMORIES_MODS_DIR ending in one) */
        make("mods/double");
        snprintf(path, sizeof(path), "%s//double", mods);
        CHECK(!Mods_ImportRemoveInstalled(slashed, path, why, sizeof(why)));
        CHECK(!exists("mods/double"));
        snprintf(path, sizeof(path), "%s//", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/kept"));
        /* refused, and nothing touched */
        make("mods/kept/inner");
        snprintf(path, sizeof(path), "%s", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/kept")); /* the folder itself */
        snprintf(path, sizeof(path), "%s/", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/kept"));
        snprintf(path, sizeof(path), "%s/kept/inner", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/kept/inner")); /* two deep */
        snprintf(path, sizeof(path), "%s/../outside/victim", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("outside/victim/mod.json"));
        snprintf(path, sizeof(path), "%s/kept/../../outside/victim", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("outside/victim/mod.json"));
        snprintf(path, sizeof(path), "%s/..", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("outside/victim/mod.json"));
        snprintf(path, sizeof(path), "%s/.", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/kept"));
        snprintf(path, sizeof(path), "%s/.import-abc", mods); /* the import's own, hidden */
        make("mods/.import-abc");
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/.import-abc"));
        /* a sibling that starts with the mods folder's name */
        snprintf(path, sizeof(path), "%s-other/x", mods);
        make("mods-other/x");
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods-other/x"));
        CHECK(Mods_ImportRemoveInstalled("", mods, why, sizeof(why)) && Mods_ImportRemoveInstalled(NULL, mods, why, sizeof(why)));
        CHECK(Mods_ImportRemoveInstalled(mods, NULL, why, sizeof(why)) && exists("mods/kept"));
        CHECK(Mods_ImportRemoveInstalled(mods, "", why, sizeof(why)) && exists("mods/kept"));
        /* a file named like a mod is not a folder of one */
        file_write("mods/afile", "x");
        snprintf(path, sizeof(path), "%s/afile", mods);
        CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("mods/afile"));
#ifndef _WIN32
        /* a link to a folder elsewhere is refused, not followed */
        {
            char target[2048];
            snprintf(target, sizeof(target), "%s/outside/victim", root);
            snprintf(path, sizeof(path), "%s/linked", mods);
            if (!symlink(target, path)) {
                CHECK(Mods_ImportRemoveInstalled(mods, path, why, sizeof(why)) && exists("outside/victim/mod.json"));
                CHECK(exists("mods/linked"));
            }
        }
#endif
    }
    /* A pack of mods in one .zip, one wrapper folder down: as many as the
     * game takes; one more is refused before anything is written. */
    {
        static char names[MODS_IMPORT_MAX + 1][32], manifests[MODS_IMPORT_MAX + 1][64];
        static ZipItem items[MODS_IMPORT_MAX + 1];
        for (int i = 0; i <= MODS_IMPORT_MAX; i++) {
            snprintf(names[i], sizeof(names[i]), "pack/m%03d/mod.json", i);
            snprintf(manifests[i], sizeof(manifests[i]), "{\"id\": \"m%03d\"}", i);
            items[i] = (ZipItem){names[i], manifests[i], 1, 0, 0, 0, 0, 0};
        }
        reset();
        CHECK(import(items, MODS_IMPORT_MAX, &count, why, sizeof(why)));
        CHECK(count == MODS_IMPORT_MAX);
        CHECK(exists("mods/m000/mod.json") && exists("mods/m255/mod.json"));
        reset();
        CHECK(!import(items, MODS_IMPORT_MAX + 1, &count, why, sizeof(why)) && count == -1);
        CHECK(strstr(why, "more than 256 mods") != NULL);
        CHECK(!exists("mods/m000"));
    }
    Mods_ImportRemoveTree(root);
    if (failures)
        fprintf(stderr, "%d checks failed\n", failures);
    else
        printf("mod import: all checks passed\n");
    return failures != 0;
}
