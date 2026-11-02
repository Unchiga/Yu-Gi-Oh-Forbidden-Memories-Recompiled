/* The mods folder the player chose (Mods_Folder, Mods_SetFolder; notes/modding.md,
 * "Your own mods folder"): one more folder the game reads, between the
 * release's mods/ and the user directory's, with the later one winning on
 * one id. Mods_Load scans once a process, so each case is a run of its own,
 * named on the command line:
 *
 *   file        how mods-folder.txt is read and written, MEMORIES_MODS_FOLDER
 *   precedence  the same id in all three places, a folder only the player's has
 *   missing     a chosen folder that is gone, or is a file: left out, with the reason
 *   env         MEMORIES_MODS_FOLDER over the file, and set to nothing
 *   owned       the game's own folders named another way, by hand: not read again
 *   exclusive   MEMORIES_MODS_DIR set: the only folder read
 */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/mods/import.h"
#include "zip_writer.h"

/* The three places, and the player's folder beside them. */
static void layout(void)
{
    char path[1024];
    make_dir("prog");
    make_dir("prog/mods");
    make_dir("user");
    make_dir("user/mods");
    make_dir("chosen");
    snprintf(path, sizeof(path), "%s/prog", root);
    assert(!setenv("MEMORIES_PROGRAM_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user", root);
    assert(!setenv("MEMORIES_USER_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    unsetenv("MEMORIES_MODS_DIR");
    unsetenv("MEMORIES_MODS_FOLDER");
    unsetenv("MEMORIES_MODS");
}

/* A mod folder `place` (relative to the scratch root) holding `manifest`. */
static void mod_at(const char *place, const char *manifest)
{
    char file[1024];
    make_dir(place);
    snprintf(file, sizeof(file), "%s/mod.json", place);
    write_text(file, manifest);
}

static int in(int mod, const char *place)
{
    char wanted[1024];
    snprintf(wanted, sizeof(wanted), "%s/%s", root, place);
    return !strcmp(Mods_Directory(mod), wanted);
}

/* How many files and folders are under `place` (relative to the root), to see that none came. */
static int tree_count(const char *place)
{
    char path[1024];
    DIR *directory;
    struct dirent *entry;
    int count = 0;
    snprintf(path, sizeof(path), "%s/%s", root, place);
    if (!(directory = opendir(path))) return 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", place, entry->d_name);
        count += 1 + tree_count(path);
    }
    closedir(directory);
    return count;
}

static void choose(const char *place)
{
    char path[1024], why[400];
    snprintf(path, sizeof(path), "%s/%s", root, place);
    if (!Mods_SetFolder(path, why, sizeof(why))) return;
    fprintf(stderr, "Mods_SetFolder: %s\n", why);
    assert(0);
}

static void load(void)
{
    Settings_Load();
    Mods_Load();
}

static void file_case(void)
{
    char out[1024], why[400], path[1024], expected[1024];
    /* No file and no environment: none. */
    assert(Mods_Folder(out, sizeof(out)) == -1);
    /* A path, round trip: written as a line, read back. */
    snprintf(path, sizeof(path), "%s/chosen", root);
    assert(!Mods_SetFolder(path, why, sizeof(why)));
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    /* Only a temporary of the write is never left, and the name is the one the tools read. */
    snprintf(expected, sizeof(expected), "%s/user/mods-folder.txt", root);
    assert(!access(expected, F_OK));
    snprintf(expected, sizeof(expected), "%s/user/mods-folder.txt.tmp", root);
    assert(access(expected, F_OK));
    /* A trailing separator and spaces are not part of the name. */
    snprintf(expected, sizeof(expected), "  %s/chosen/  ", root);
    assert(!Mods_SetFolder(expected, why, sizeof(why)));
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    /* A folder that is not there yet is no reason to refuse it: the permission
     * for a folder may only be asked for after it is chosen. */
    snprintf(expected, sizeof(expected), "%s/not-yet", root);
    assert(!Mods_SetFolder(expected, why, sizeof(why)) && !Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    /* The folders the game reads already are refused, and the choice stays. */
    snprintf(path, sizeof(path), "%s/user/mods", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1 && strstr(why, "reads or writes already"));
    snprintf(path, sizeof(path), "%s/user/mods/", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/user/mods/inside/deeper", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/prog/mods", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    assert(Mods_SetFolder("mods", why, sizeof(why)) == -1);   /* from the program directory: the same */
    /* The same folders spelled another way: ".", "..", a doubled separator, a relative
     * path through the program directory, a quoted one. */
    snprintf(path, sizeof(path), "%s/prog/./mods", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/prog/other/../mods//", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/user/mods/../mods/x/..", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    assert(Mods_SetFolder("./mods", why, sizeof(why)) == -1 && Mods_SetFolder("x/../mods", why, sizeof(why)) == -1);
    assert(Mods_SetFolder("../prog/mods", why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "\"%s/prog/mods\"", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
#ifndef _WIN32
    /* A link to one of them, and a link inside the user directory. */
    snprintf(path, sizeof(path), "%s/prog/mods", root);
    snprintf(expected, sizeof(expected), "%s/alias-release", root);
    assert(!symlink(path, expected));
    assert(Mods_SetFolder(expected, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/user", root);
    snprintf(expected, sizeof(expected), "%s/alias-user", root);
    assert(!symlink(path, expected));
    assert(Mods_SetFolder(expected, why, sizeof(why)) == -1);
    snprintf(expected, sizeof(expected), "%s/alias-user/mods", root);
    assert(Mods_SetFolder(expected, why, sizeof(why)) == -1);
#endif
    /* The user directory itself and what the game keeps in it besides mods/ is
     * the game's to write: no. */
    snprintf(path, sizeof(path), "%s/user", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/user/downloads", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    snprintf(path, sizeof(path), "%s/user/mods-extra", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    /* Nothing was changed by the refusals. */
    snprintf(expected, sizeof(expected), "%s/not-yet", root);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    /* Siblings of them, that only start alike, are other folders. */
    snprintf(path, sizeof(path), "%s/prog/mods2", root);
    assert(!Mods_SetFolder(path, why, sizeof(why)) && !Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    snprintf(path, sizeof(path), "%s/user-extra", root);
    assert(!Mods_SetFolder(path, why, sizeof(why)) && !Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    assert(!Mods_SetFolder("mods2", why, sizeof(why)));   /* from the program directory */
    snprintf(expected, sizeof(expected), "%s/prog/mods2", root);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    /* Double quotes, as Explorer's "Copy as path" gives, are not part of the name. */
    snprintf(path, sizeof(path), "%s/chosen", root);
    snprintf(expected, sizeof(expected), " \"%s/chosen\" ", root);
    assert(!Mods_SetFolder(expected, why, sizeof(why)) && !Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    /* A line break inside a name is no folder. */
    snprintf(expected, sizeof(expected), "%s/chosen\nsecond", root);
    assert(Mods_SetFolder(expected, why, sizeof(why)) == -1 && strstr(why, "line break"));
    snprintf(expected, sizeof(expected), "%s/chosen\r%s/second", root, root);
    assert(Mods_SetFolder(expected, why, sizeof(why)) == -1);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, path));
    /* MEMORIES_MODS_DIR stands for the user's folder. */
    snprintf(path, sizeof(path), "%s/named", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);
    /* Saved all the same, but it says that MEMORIES_MODS_DIR overrides it. */
    snprintf(expected, sizeof(expected), "%s/chosen", root);
    assert(!Mods_SetFolder(expected, why, sizeof(why)) && !strncmp(why, "Saved, but MEMORIES_MODS_DIR", 28));
    assert(!Mods_SetFolder("", why, sizeof(why)) && !strncmp(why, "Cleared, but MEMORIES_MODS_DIR", 30));
    assert(Mods_Folder(out, sizeof(out)) == -1);
    unsetenv("MEMORIES_MODS_DIR");
    /* And MEMORIES_MODS_FOLDER: the file is written, the variable is what is used. */
    assert(!setenv("MEMORIES_MODS_FOLDER", "/from/env", 1));
    assert(!Mods_SetFolder(expected, why, sizeof(why)) && !strncmp(why, "Saved, but MEMORIES_MODS_FOLDER", 31));
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, "/from/env"));
    assert(!Mods_SetFolder("", why, sizeof(why)) && !strncmp(why, "Cleared, but MEMORIES_MODS_FOLDER", 33));
    assert(!Mods_SetFolder(expected, why, sizeof(why)) && why[0]);
    unsetenv("MEMORIES_MODS_FOLDER");
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    /* Nothing overrides it: no note. */
    assert(!Mods_SetFolder("", why, sizeof(why)) && !why[0] && Mods_Folder(out, sizeof(out)) == -1);
    /* A drive's current folder is not a full path (Windows' "C:" and "C:foo"); neither is a
     * name that cannot be made one. */
    assert(Mods_SetFolder("C:", why, sizeof(why)) == -1 && strstr(why, "full path, such as C:"));
    assert(Mods_SetFolder("C:foo", why, sizeof(why)) == -1 && strstr(why, "full path"));
    assert(Mods_SetFolder("d:mods/pack", why, sizeof(why)) == -1);
    /* Not a name: nothing written. */
    assert(Mods_SetFolder(NULL, why, sizeof(why)) == 0 && Mods_Folder(out, sizeof(out)) == -1);
    assert(Mods_SetFolder("   ", why, sizeof(why)) == 0 && Mods_Folder(out, sizeof(out)) == -1);
    snprintf(path, sizeof(path), "%s/chosen", root);
    assert(!Mods_SetFolder(path, why, sizeof(why)) && !Mods_Folder(out, sizeof(out)));
    assert(!Mods_SetFolder(NULL, why, sizeof(why)));   /* clearing twice is no error */
    assert(Mods_Folder(out, sizeof(out)) == -1);
    {
        char tall[2048];
        memset(tall, 'x', sizeof(tall) - 1);
        tall[0] = '/';
        tall[sizeof(tall) - 1] = '\0';
        assert(Mods_SetFolder(tall, why, sizeof(why)) == -1 && strstr(why, "too long"));
        /* Short enough alone, too long from the program directory: still "too long", not "own folder". */
        memset(tall, 'x', 1000);
        tall[1000] = '\0';
        assert(Mods_SetFolder(tall, why, sizeof(why)) == -1 && strstr(why, "too long"));
    }

    /* What a hand-written file may hold: a byte order mark, a CRLF, a blank
     * line first, a trailing slash. Only the first line that says anything counts. */
    snprintf(expected, sizeof(expected), "%s/chosen", root);
    snprintf(path, sizeof(path), "\xEF\xBB\xBF\r\n  \r\n%s/chosen/\r\n%s/second\r\n", root, root);
    write_text("user/mods-folder.txt", path);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    write_text("user/mods-folder.txt", "");
    assert(Mods_Folder(out, sizeof(out)) == -1);
    write_text("user/mods-folder.txt", "\r\n\r\n");
    assert(Mods_Folder(out, sizeof(out)) == -1);
    /* A relative path is from the program directory (a portable pack's). */
    write_text("user/mods-folder.txt", "mods-extra\n");
    snprintf(expected, sizeof(expected), "%s/prog/mods-extra", root);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    /* One that does not fit is no folder. */
    assert(Mods_Folder(out, 8) == -1);
#ifdef _WIN32
    write_text("user/mods-folder.txt", "C:\\Mods\\Pack\\\r\n");
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, "C:/Mods/Pack"));
    write_text("user/mods-folder.txt", "c:\\");
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, "c:/"));
    snprintf(path, sizeof(path), "%s\\USER\\Mods", root);
    assert(Mods_SetFolder(path, why, sizeof(why)) == -1);   /* the file system does not tell case */
#endif

    /* The environment wins over the file; set to nothing it says none. */
    write_text("user/mods-folder.txt", "from-file\n");
    assert(!setenv("MEMORIES_MODS_FOLDER", "/from/env/", 1));
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, "/from/env"));
#ifndef _WIN32   /* there, setting a variable to nothing removes it */
    assert(!setenv("MEMORIES_MODS_FOLDER", "", 1));
    assert(Mods_Folder(out, sizeof(out)) == -1);
#endif
    unsetenv("MEMORIES_MODS_FOLDER");
    snprintf(expected, sizeof(expected), "%s/prog/from-file", root);
    assert(!Mods_Folder(out, sizeof(out)) && !strcmp(out, expected));
    assert(!*Mods_FolderProblem());   /* before Mods_Load */
}

static void precedence_case(void)
{
    char out[1024], why[400], install[1024];
    int same, middle, shipped, only, twice, mine, before;
    mod_at("prog/mods/same", "{\"id\":\"same\",\"name\":\"shipped\"}");
    mod_at("prog/mods/middle", "{\"id\":\"middle\",\"name\":\"shipped\",\"enabled\":true}");
    mod_at("prog/mods/shipped-only", "{\"id\":\"shipped-only\"}");
    mod_at("chosen/same", "{\"id\":\"same\",\"name\":\"chosen\"}");
    mod_at("chosen/middle", "{\"id\":\"middle\",\"name\":\"chosen\"}");
    mod_at("chosen/folder-only", "{\"id\":\"folder-only\"}");
    /* The same id twice in the chosen folder: the first, in the sorted order. */
    mod_at("chosen/twice-b", "{\"id\":\"twice\",\"name\":\"b\"}");
    mod_at("chosen/twice-a", "{\"id\":\"twice\",\"name\":\"a\"}");
    mod_at("chosen/.hidden", "{\"id\":\"hidden\"}");
    make_dir("chosen/not-a-mod");
    mod_at("user/mods/same", "{\"id\":\"same\",\"name\":\"user\"}");
    mod_at("user/mods/mine", "{\"id\":\"mine\"}");
    choose("chosen");
    before = tree_count("chosen");
    load();
    assert(!*Mods_FolderProblem());
    same = find("same");
    middle = find("middle");
    shipped = find("shipped-only");
    only = find("folder-only");
    twice = find("twice");
    mine = find("mine");
    assert(same >= 0 && middle >= 0 && shipped >= 0 && only >= 0 && twice >= 0 && mine >= 0);
    assert(find("hidden") < 0 && find("not-a-mod") < 0);
    assert(Mods_Count() == 6);
    /* The user directory's copy is last and wins, over both; the player's
     * folder wins over the release's; what only one place has is as it is. */
    assert(in(same, "user/mods/same") && !strcmp(Mods_Origin(same), "installed"));
    assert(!strcmp(Mods_Metadata(same, "name"), "user"));
    assert(in(middle, "chosen/middle") && !strcmp(Mods_Origin(middle), "folder"));
    assert(!strcmp(Mods_Metadata(middle, "name"), "chosen"));
    assert(in(shipped, "prog/mods/shipped-only") && !strcmp(Mods_Origin(shipped), "shipped"));
    assert(in(only, "chosen/folder-only") && !strcmp(Mods_Origin(only), "folder"));
    assert(in(twice, "chosen/twice-a") && !strcmp(Mods_Origin(twice), "folder"));
    assert(in(mine, "user/mods/mine") && !strcmp(Mods_Origin(mine), "installed"));
    /* The copy that replaced the release's keeps whether that was on. */
    assert(Mods_Enabled(middle));
    /* The game never deletes from the player's folder: Remove would be
     * refused whatever asked, and the mod stays where it is. */
    assert(!Mods_InstallDirectory(install, sizeof(install)));
    assert(Mods_ImportRemoveInstalled(install, Mods_Directory(middle), why, sizeof(why)) != 0);
    assert(Mods_ImportRemoveInstalled(install, Mods_Directory(only), why, sizeof(why)) != 0);
    snprintf(out, sizeof(out), "%s/chosen/middle/mod.json", root);
    assert(!access(out, F_OK));
    snprintf(out, sizeof(out), "%s/chosen/folder-only/mod.json", root);
    assert(!access(out, F_OK));
    /* The install folder is the user directory's, not the chosen one. */
    snprintf(out, sizeof(out), "%s/user/mods", root);
    assert(!strcmp(install, out));
    /* An import of an id the chosen folder has (Mods_ImportInstall, as the Android
     * panel's Import mod... and the HD pack run it) lands in the user directory's
     * mods folder and takes the slot: Mods_Discover reads the new folder as installed. */
    {
        ZipItem items[] = {{"folder-only/", NULL, 0, 040755, 0, 0, 0, 0},
                           {"folder-only/mod.json", "{\"id\":\"folder-only\",\"name\":\"imported\"}", 0, 0100644, 0, 0, 0, 0}};
        ModsImport *import;
        int later = 0;
        snprintf(out, sizeof(out), "%s/picked.zip", root);
        write_zip(out, items, 2);
        import = Mods_ImportOpen(out, why, sizeof(why));
        assert(import && Mods_ImportCount(import) == 1);
        assert(!strcmp(Mods_ImportMod(import, 0)->id, "folder-only") && !Mods_ImportMod(import, 0)->replace[0]);
        assert(Mods_ImportInstall(import, install, why, sizeof(why)));
        Mods_ImportClose(import);
        snprintf(out, sizeof(out), "%s/user/mods/folder-only", root);
        assert(Mods_Discover(out, &later) == only && !later);
        assert(in(only, "user/mods/folder-only") && !strcmp(Mods_Origin(only), "installed"));
        assert(!strcmp(Mods_Metadata(only, "name"), "imported"));
    }
    /* Nothing was written into the chosen folder, by the scan or the import. */
    assert(tree_count("chosen") == before);
}

static void missing_case(const char *place, int file)
{
    mod_at("prog/mods/shipped-only", "{\"id\":\"shipped-only\"}");
    mod_at("user/mods/mine", "{\"id\":\"mine\"}");
    if (file) write_text(place, "not a folder");
    choose(place);
    load();
    /* Left out, the others are as they were, and the reason is there to show. */
    assert(Mods_Count() == 2 && find("shipped-only") >= 0 && find("mine") >= 0);
    assert(*Mods_FolderProblem() && strstr(Mods_FolderProblem(), place));
    assert(file ? strstr(Mods_FolderProblem(), "not a folder") || strstr(Mods_FolderProblem(), "could not be read")
                : strstr(Mods_FolderProblem(), "does not exist") != NULL);
    {
        char out[1024];
        assert(!Mods_Folder(out, sizeof(out))); /* still chosen: it may come back */
    }
}

/* A hand-written mods-folder.txt that names the release's mods/ another way: the
 * game's own folder is not scanned again as the player's (origin "folder"). */
static void owned_case(const char *spelling)
{
    char line[1100], expected[1400];
    mod_at("prog/mods/shipped-only", "{\"id\":\"shipped-only\"}");
    snprintf(line, sizeof(line), "%s/%s\n", root, spelling);
    write_text("user/mods-folder.txt", line);
    load();
    assert(Mods_Count() == 1 && !strcmp(Mods_Origin(find("shipped-only")), "shipped"));
    /* The path as the file spells it (a trailing separator is not part of it). */
    snprintf(line, sizeof(line), "%s/%s", root, spelling);
    if (line[strlen(line) - 1] == '/') line[strlen(line) - 1] = '\0';
    snprintf(expected, sizeof(expected), "%s is one of the game's own folders, which it reads or writes already.", line);
    assert(!strcmp(Mods_FolderProblem(), expected));
}

static void env_case(int empty)
{
#ifdef _WIN32
    if (empty) {   /* a variable set to nothing is removed on Windows: there is no such state to test */
        puts("envempty: skipped on Windows");
        exit(77);   /* CTest's skip code, set on the test in CMakeLists.txt */
    }
#endif
    mod_at("chosen/from-file", "{\"id\":\"from-file\"}");
    make_dir("other");
    mod_at("other/from-env", "{\"id\":\"from-env\"}");
    choose("chosen");
    if (empty) {
        assert(!setenv("MEMORIES_MODS_FOLDER", "", 1));
    } else {
        char path[1024];
        snprintf(path, sizeof(path), "%s/other", root);
        assert(!setenv("MEMORIES_MODS_FOLDER", path, 1));
    }
    load();
    assert(!*Mods_FolderProblem());
    if (empty) {
        assert(Mods_Count() == 0);
    } else {
        assert(Mods_Count() == 1 && find("from-env") >= 0 && !strcmp(Mods_Origin(find("from-env")), "folder"));
    }
}

static void exclusive_case(void)
{
    char path[1024];
    mod_at("prog/mods/shipped-only", "{\"id\":\"shipped-only\"}");
    mod_at("user/mods/mine", "{\"id\":\"mine\"}");
    mod_at("chosen/folder-only", "{\"id\":\"folder-only\"}");
    make_dir("named");
    mod_at("named/named-only", "{\"id\":\"named-only\"}");
    choose("chosen");
    snprintf(path, sizeof(path), "%s/named", root);
    assert(!setenv("MEMORIES_MODS_DIR", path, 1));
    load();
    assert(Mods_Count() == 1 && find("named-only") >= 0 && !strcmp(Mods_Origin(find("named-only")), "installed"));
    assert(strstr(Mods_FolderProblem(), "MEMORIES_MODS_DIR is set"));
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    assert(scratch_dir(root, sizeof(root), "memories-modfolder"));
#ifdef _WIN32
    for (char *c = root; *c; c++)   /* Mods_Folder gives a path with slashes; so must the paths compared to it */
        if (*c == '\\') *c = '/';
#endif
    layout();
    if (!strcmp(what, "file")) file_case();
    else if (!strcmp(what, "precedence")) precedence_case();
    else if (!strcmp(what, "missing")) missing_case("gone", 0);
    else if (!strcmp(what, "notfolder")) missing_case("a-file", 1);
    else if (!strcmp(what, "owned")) owned_case("prog/./mods");
    else if (!strcmp(what, "owned-up")) owned_case("prog/x/../mods/");
    else if (!strcmp(what, "owned-user")) owned_case("user/downloads");
    else if (!strcmp(what, "env")) env_case(0);
    else if (!strcmp(what, "envempty")) env_case(1);
    else if (!strcmp(what, "exclusive")) exclusive_case();
    else {
        fprintf(stderr, "mods_folder_test: unknown case \"%s\"\n", what);
        return 2;
    }
    puts("mods folder: ok");
    return 0;
}
