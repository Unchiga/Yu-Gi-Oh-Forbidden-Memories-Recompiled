#define _POSIX_C_SOURCE 200809L
#include "pc/compat/posix.h"
#include "pc/platform/game_files.h"
#include "pc/platform/paths.h"
#include "scratch.h"
#include <assert.h>
#include <string.h>

static char root[SCRATCH_MAX], valid[1024];
static int picks, errors, cancel, retry, picker_failure;

/* Isolate discovery from the developer's ROM and real user directory. */
const char *Paths_ProgramDir(void) { return root; }
const char *Paths_UserDir(void) { return root; }
int Paths_User(char *out, size_t size, const char *leaf)
{
    return snprintf(out, size, "%s/%s", root, leaf) >= (int)size ? -1 : 0;
}
int Paths_Program(char *out, size_t size, const char *leaf) { return Paths_User(out, size, leaf); }
void Paths_WriteBegin(void) {}
const char *Paths_WriteError(char *out, size_t size, const char *path)
{
    snprintf(out, size, "%s: failed.", path);
    return out;
}
int Platform_SelectDisc(char *path, size_t size, char *why, size_t why_size)
{
    picks++;
    assert(picks <= 2);
    if (picker_failure) {
        snprintf(why, why_size, "picker unavailable");
        return -1;
    }
    if (cancel) return 0;
    snprintf(path, size, "%s", retry && picks == 1 ? "missing.cue" : valid);
    return 1;
}
void Platform_ShowError(const char *title, const char *message)
{
    assert(title[0] && message[0]);
    errors++;
}

static void little(unsigned char *p, unsigned value)
{
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

/* A disc whose executable is not called SLUS_014.11: a community image may
 * rename it, and then the name comes from the disc's own SYSTEM.CNF, which
 * is where the PlayStation would look too. */
static void make_named_disc(const char *path)
{
    unsigned char raw[2352] = {0};
    FILE *file = fopen(path, "wb");
    int sector;
    assert(file);
    for (sector = 0; sector < 24; sector++) {
        unsigned char *data = raw + 24;
        memset(raw, 0, sizeof(raw));
        if (sector == 16) {
            data[0] = 1; memcpy(data + 1, "CD001", 5);
            little(data + 158, 20); little(data + 166, 2048);
        } else if (sector == 20) {
            /* SYSTEM.CNF, then the executable it names. */
            unsigned char *record = data;
            record[0] = 46;
            little(record + 2, 21); little(record + 10, 32);
            record[32] = 13; memcpy(record + 33, "SYSTEM.CNF;1", 13);
            record += 46;
            record[0] = 48;
            little(record + 2, 22); little(record + 10, 4096);
            record[32] = 14; memcpy(record + 33, "GOG_VNT.223;1", 14);
        } else if (sector == 21) {
            memcpy(data, "BOOT = cdrom:\\GOG_VNT.223;1\nTCB = 4\n", 32);
        } else if (sector == 22) memcpy(data, "PS-X EXE", 8);
        assert(fwrite(raw, 1, sizeof(raw), file) == sizeof(raw));
    }
    assert(!fclose(file));
}

/* Minimal synthetic MODE2/2352 disc: ISO root + two-sector PS-X EXE.
 * No copyrighted game bytes are needed to exercise discovery and setup. */
static void make_disc(const char *path, int malformed)
{
    unsigned char raw[2352] = {0};
    FILE *file = fopen(path, "wb");
    int sector;
    assert(file);
    for (sector = 0; sector < 23; sector++) {
        unsigned char *data = raw + 24;
        memset(raw, 0, sizeof(raw));
        if (sector == 16) {
            data[0] = 1; memcpy(data + 1, "CD001", 5);
            little(data + 158, 20); little(data + 166, 2048);
        } else if (sector == 20) {
            data[0] = malformed ? 34 : 46;
            little(data + 2, 21); little(data + 10, 4096);
            data[32] = 13; memcpy(data + 33, "SLUS_014.11;1", 13);
        } else if (sector == 21) memcpy(data, "PS-X EXE", 8);
        assert(fwrite(raw, 1, sizeof(raw), file) == sizeof(raw));
    }
    assert(!fclose(file));
}

static int change_dir(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = Memories_Utf8ToWide(path);
    int result = wide ? _wchdir(wide) : -1;
    free(wide);
    return result;
#else
    return chdir(path);
#endif
}

int main(int argc, char **argv)
{
    char why[1024], saved[1024], buffer[1024], iso[1024];
    FILE *file;
    size_t length;
    unsigned char *exe;
    assert(argc == 2);
    assert(scratch_dir(root, sizeof(root), "memories-rom"));
    assert(!change_dir(root));
    assert(!unsetenv("MEMORIES_DISC"));
    assert(!unsetenv("MEMORIES_HEADLESS"));
    snprintf(valid, sizeof(valid), "%s/disc-\u6771\u4eac.rom", root); /* selected paths need no .bin suffix */
    Paths_User(saved, sizeof(saved), "disc-path.txt");
    make_disc(valid, 0);
    if (!strcmp(argv[1], "cancel")) {
        cancel = 1;
        assert(GameFiles_Setup(why, sizeof(why)) == 0 && picks == 1 && errors == 0);
        assert(access(saved, F_OK));
    } else if (!strcmp(argv[1], "headless")) {
        assert(!setenv("MEMORIES_HEADLESS", "1", 1));
        assert(GameFiles_Setup(why, sizeof(why)) == -1 && picks == 0);
    } else if (!strcmp(argv[1], "override")) {
        assert(!setenv("MEMORIES_DISC", "missing.bin", 1));
        assert(GameFiles_Setup(why, sizeof(why)) == -1 && picks == 0);
        assert(strstr(why, "MEMORIES_DISC"));
        assert(!setenv("MEMORIES_DISC", valid, 1));
        assert(GameFiles_Setup(why, sizeof(why)) == 1 && picks == 0);
    } else if (!strcmp(argv[1], "picker-failure")) {
        picker_failure = 1;
        assert(GameFiles_Setup(why, sizeof(why)) == -1 && picks == 1);
        assert(strstr(why, "unavailable"));
    } else if (!strcmp(argv[1], "malformed")) {
        make_disc(valid, 1);
        assert(GameFiles_SelectDisc(valid, why, sizeof(why)) == -1);
        assert(access(saved, F_OK));
    } else if (!strcmp(argv[1], "named")) {
        /* An image that renamed its executable: the disc is found through
         * the name in its own SYSTEM.CNF, and its executable comes out. */
        snprintf(iso, sizeof(iso), "%s/galaxy.bin", root);
        make_named_disc(iso);
        assert(GameFiles_Setup(why, sizeof(why)) == 1);
        assert(picks == 0);
        assert(!strcmp(GameFiles_Disc(why, sizeof(why)), iso));
        exe = GameFiles_ReadExecutable(iso, &length);
        assert(exe && length == 4096 && !memcmp(exe, "PS-X EXE", 8)); free(exe);
    } else if (!strcmp(argv[1], "write-failure")) {
        assert(!mkdir(saved, 0700));
        assert(GameFiles_SelectDisc(valid, why, sizeof(why)) == -1);
        assert(strstr(why, "Could not save"));
        assert(!rmdir(saved));
    } else {
        if (!strcmp(argv[1], "remembered") || !strcmp(argv[1], "moved")) {
            file = fopen(saved, "wb"); assert(file);
            assert(fputs(!strcmp(argv[1], "moved") ? "missing.bin" : valid, file) >= 0);
            assert(!fclose(file));
        }
        retry = !strcmp(argv[1], "retry");
        assert(GameFiles_Setup(why, sizeof(why)) == 1);
        assert(picks == (!strcmp(argv[1], "remembered") ? 0 : retry ? 2 : 1));
        assert(errors == retry);
        assert(!strcmp(GameFiles_Disc(why, sizeof(why)), valid));
        file = fopen(saved, "rb"); assert(file);
        length = fread(buffer, 1, sizeof(buffer) - 1, file); buffer[length] = 0;
        assert(!fclose(file)); assert(!strcmp(buffer, valid));
        exe = GameFiles_ReadExecutable(valid, &length);
        assert(exe && length == 4096 && !memcmp(exe, "PS-X EXE", 8)); free(exe);
    }
    /* scratch_dir removes the folder; a scanner holding the image must not fail the test */
    assert(!change_dir(".."));
    return 0;
}
