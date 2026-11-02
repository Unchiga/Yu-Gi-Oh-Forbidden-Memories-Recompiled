/* Real loader/settings, fake font and restart: verify staged UI behavior. */
#define main original_mods_test
#include "pc/compat/fs.h"
#include "mods_test.c"
#undef main
#include "pc/platform/mods_window.h"
#include "pc/mods/overlap.h"
#include "pc/mods/hd_pack.h"
#include "zip_writer.h"
#ifndef _WIN32
#include <dirent.h>
#endif
static int restarts;
static int test_scale = 1; /* Menu_Scale: the phone's density */
int Menu_Scale(void) { return test_scale; }
static int test_touch; /* Menu_TouchTarget: 0 with a mouse */
int Menu_TouchTarget(void) { return test_touch; }
int Menu_TextWidthScaled(const char *s, int scale) { return (int)strlen(s) * 7 * scale; }
/* What a draw wrote, while `capture` is on: each string, its color and
 * the middle of its line (in the canvas it was drawn on). */
static int capture;
static char drawn[16384];
static int drawn_y(const char *part)
{
    const char *at = strstr(drawn, part), *line;
    if (!at) return -1;
    for (line = at; line > drawn && line[-1] != '\n'; line--) {
    }
    return atoi(line);
}
void Menu_DrawTextScaled(MenuCanvas *c, int x, int y, const char *s, uint32_t color, int scale)
{
    (void)c;
    (void)x;
    (void)scale;
    if (capture && strlen(drawn) + strlen(s) + 32 < sizeof(drawn))
        snprintf(drawn + strlen(drawn), sizeof(drawn) - strlen(drawn), "%d %06x %s\n", y, (unsigned)color, s);
}
static void draw(int w, int h)
{
    MenuCanvas canvas = {0};
    canvas.width = canvas.stride = w;
    canvas.height = h;
    canvas.pixels = calloc((size_t)w * h, 4);
    assert(canvas.pixels);
    drawn[0] = 0;
    capture = 1;
    ModsWindow_Draw(&canvas);
    capture = 0;
    free(canvas.pixels);
}
static char opened[1024];
int Platform_OpenFolder(const char *path)
{
    snprintf(opened, sizeof(opened), "%s", path);
    return 0;
}
int Platform_RestartGame(void)
{
    restarts++;
    return -1;
}
static int input(MenuEventType type, int x, int y, MenuKey key, const char *text)
{
    MenuEvent e = {0};
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = 1;
    e.key = key;
    if (text)
        snprintf(e.text, sizeof(e.text), "%s", text);
    return ModsWindow_Event(&e);
}
static int click(int x, int y) { return input(MENU_EVENT_BUTTON_DOWN, x, y, MENU_KEY_OTHER, NULL); }
/* Import mod... with a file picker that answers at once with the .zip
 * the test wrote (ModsWindow_SetImport), and what the window then says. */
static char import_source[1024];
static int picks;
static int fake_pick(char *why, size_t size)
{
    (void)why;
    (void)size;
    picks++;
    return 0;
}
static int fake_picked(char *why, size_t size)
{
    (void)why;
    (void)size;
    return 1;
}
static int fake_fetch(char *path, size_t size, char *why, size_t why_size)
{
    (void)why;
    (void)why_size;
    snprintf(path, size, "%s", import_source);
    return 1;
}
static void tap_widget(int id)
{
    int x, y;
    assert(ModsWindow_Locate(id, &x, &y));
    click(x, y);
    input(MENU_EVENT_BUTTON_UP, x, y, MENU_KEY_OTHER, NULL);
}
/* Writes `items` as the picked .zip, taps Import mod... and runs the ticks
 * (the picker's answer, then the import); what the footer says. */
static const char *import_zip(const ZipItem *items, int n)
{
    static char said[4096];
    const char *line;
    write_zip(import_source, items, n);
    tap_widget(MODS_UI_FOLDER);
    while (ModsWindow_Tick()) {
    }
    draw(1600, 900);
    said[0] = 0;
    for (line = drawn; *line; line = strchr(line, '\n') + 1) {
        const char *text = strchr(strchr(line, ' ') + 1, ' ') + 1;
        size_t length = (size_t)(strchr(line, '\n') - text);
        if (strlen(said) + length + 2 < sizeof(said)) {
            strncat(said, text, length);
            strcat(said, " ");
        }
    }
    return said;
}
static int folder_has(const char *relative)
{
    char path[2048];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    return !stat(path, &info);
}
/* Mods folder... with a folder picker that answers as the test says
 * (ModsWindow_SetFolderPicker), and what the window then shows. */
static char folder_answer[1024], folder_failure[200];
static int folder_result, folder_opens, folder_waits;
static int fake_folder_pick(char *why, size_t size)
{
    (void)why;
    (void)size;
    folder_opens++;
    return 0;
}
static int fake_folder_picked(char *path, size_t size, char *why, size_t why_size)
{
    if (folder_waits-- > 0)
        return 0;
    snprintf(path, size, "%s", folder_answer);
    snprintf(why, why_size, "%s", folder_failure);
    return folder_result;
}
/* Everything the window drew, as one line of text. */
static const char *on_screen(int w, int h)
{
    static char said[4096];
    const char *line;
    draw(w, h);
    said[0] = 0;
    for (line = drawn; *line; line = strchr(line, '\n') + 1) {
        const char *text = strchr(strchr(line, ' ') + 1, ' ') + 1;
        size_t length = (size_t)(strchr(line, '\n') - text);
        if (strlen(said) + length + 2 < sizeof(said)) {
            strncat(said, text, length);
            strcat(said, " ");
        }
    }
    if (getenv("MODS_TEST_SCREEN"))
        fprintf(stderr, "screen: %s\n", said);
    return said;
}
/* Mods folder... is pressed and answered: the folder picker is asked, and
 * its answer (folder_result, folder_answer) is taken at the ticks. */
static void choose_folder(int result, const char *path)
{
    folder_result = result;
    snprintf(folder_answer, sizeof(folder_answer), "%s", path ? path : "");
    folder_waits = 2;
    tap_widget(MODS_UI_MODSDIR);
    tap_widget(MODS_UI_APPLY); /* Choose folder... */
    for (int i = 0; i < 5; i++)
        ModsWindow_Tick();
}
/* MEMORIES_MODS_DIR set (as in this whole run): the choice is saved all the
 * same, and the window says it is not read; no restart helps it. */
static void test_folder_override(void)
{
    char other[1024];
    int x, y, opens = folder_opens, before = restarts;
    const char *said;
    /* a folder outside the user directory, which is root here */
    assert(scratch_dir(other, sizeof(other), "memories-mod-window-elsewhere"));
    ModsWindow_SetFolderPicker(NULL, NULL);
    ModsWindow_Init();
    ModsWindow_Resize(920, 640);
    assert(!ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y)); /* no picker: no button */
    ModsWindow_SetFolderPicker(fake_folder_pick, fake_folder_picked);
    ModsWindow_Init();
    assert(ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y));
    choose_folder(1, other);
    said = on_screen(920, 640);
    assert(folder_opens == opens + 1);
    assert(strstr(said, "Mods folder: ") && strstr(said, "elsewhere") && strstr(said, "MEMORIES_MODS_DIR"));
    assert(!strstr(said, "after a restart"));
    /* ... so Apply has nothing to restart for. */
    tap_widget(MODS_UI_APPLY);
    assert(strstr(on_screen(920, 640), "No pending changes") && restarts == before);
    /* Use no folder. */
    tap_widget(MODS_UI_MODSDIR);
    said = on_screen(920, 640);
    assert(strstr(said, "Mods folder: ") && strstr(said, "elsewhere") && strstr(said, "use none?"));
    tap_widget(MODS_UI_MODSDIR);
    assert(strstr(on_screen(920, 640), "Mods folder: none."));
    ModsWindow_SetFolderPicker(NULL, NULL);
}
/* The panel on a phone or tablet (touch), as an Android app has it. */
static void test_import(void)
{
    static const ZipItem good[] = {{"Downloads/newmod/mod.json", "{\"id\": \"newmod\", \"name\": \"New mod\", "
                                                                "\"enabled\": true}", 1, 0, 0, 0, 0, 0},
                                   {"Downloads/newmod/name.txt", "hello", 1, 0, 0, 0, 0, 0}};
    static const ZipItem pc[] = {{"pcmod/mod.json", "{\"id\": \"pcmod\", \"name\": \"PC mod\", \"library\": "
                                                    "\"pcmod\"}", 0, 0, 0, 0, 0, 0},
                                 {"pcmod/pcmod.o", "ELF", 0, 0, 0, 0, 0, 0}};
    static const ZipItem arm[] = {{"armmod/mod.json", "{\"id\": \"armmod\", \"name\": \"Arm mod\", \"library\": "
                                                      "\"armmod\"}", 0, 0, 0, 0, 0, 0},
                                  {"armmod/armmod.o", "ELF", 0, 0, 0, 0, 0, 0},
                                  {"armmod/armmod.aarch64.o", "ELF", 0, 0, 0, 0, 0, 0}};
    static const ZipItem perm[] = {{"permod/mod.json", "{\"id\": \"permod\", \"name\": \"Per-target mod\", "
                                                       "\"libraries\": {\"aarch64\": \"d.o\"}}", 0, 0, 0, 0, 0, 0}};
    static const ZipItem two[] = {{"m1/mod.json", "{\"name\": \"First\"}", 0, 0, 0, 0, 0, 0},
                                  {"m2/mod.json", "{\"name\": \"Second\"}", 0, 0, 0, 0, 0, 0}};
    static const ZipItem none[] = {{"readme.txt", "no mod here", 0, 0, 0, 0, 0, 0}};
    static const ZipItem slip[] = {{"newmod/../../escaped.txt", "x", 0, 0, 0, 0, 0, 0},
                                   {"newmod/mod.json", "{}", 0, 0, 0, 0, 0, 0}};
    int count = Mods_Count(), x, y, mod = -1;
    const char *said;
    snprintf(import_source, sizeof(import_source), "%s/picked.zip", root);
    test_touch = 48;
    ModsWindow_SetImport(NULL, NULL, NULL);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(!ModsWindow_Locate(MODS_UI_FOLDER, &x, &y)); /* no picker: no button */
    ModsWindow_SetImport(fake_pick, fake_picked, fake_fetch);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    said = import_zip(good, 2);
    assert(picks == 1 && strstr(said, "Imported New mod."));
    assert(Mods_Count() == count + 1 && (mod = find("newmod")) == count);
    assert(!Mods_Enabled(mod) && Settings_GetNamed("mod.newmod", 1) == 0); /* off, and off at the next launch */
    assert(folder_has("mods/newmod/name.txt") && !folder_has("picked.zip"));
    /* The same again: Replace or Cancel. */
    said = import_zip(good, 2);
    assert(strstr(said, "New mod is already installed. Replace it") && strstr(said, "Replace"));
    tap_widget(MODS_UI_CLOSE); /* Cancel */
    assert(Mods_Count() == count + 1 && folder_has("mods/newmod/name.txt") && !folder_has("picked.zip"));
    said = import_zip(good, 2);
    tap_widget(MODS_UI_APPLY); /* Replace */
    draw(1600, 900);
    assert(strstr(drawn, "Replaced New mod.") && Mods_Count() == count + 1);
    /* Code: imported, off; where the game has no code mod loader
     * (MEMORIES_NO_CODE_MODS, which no game build sets now) said so whatever
     * objects it has, an arm64 one or not; with the loader, nothing to say.
     * "libraries" alone is code too, as the loader reads it. */
    said = import_zip(pc, 2);
#ifdef MEMORIES_NO_CODE_MODS
    assert(strstr(said, "Imported PC mod. This mod has code, which the game cannot run in this build yet. "
                        "It stays off and changes nothing in the game."));
#else
    assert(strstr(said, "Imported PC mod.") && !strstr(said, "cannot run"));
#endif
    assert(!Mods_Enabled(find("pcmod")));
    said = import_zip(arm, 3);
#ifdef MEMORIES_NO_CODE_MODS
    assert(strstr(said, "Imported Arm mod. This mod has code, which the game cannot run in this build yet."));
#else
    assert(strstr(said, "Imported Arm mod.") && !strstr(said, "cannot run"));
#endif
    said = import_zip(perm, 1);
#ifdef MEMORIES_NO_CODE_MODS
    assert(strstr(said, "Imported Per-target mod. This mod has code, which the game cannot run in this build yet."));
#else
    assert(strstr(said, "Imported Per-target mod.") && !strstr(said, "cannot run"));
#endif
    assert(!Mods_Enabled(find("permod")));
    /* Several at once; none at all; a name that leaves the folder. */
    said = import_zip(two, 2);
    assert(strstr(said, "Imported 2 mods: First and Second.") && find("m1") >= 0 && find("m2") >= 0);
    /* One of them in place: its files are this launch's, so no Replace. */
    Mods_SetEnabled(find("m1"), 1);
    assert(Mods_Active(find("m1")));
    count = Mods_Count();
    said = import_zip(two, 2);
    assert(strstr(said, "First is in use, so its files cannot be replaced now.") && Mods_Count() == count);
    assert(folder_has("mods/m1/mod.json") && !folder_has("picked.zip"));
    /* A folder of that name with another mod in it: never replaced, the new
     * one gets a free name. */
    {
        static const ZipItem other[] = {{"mod05/mod.json", "{\"id\": \"other\", \"name\": \"Other\"}", 0, 0, 0, 0, 0, 0}};
        said = import_zip(other, 1);
        assert(strstr(said, "Imported Other.") && find("other") >= 0 && find("mod05") >= 0);
        assert(folder_has("mods/mod05-2/mod.json") && !strcmp(Mods_Id(find("mod05")), "mod05"));
        assert(strstr(Mods_Directory(find("other")), "mod05-2"));
    }
    {
        /* The free name is not another mod's of the same .zip: mod06 is
         * taken by another mod, and mod06-2 is the second mod's own. */
        static const ZipItem pair[] = {{"mod06/mod.json", "{\"id\": \"first6\", \"name\": \"First6\"}", 0, 0, 0, 0, 0, 0},
                                       {"Mod06-2/mod.json", "{\"id\": \"second6\", \"name\": \"Second6\"}", 0, 0, 0, 0, 0, 0}};
        said = import_zip(pair, 2);
        assert(strstr(said, "Imported 2 mods: ") && strstr(said, "First6") && strstr(said, "Second6"));
        assert(strstr(Mods_Directory(find("first6")), "mod06-3") && strstr(Mods_Directory(find("second6")), "Mod06-2"));
    }
    said = import_zip(none, 1);
    assert(strstr(said, "This .zip has no mod in it."));
    count = Mods_Count();
    said = import_zip(slip, 2);
    assert(strstr(said, "points outside its folder") && Mods_Count() == count);
    assert(!folder_has("escaped.txt") && !folder_has("mods/escaped.txt"));
    ModsWindow_SetImport(NULL, NULL, NULL);
    test_touch = 0;
}
/* Remove, beside Import mod... on a touch panel: a question, then the
 * mod's folder goes and the list drops it; a mod in use stays. */
static int listed(const char *name)
{
    char line[256];
    snprintf(line, sizeof(line), " e9edf3 %s\n", name);
    return strstr(drawn, line) != NULL;
}
static int installed_count(void)
{
    const char *at = strstr(drawn, " installed  /  ");
    if (!at)
        return -1;
    while (at > drawn && at[-1] != '\n' && at[-1] != ' ')
        at--;
    return atoi(at);
}
static void test_remove(void)
{
    static const ZipItem good[] = {{"rmnew/mod.json", "{\"id\": \"rmnew\", \"name\": \"Remove me\"}", 1, 0, 0, 0, 0, 0},
                                   {"rmnew/deep/file.txt", "hello", 1, 0, 0, 0, 0, 0}};
    static const ZipItem live[] = {{"rmlive/mod.json", "{\"id\": \"rmlive\", \"name\": \"Live one\"}", 1, 0, 0, 0, 0, 0}};
    int x, y, mod, other;
    const char *said;
    snprintf(import_source, sizeof(import_source), "%s/picked.zip", root);
    test_touch = 48;
    /* No picker, no Remove: the desktop's panel. */
    ModsWindow_SetImport(NULL, NULL, NULL);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(!ModsWindow_Locate(MODS_UI_REMOVE, &x, &y));
    draw(1600, 900);
    assert(!strstr(drawn, "Remove"));
    ModsWindow_SetImport(fake_pick, fake_picked, fake_fetch);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(ModsWindow_Locate(MODS_UI_REMOVE, &x, &y));
    /* An imported mod, off and never loaded: Remove asks, Cancel keeps it. */
    said = import_zip(good, 2);
    assert(strstr(said, "Imported Remove me."));
    mod = find("rmnew");
    assert(mod >= 0 && !Mods_InUse(mod) && folder_has("mods/rmnew/deep/file.txt"));
    draw(1600, 900);
    assert(listed("Remove me") && strstr(drawn, " Remove\n"));
    tap_widget(MODS_UI_REMOVE);
    draw(1600, 900);
    assert(strstr(drawn, "Remove Remove me? Its folder is deleted from the mods folder, with everything in it."));
    assert(strstr(drawn, "Cancel") && strstr(drawn, " Remove\n"));
    /* The answer is the Apply button, which reads Remove: no second, dead one. */
    assert(!ModsWindow_Locate(MODS_UI_REMOVE, &x, &y));
    tap_widget(MODS_UI_CLOSE); /* Cancel */
    assert(!Mods_Removed(mod) && folder_has("mods/rmnew/deep/file.txt"));
    assert(ModsWindow_Locate(MODS_UI_REMOVE, &x, &y)); /* and it is back */
    /* What is staged for the mod (its On, its load order) is unsaved changes... */
    tap_widget(MODS_UI_ORDER + 1);
    tap_widget(MODS_UI_TOGGLE);
    assert(ModsWindow_Locate(MODS_UI_CLOSE, &x, &y) && click(x, y) == 0);
    draw(1600, 900);
    assert(strstr(drawn, "Discard your unsaved mod changes?"));
    input(MENU_EVENT_KEY_DOWN, 0, 0, MENU_KEY_ESCAPE, NULL);
    /* ... until the mod is removed: then they go with it. */
    /* Yes: the folder is gone, the mod leaves the list, another is shown. */
    tap_widget(MODS_UI_REMOVE);
    tap_widget(MODS_UI_APPLY);
    draw(1600, 900);
    assert(strstr(drawn, "Removed Remove me.") && !listed("Remove me"));
    assert(!folder_has("mods/rmnew") && folder_has("mods") && Mods_Removed(mod));
    assert(!Mods_Enabled(mod) && Settings_GetNamed("mod.rmnew", 1) == 0);
    assert(Mods_Failed(mod)); /* inert: nothing can apply it */
    /* The count in the footer leaves it out. */
    tap_widget(MODS_UI_REMOVE);
    tap_widget(MODS_UI_CLOSE); /* Cancel clears the message */
    draw(1600, 900);
    assert(installed_count() == Mods_Count() - 1);
    assert(!strstr(drawn, "Unsaved"));
    /* Nothing staged is left behind: Close does not ask to discard. */
    assert(ModsWindow_Locate(MODS_UI_CLOSE, &x, &y) && click(x, y) == 1);
    /* A profile that names it (on, with a load order) leaves it alone. */
    {
        char path[1024];
        FILE *f;
        assert(Mods_ProfileSave("Default"));
        assert(!Paths_User(path, sizeof(path), "mod-profiles/Default.txt"));
        f = fopen(path, "a");
        assert(f);
        fputs("mod.rmnew=1\nmod.rmnew.order=7\n", f);
        fclose(f);
        tap_widget(MODS_UI_LOAD);
        draw(1600, 900);
        assert(strstr(drawn, "Profile loaded for review."));
        assert(ModsWindow_Locate(MODS_UI_CLOSE, &x, &y) && click(x, y) == 1);
        assert(!Mods_Enabled(mod) && Mods_Removed(mod));
    }
    /* Back again, as a new import of the same id (an updated version). */
    said = import_zip(good, 2);
    assert(strstr(said, "Imported Remove me.") && find("rmnew") == mod && !Mods_Removed(mod));
    draw(1600, 900);
    assert(listed("Remove me") && !Mods_Enabled(mod));
    /* A mod that is on: its files are in use, so it stays, and says why. */
    said = import_zip(live, 1);
    other = find("rmlive");
    assert(strstr(said, "Imported Live one.") && other >= 0);
    Mods_SetEnabled(other, 1);
    assert(Mods_Active(other) && Mods_InUse(other));
    tap_widget(MODS_UI_REMOVE);
    draw(1600, 900);
    assert(strstr(drawn, "Live one is on, so the game is using its files."));
    assert(!strstr(drawn, "Cancel") && !Mods_Removed(other) && folder_has("mods/rmlive/mod.json"));
    /* Turned off, it was still used since the game started. */
    Mods_SetEnabled(other, 0);
    assert(Mods_InUse(other));
    tap_widget(MODS_UI_REMOVE);
    draw(1600, 900);
    assert(strstr(drawn, "Live one was tried since the game started") && folder_has("mods/rmlive/mod.json"));
    /* On a phone's two pages: Remove is on the details page, and ends on the list. */
    ModsWindow_Resize(700, 500);
    ModsWindow_Init();
    ModsWindow_Resize(700, 500);
    assert(!ModsWindow_Locate(MODS_UI_REMOVE, &x, &y)); /* the list's page: Import */
    assert(ModsWindow_Locate(MODS_UI_FOLDER, &x, &y));
    assert(ModsWindow_Locate(MODS_UI_ROW_FIRST, &x, &y));
    click(x, y); /* the first row opens its page */
    input(MENU_EVENT_BUTTON_UP, x, y, MENU_KEY_OTHER, NULL);
    assert(ModsWindow_Locate(MODS_UI_REMOVE, &x, &y) && !ModsWindow_Locate(MODS_UI_FOLDER, &x, &y));
    ModsWindow_SetImport(NULL, NULL, NULL);
    test_touch = 0;
}
/* HD pack... with a fake network (HdNet): GitHub's answer and the .zip
 * are the test's, served in small pieces on the job's thread. */
/* A millisecond (POSIX's nanosleep: usleep is not in every libc's C11 mode). */
static void nap(void)
{
    struct timespec t = {0, 1000000L};
    nanosleep(&t, NULL);
}
#define FAKE_SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
static char fake_api[4096], fake_zip[1024];
static int fake_status = 200, fake_error, fake_opens, fake_files, fake_forgets;
/* The download: at its second piece it waits while fake_gate is 0, then
 * ends as fake_end says (0: goes on), or waits for the network while
 * fake_waiting is set. */
static int fake_gate = 1, fake_end, fake_end_reason, fake_waiting;
/* The system saves under a name of its own (<name>-1); Stop comes while the
 * finished file is hashed. */
static int fake_renamed, fake_stop_in_hash;
static struct {
    FILE *in, *out;
    char path[2048];
    unsigned long done;
    int pieces, live;
} fetch;
static int fake_open(const char *url, void **stream, int *status)
{
    size_t *at;
    fake_opens++;
    assert(!strcmp(url, HD_PACK_LATEST));
    if (fake_error)
        return fake_error;
    *status = fake_status;
    assert((at = calloc(1, sizeof(*at))));
    *stream = at;
    return 0;
}
static long fake_read(void *stream, unsigned char *buffer, size_t size)
{
    size_t *at = stream, n = strlen(fake_api + *at);
    n = n < size ? n : size;
    n = n < 1024 ? n : 1024;
    memcpy(buffer, fake_api + *at, n);
    *at += n;
    return (long)n;
}
static void fake_close(void *stream) { free(stream); }
static int fake_download(const char *url, const char *path, long long *id)
{
    assert(!strncmp(url, "https://", 8) && !fetch.live);
    fake_files++;
    memset(&fetch, 0, sizeof(fetch));
    snprintf(fetch.path, sizeof(fetch.path), "%s%s", path, fake_renamed ? "-1" : "");
    assert((fetch.in = fopen(fake_zip, "rb")) && (fetch.out = fopen(fetch.path, "wb")));
    fetch.live = 1;
    *id = 7;
    return 0;
}
static int fake_poll(long long id, unsigned long *done, int *reason)
{
    unsigned char piece[1024];
    size_t n;
    assert(id == 7 && fetch.live);
    *done = fetch.done;
    if (fetch.pieces == 1 && !__atomic_load_n(&fake_gate, __ATOMIC_ACQUIRE))
        return HD_FETCH_RUNNING;
    if (fetch.pieces == 1 && fake_waiting)
        return HD_FETCH_WAITING;
    if (fetch.pieces == 1 && fake_end) {
        *reason = fake_end_reason;
        return fake_end;
    }
    if (!fetch.out)
        return HD_FETCH_DONE;
    fetch.pieces++;
    if ((n = fread(piece, 1, sizeof(piece), fetch.in)) > 0) {
        fwrite(piece, 1, n, fetch.out);
        fetch.done += n;
        *done = fetch.done;
        return HD_FETCH_RUNNING;
    }
    fclose(fetch.out);
    fetch.out = NULL;
    return HD_FETCH_DONE;
}
static void fake_stop(long long id)
{
    assert(id == 7 && fetch.live);
    if (fetch.out)
        fclose(fetch.out);
    fclose(fetch.in);
    remove(fetch.path); /* as DownloadManager.remove: the file, if still there */
    memset(&fetch, 0, sizeof(fetch));
}
static int fake_sha256(const char *path, char *out)
{
    (void)path;
    if (fake_stop_in_hash)
        HdPack_Cancel(); /* what Stop does (the window itself is the main thread's) */
    snprintf(out, 65, "%s", FAKE_SHA);
    return 1;
}
static void fake_forget(void) { fake_forgets++; }
static int fake_where(long long id, char *path, size_t size)
{
    assert(id == 7 && fetch.live);
    snprintf(path, size, "%s", fetch.path);
    return 1;
}
static HdNet fake_net = {fake_open, fake_read, fake_close, fake_download, fake_poll, fake_where, fake_stop, fake_sha256,
                         fake_forget, NULL};
/* GitHub's /releases/latest for `tag`, with the HD pack's asset (`size`
 * bytes, the fake's SHA-256 or another) and a source asset beside it. */
static void fake_release(const char *tag, long size, const char *digest)
{
    snprintf(fake_api, sizeof(fake_api),
             "{\"tag_name\": \"%s\", \"prerelease\": false, \"assets\": [{\"name\": \"memories-pc-%s-windows.zip\", "
             "\"size\": 5, \"digest\": \"sha256:%s\", \"browser_download_url\": \"https://example.invalid/a.zip\"}, "
             "{\"name\": \"" HD_PACK_PREFIX "%s.zip\", \"size\": %ld, \"digest\": \"sha256:%s\", "
             "\"browser_download_url\": \"https://github.com/x/releases/download/%s/" HD_PACK_PREFIX "%s.zip\"}]}",
             tag, tag, FAKE_SHA, tag, size, digest, tag, tag);
}
static long file_size(const char *path)
{
    struct stat info;
    return stat(path, &info) ? -1 : (long)info.st_size;
}
/* Ticks (and waits for the job's thread) until nothing is left to do;
 * what the footer says then. */
static const char *hd_said(void)
{
    static char said[4096];
    const char *line;
    for (int idle = 0; idle < 3;) {
        if (ModsWindow_Tick() || HdPack_Busy())
            idle = 0;
        else
            idle++;
        nap();
    }
    draw(1600, 900);
    said[0] = 0;
    for (line = drawn; *line; line = strchr(line, '\n') + 1) {
        const char *text = strchr(strchr(line, ' ') + 1, ' ') + 1;
        size_t length = (size_t)(strchr(line, '\n') - text);
        if (strlen(said) + length + 2 < sizeof(said)) {
            strncat(said, text, length);
            strcat(said, " ");
        }
    }
    return said;
}
static int downloads_empty(void)
{
    char path[2048];
    DIR *directory;
    struct dirent *entry;
    int empty = 1;
    snprintf(path, sizeof(path), "%s/downloads", root);
    if (!(directory = opendir(path)))
        return 1;
    while ((entry = readdir(directory)))
        empty &= entry->d_name[0] == '.';
    closedir(directory);
    return empty;
}
static void test_hd_pack_asset(void)
{
    HdRelease r;
    char why[400];
    fake_release("v0.2.0", 111279946,
                 "5BD2793F0F6369A2B28BE64E9D6E714B518F65C7132A87EC099534A4F2B981E4"); /* upper case is taken */
    assert(HdPack_PickAsset(fake_api, &r, why, sizeof(why)) == 1);
    assert(!strcmp(r.tag, "v0.2.0") && !strcmp(r.asset, "yfm-redecomp-hd-mod-v0.2.0.zip") && r.size == 111279946);
    assert(!strcmp(r.sha256, "5bd2793f0f6369a2b28be64e9d6e714b518f65c7132a87ec099534a4f2b981e4"));
    assert(!strncmp(r.url, "https://github.com/", 19));
    assert(HdPack_SpaceNeeded(&r) == 111279946ULL * 9 / 4 + (32ULL << 20));
    assert(HdPack_PickAsset("{\"tag_name\": \"v1.0.0\", \"assets\": []}", &r, why, sizeof(why)) == 0);
    assert(strstr(why, "The latest release (v1.0.0) has no HD pack."));
    assert(HdPack_PickAsset("{\"tag_name\": \"v1.0.0\", \"assets\": [{\"name\": \"" HD_PACK_PREFIX "v1.0.0.zip\", "
                            "\"size\": 10, \"browser_download_url\": \"https://x/y.zip\"}]}",
                            &r, why, sizeof(why)) == 0);
    assert(strstr(why, "no checksum")); /* no "digest": never downloaded unchecked */
    assert(HdPack_PickAsset("{\"tag_name\": \"v1.0.0\", \"assets\": [{\"name\": \"" HD_PACK_PREFIX "v1.0.0.zip\", "
                            "\"size\": 2000000000, \"digest\": \"sha256:" FAKE_SHA "\", "
                            "\"browser_download_url\": \"https://x/y.zip\"}]}",
                            &r, why, sizeof(why)) == 0);
    assert(strstr(why, "too large: more than 512 MB once unpacked"));
    assert(HdPack_PickAsset("{\"tag_name\": \"v1.0.0\", \"assets\": [{\"name\": \"" HD_PACK_PREFIX "v1.0.0.zip\", "
                            "\"size\": 600000000, \"digest\": \"sha256:" FAKE_SHA "\", "
                            "\"browser_download_url\": \"https://x/y.zip\"}]}",
                            &r, why, sizeof(why)) == 0); /* under 1 GB, but more than the importer unpacks */
    assert(strstr(why, "too large"));
    assert(HdPack_PickAsset("{\"message\": \"Not Found\"}", &r, why, sizeof(why)) == -1);
    assert(HdPack_PickAsset("<html>", &r, why, sizeof(why)) == -1);
}
static void test_hd_pack(void)
{
    static const ZipItem pack[] = {
        {"mods/assets-hd/mod.json", "{\"id\": \"" HD_PACK_ID "\", \"name\": \"Forbidden Memories HD\", "
                                    "\"version\": \"1.0\", \"enabled\": true}", 1, 0, 0, 0, 0, 0},
        {"mods/assets-hd/textures/card.png", "not really a PNG, but bytes all the same; a little longer than one piece "
                                             "of the fake's download, so that the download has a second piece to wait at "
                                             "when the test stops it, as a player's Stop does halfway. ................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................"
                                             "................................................................................",
         0, 0, 0, 0, 0, 0}};
    static const ZipItem other[] = {{"mods/other/mod.json", "{\"id\": \"not-hd\"}", 0, 0, 0, 0, 0, 0}};
    char marker[64], path[2048];
    const char *said;
    int count, x, y, hd = -1, picked = picks;
    long size;
    test_hd_pack_asset();
    snprintf(fake_zip, sizeof(fake_zip), "%s/served.zip", root);
    write_zip(fake_zip, pack, 2);
    size = file_size(fake_zip);
    assert(size > 1100); /* two of the fake's pieces at least */
    test_touch = 48;
    HdPack_SetNet(NULL);
    ModsWindow_SetImport(fake_pick, fake_picked, fake_fetch);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(!ModsWindow_Locate(MODS_UI_HD, &x, &y)); /* no network: no button, nothing contacted */
    HdPack_SetNet(&fake_net);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(ModsWindow_Locate(MODS_UI_HD, &x, &y) && fake_opens == 0);
    /* No internet, GitHub's limit, a release without the pack. */
    fake_error = HD_NET_OFFLINE;
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "No internet connection.") && HdPack_State() == HD_IDLE);
    fake_error = 0;
    fake_status = 403;
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "GitHub is limiting requests") && HdPack_State() == HD_IDLE);
    fake_status = 200;
    snprintf(fake_api, sizeof(fake_api), "{\"tag_name\": \"v9.0.0\", \"assets\": []}");
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "The latest release (v9.0.0) has no HD pack.") && fake_files == 0);
    /* The question first: the size, Wi-Fi and the room it needs. Cancel. */
    fake_release("v9.0.0", size, FAKE_SHA);
    tap_widget(MODS_UI_HD);
    said = hd_said();
    assert(strstr(said, "Download the HD pack (v9.0.0)? 0 MB; Wi-Fi recommended. Needs ~34 MB free."));
    assert(strstr(said, "Download") && fake_files == 0);
    tap_widget(MODS_UI_CLOSE); /* Cancel */
    assert(HdPack_State() == HD_IDLE && fake_files == 0);
    /* A damaged download: nothing installed, nothing left. */
    count = Mods_Count();
    fake_release("v9.0.0", size, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY); /* Download */
    assert(strstr(hd_said(), "The download was damaged. Try again.") && fake_files == 1);
    assert(Mods_Count() == count && downloads_empty() && !folder_has("mods/assets-hd"));
    /* The download manager gives up halfway: each reason in plain words,
     * nothing left, the system's download stopped. */
    fake_release("v9.0.0", size, FAKE_SHA);
    {
        static const struct {
            int end, reason;
            const char *said;
        } ends[] = {{HD_FETCH_FAILED, 1004, "The download from GitHub failed. Try again."},
                    {HD_FETCH_FAILED, 1006, "Not enough free space for the HD pack."},
                    {HD_FETCH_FAILED, 403, "GitHub is limiting requests"},
                    {HD_FETCH_GONE, 0, "The download was cancelled outside the game."}};
        for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
            fake_end = ends[i].end;
            fake_end_reason = ends[i].reason;
            tap_widget(MODS_UI_HD);
            hd_said();
            tap_widget(MODS_UI_APPLY);
            assert(strstr(hd_said(), ends[i].said) && downloads_empty() && !fetch.live);
        }
        fake_end = 0;
    }
    /* Waiting for the network: said so; Stop still ends it. */
    fake_waiting = 1;
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    while (!HdPack_Waiting())
        nap();
    ModsWindow_Tick();
    draw(1600, 900);
    assert(strstr(drawn, "Waiting for the network to go on with the HD pack (v9.0.0)"));
    tap_widget(MODS_UI_HD); /* Stop */
    assert(strstr(hd_said(), "The HD pack's download was stopped.") && downloads_empty() && !fetch.live);
    fake_waiting = 0;
    assert(fake_forgets == fake_files); /* each download first forgets what a dead app left */
    /* Stop halfway: nothing installed, nothing left. */
    fake_gate = 0;
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    while (HdPack_Done() == 0)
        nap();
    assert(HdPack_Busy());
    tap_widget(MODS_UI_APPLY); /* Apply waits for it */
    draw(1600, 900);
    assert(strstr(drawn, "Wait for the HD pack to finish") && strstr(drawn, "Stop"));
    tap_widget(MODS_UI_FOLDER); /* so does Import */
    tap_widget(MODS_UI_HD);     /* Stop */
    __atomic_store_n(&fake_gate, 1, __ATOMIC_RELEASE);
    assert(strstr(hd_said(), "The HD pack's download was stopped. Nothing was installed."));
    assert(Mods_Count() == count && downloads_empty() && !folder_has("mods/assets-hd") && picks == picked);
    /* Stop while the finished file is checked: still nothing installed. */
    fake_stop_in_hash = 1;
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    assert(strstr(hd_said(), "The HD pack's download was stopped.") && downloads_empty() && !folder_has("mods/assets-hd"));
    fake_stop_in_hash = 0;
    /* Stop in the frames after the download is checked, before the window
     * takes it: the Stop is not lost, and nothing is unpacked. */
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    while (HdPack_State() != HD_DOWNLOADED) {
        assert(HdPack_State() != HD_FAILED);
        nap();
    }
    tap_widget(MODS_UI_HD); /* Stop */
    assert(strstr(hd_said(), "The HD pack's download was stopped. Nothing was installed."));
    assert(Mods_Count() == count && downloads_empty() && !folder_has("mods/assets-hd"));
    /* The window's own question while a job runs keeps the line and its
     * buttons; Cancel brings the share back. */
    fake_gate = 0;
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    while (HdPack_Done() == 0)
        nap();
    tap_widget(MODS_UI_CHECK_FIRST); /* a staged change */
    tap_widget(MODS_UI_CLOSE);       /* asks about it */
    for (int i = 0; i < 5; i++)
        ModsWindow_Tick();
    draw(1600, 900);
    assert(strstr(drawn, "Discard your unsaved mod changes?") && !strstr(drawn, "Downloading the HD pack"));
    tap_widget(MODS_UI_CLOSE); /* Cancel: the question goes, the change stays */
    ModsWindow_Tick();
    draw(1600, 900);
    assert(strstr(drawn, "Downloading the HD pack (v9.0.0)"));
    tap_widget(MODS_UI_CHECK_FIRST); /* unstaged again */
    tap_widget(MODS_UI_HD);          /* Stop */
    __atomic_store_n(&fake_gate, 1, __ATOMIC_RELEASE);
    assert(strstr(hd_said(), "The HD pack's download was stopped.") && downloads_empty());
    /* A download that is not the HD pack. */
    write_zip(fake_zip, other, 1);
    fake_release("v9.0.0", file_size(fake_zip), FAKE_SHA);
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    assert(strstr(hd_said(), "That download is not the HD pack") && downloads_empty() && !folder_has("mods/other"));
    write_zip(fake_zip, pack, 2);
    fake_release("v9.0.0", size, FAKE_SHA);
    /* The whole way: downloaded (the system names the file <name>-1),
     * checked, unpacked, listed off. */
    fake_renamed = 1;
    tap_widget(MODS_UI_HD);
    hd_said();
    tap_widget(MODS_UI_APPLY);
    said = hd_said();
    assert(strstr(said, "HD pack installed (v9.0.0). Tick it and apply to use it."));
    fake_renamed = 0;
    assert(Mods_Count() == count + 1 && (hd = find(HD_PACK_ID)) == count && !Mods_Enabled(hd));
    assert(Settings_GetNamed("mod." HD_PACK_ID, 1) == 0); /* off at the next launch too, as an import */
    assert(folder_has("mods/assets-hd/textures/card.png") && downloads_empty());
    snprintf(path, sizeof(path), "%s/mods/assets-hd", root);
    assert(HdPack_InstalledTag(path, marker, sizeof(marker)) && !strcmp(marker, "v9.0.0"));
    /* Again: already there, nothing downloaded. */
    fake_files = 0;
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "The HD pack is already installed (v9.0.0).") && fake_files == 0);
    /* An older release than the one installed. */
    fake_release("v8.0.0", size, FAKE_SHA);
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "The installed HD pack (v9.0.0) is newer than the latest release (v8.0.0).") &&
           fake_files == 0);
    /* A newer one: Update, which replaces the folder. */
    fake_release("v9.1.0", size, FAKE_SHA);
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "Update the HD pack from v9.0.0 to v9.1.0? 0 MB; Wi-Fi recommended."));
    tap_widget(MODS_UI_APPLY);
    assert(strstr(hd_said(), "HD pack updated (v9.1.0).") && fake_files == 1);
    assert(Mods_Count() == count + 1 && HdPack_InstalledTag(path, marker, sizeof(marker)) &&
           !strcmp(marker, "v9.1.0") && !folder_has("mods/assets-hd-2"));
    /* In use: never replaced under the running game. */
    Mods_SetEnabled(hd, 1);
    assert(Mods_Active(hd));
    fake_release("v9.2.0", size, FAKE_SHA);
    tap_widget(MODS_UI_HD);
    assert(strstr(hd_said(), "The HD pack is in use, so it cannot be replaced now.") && fake_files == 1);
    /* The panel opening (the platform setting its picker) clears what a
     * job the app ended in left: the system's downloads, on a thread. */
    {
        int forgets = fake_forgets;
        write_text("downloads/" HD_PACK_PREFIX "v1.zip.part", "cut short");
        assert(!downloads_empty());
        ModsWindow_SetImport(fake_pick, fake_picked, fake_fetch);
        while (HdPack_State() != HD_IDLE)
            nap();
        assert(fake_forgets == forgets + 1 && downloads_empty());
    }
    HdPack_SetNet(NULL);
    ModsWindow_SetImport(NULL, NULL, NULL);
    test_touch = 0;
}
/* menu.h's UTF-8 cuts: never inside a character. */
static void test_text_cuts(void)
{
    const char *word = "Portugu\xC3\xAAs"; /* "Português": ê is bytes 7-8 */
    char cut[16];
    assert(Menu_TextBack(word, 9) == 7);
    assert(Menu_TextBack(word, 7) == 6);
    assert(Menu_TextBack(word, 0) == 0);
    assert(Menu_TextFit(word, 8) == 7);
    assert(Menu_TextFit(word, 9) == 9);
    {
        /* Ends before `length`: a buffer that long, as a caller's is (gcc
         * reads the string literal's 11 bytes as out of bounds for 50). */
        static const char padded[64] = "Portugu\xC3\xAAs";
        assert(Menu_TextFit(padded, 50) == 10);
    }
    memcpy(cut, word, 8); /* "Portugu" and the first byte of ê */
    cut[8] = '\0';
    Menu_TextTrim(cut);
    assert(strcmp(cut, "Portugu") == 0);
    snprintf(cut, sizeof(cut), "%s", word);
    Menu_TextTrim(cut);
    assert(strcmp(cut, word) == 0);
    snprintf(cut, sizeof(cut), "a\xE2\x80");  /* a cut-short U+2014 */
    Menu_TextTrim(cut);
    assert(strcmp(cut, "a") == 0);
}
/* The mods folder as the game starts with one (the run named "folder": the
 * only run where MEMORIES_MODS_DIR is not set, since the mods are scanned
 * once a process): a folder chosen before the start, mods of it marked as
 * its own, Remove leaving them be, and the choices a window makes. */
static void folder_mod(const char *place, const char *id)
{
    char file[1024];
    make_dir(place);
    snprintf(file, sizeof(file), "%s/mod.json", place);
    write_text(file, id);
}
static void folder_choices(int touch)
{
    char path[1024], chosen[1024], second[1024];
    int x, y, mine = find("mine"), before = restarts;
    const char *said;
    snprintf(chosen, sizeof(chosen), "%s/chosen", root);
    snprintf(second, sizeof(second), "%s/second", root);
    test_touch = touch;
    ModsWindow_SetImport(touch ? fake_pick : NULL, touch ? fake_picked : NULL, touch ? fake_fetch : NULL);
    ModsWindow_SetFolderPicker(fake_folder_pick, fake_folder_picked);
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
    assert(mine >= 0 && !strcmp(Mods_Origin(mine), "folder") && !strcmp(Mods_Origin(find("shipped")), "shipped"));
    assert(Mods_Folder(path, sizeof(path)) == 0 && !strcmp(path, chosen));
    /* Where the mod comes from, in its details: the first row is selected,
     * and the next if that is the shipped one. */
    said = on_screen(1600, 900);
    if (!strstr(said, "from your mods folder")) {
        input(MENU_EVENT_KEY_DOWN, 0, 0, MENU_KEY_DOWN, NULL);
        said = on_screen(1600, 900);
    }
    assert(strstr(said, "from your mods folder") && strstr(said, "Mine") && !strstr(said, "/  shipped"));
    /* Remove does not delete from it: it says so and touches nothing. */
    if (touch) {
        tap_widget(MODS_UI_REMOVE);
        said = on_screen(1600, 900);
        assert(strstr(said, "is in your own mods folder, which the game does not delete from. Delete it there."));
        snprintf(path, sizeof(path), "chosen/mine/mod.json");
        assert(folder_has(path) && !Mods_Removed(mine));
    }
    if (!touch) { /* the key hint is whole beside the buttons, at the window's own size */
        char hint[256];
        const char *at, *end;
        ModsWindow_Resize(920, 640);
        assert(ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y));
        draw(920, 640);
        at = strstr(drawn, "scroll");
        assert(at && (end = strchr(at, '\n')));
        while (at > drawn && at[-1] != '\n')
            at--;
        at = strchr(strchr(at, ' ') + 1, ' ') + 1; /* past the line's y and color */
        snprintf(hint, sizeof(hint), "%.*s", (int)(end - at), at);
        assert(Menu_TextWidthScaled(hint, 1) <= x - 28 && strstr(hint, "Wheel: scroll"));
        ModsWindow_Resize(1600, 900);
    }
    /* The question: the folder now, and the choices. */
    tap_widget(MODS_UI_MODSDIR);
    said = on_screen(1600, 900);
    assert(strstr(said, "Mods folder: ") && strstr(said, chosen) && strstr(said, "Choose another folder, or use none?"));
    assert(strstr(said, "Use no folder") && strstr(said, "Choose folder...") && strstr(said, "Cancel"));
    tap_widget(MODS_UI_CLOSE); /* Cancel */
    assert(!strstr(on_screen(1600, 900), "Choose another folder"));
    /* Another folder: saved at once, read after a restart, which Apply asks for. */
    folder_opens = 0;
    choose_folder(1, second);
    said = on_screen(1600, 900);
    assert(folder_opens == 1 && strstr(said, "Mods folder: ") && strstr(said, second));
    assert(strstr(said, "The game reads it after a restart: Apply changes."));
    assert(Mods_Folder(path, sizeof(path)) == 0 && !strcmp(path, second));
    tap_widget(MODS_UI_APPLY);
    said = on_screen(1600, 900);
    assert(strstr(said, "Applying these changes restarts the game.") && strstr(said, "Apply & restart") && restarts == before);
    tap_widget(MODS_UI_APPLY);
    assert(restarts == before + 1); /* Platform_RestartGame, which fails here */
    /* Back to the one the game started with: nothing to restart for. */
    choose_folder(1, chosen);
    said = on_screen(1600, 900);
    assert(!strstr(said, "after a restart"));
    tap_widget(MODS_UI_APPLY);
    assert(strstr(on_screen(1600, 900), "No pending changes") && restarts == before + 1);
    /* The window is asked for again (Platform_OpenMods sets the picker each time) while the
     * dialog is open: the answer is still taken, and is the one that was given. */
    folder_opens = 0;
    folder_result = 1;
    snprintf(folder_answer, sizeof(folder_answer), "%s", second);
    folder_waits = 2;
    tap_widget(MODS_UI_MODSDIR);
    tap_widget(MODS_UI_APPLY); /* Choose folder... */
    ModsWindow_Tick();
    ModsWindow_SetFolderPicker(fake_folder_pick, fake_folder_picked);
    tap_widget(MODS_UI_MODSDIR); /* asked again while it is open: no second dialog, no old answer */
    for (int i = 0; i < 5; i++)
        ModsWindow_Tick();
    assert(folder_opens == 1 && Mods_Folder(path, sizeof(path)) == 0 && !strcmp(path, second));
    choose_folder(1, chosen);
    assert(Mods_Folder(path, sizeof(path)) == 0 && !strcmp(path, chosen));
    /* Use no folder: a restart is due. */
    tap_widget(MODS_UI_MODSDIR);
    tap_widget(MODS_UI_MODSDIR);
    said = on_screen(1600, 900);
    assert(strstr(said, "Mods folder: none.") && strstr(said, "after a restart"));
    assert(Mods_Folder(path, sizeof(path)) != 0);
    assert(!folder_has("user/mods-folder.txt")); /* the file goes with the choice */
    /* The question with none chosen has one choice. */
    tap_widget(MODS_UI_MODSDIR);
    said = on_screen(1600, 900);
    assert(strstr(said, "Mods folder: none. Choose a folder to read its mods") &&
           !strstr(said, "Use no folder"));
    assert(!ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y));
    tap_widget(MODS_UI_CLOSE);
    /* A folder the game keeps its own things in is refused, with the reason. */
    snprintf(path, sizeof(path), "%s/user/mods/inside", root);
    choose_folder(1, path);
    said = on_screen(1600, 900);
    assert(strstr(said, "game") && Mods_Folder(path, sizeof(path)) != 0);
    /* No choice made, or the picker failing. */
    choose_folder(-1, NULL);
    assert(!strstr(on_screen(1600, 900), "Choose the folder"));
    snprintf(folder_failure, sizeof(folder_failure), "Could not open the folder picker: no portal");
    choose_folder(-2, NULL);
    assert(strstr(on_screen(1600, 900), "Could not open the folder picker: no portal"));
    folder_failure[0] = 0;
    /* The choice is back to the start's: the window may close without asking. */
    choose_folder(1, chosen);
    assert(ModsWindow_RequestClose());
    ModsWindow_SetImport(NULL, NULL, NULL);
    ModsWindow_SetFolderPicker(NULL, NULL);
    test_touch = 0;
}
/* A phone: a folder chosen without the right to read it asks for the right,
 * in a sentence first; refused, it stays chosen and the window says how to
 * give the right; given, a restart is due even if the folder is the one the
 * game started with (which it could not read). */
static int access_held, access_asks, access_answer, access_possible = 1;
static const char *access_name = "All files access";
static char access_failure[200];
static int fake_have(void) { return access_held; }
static int fake_can_ask(void) { return access_possible; }
static const char *fake_right(void) { return access_name; }
/* In how many lines `whole`, the footer's message, was drawn (it is cut after
 * four): the lines from the one it starts in, until they hold all of it. -1 when
 * it is not on the screen, -2 when what follows is not it. */
static int message_lines(const char *whole)
{
    char head[16];
    const char *line;
    size_t at = 0;
    int lines = 0;
    snprintf(head, sizeof(head), "%.12s", whole);
    if (!(line = strstr(drawn, head)))
        return -1;
    while (line > drawn && line[-1] != '\n')
        line--;
    while (*line && whole[at]) {
        const char *text = strchr(strchr(line, ' ') + 1, ' ') + 1, *end = strchr(line, '\n');
        size_t length = (size_t)(end - text);
        if (strncmp(text, whole + at, length))
            return -2;
        lines++;
        at += length;
        if (whole[at] == ' ') /* a line ends where a space was, which the text lost */
            at++;
        line = end + 1;
    }
    return whole[at] ? -2 : lines;
}
static int fake_ask(char *why, size_t size)
{
    (void)why;
    (void)size;
    access_asks++;
    return 0;
}
static int fake_asked(char *why, size_t size)
{
    snprintf(why, size, "%s", access_failure);
    return access_answer;
}
/* The window as it opens, at a size where a scratch folder's path fits a line. */
static void reopen(void)
{
    ModsWindow_Init();
    ModsWindow_Resize(1600, 900);
}
static void folder_access_case(void)
{
    char path[1024], second[1024];
    const char *said;
    int x, y, before = restarts;
    snprintf(second, sizeof(second), "%s/second", root);
    test_touch = 48;
    ModsWindow_SetFolderPicker(fake_folder_pick, fake_folder_picked);
    ModsWindow_SetFolderAccess(fake_have, fake_ask, fake_asked, fake_can_ask, fake_right);
    access_held = 0;
    reopen();
    /* Refused: the folder stays chosen, and the way to give the right is said. */
    choose_folder(1, second);
    said = on_screen(1600, 900);
    assert(strstr(said, "needs All files access") && strstr(said, "Allow it?") && !access_asks);
    assert(strstr(said, "Allow") && strstr(said, "Cancel"));
    assert(!ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y)); /* steps aside for the question */
    assert(Mods_Folder(path, sizeof(path)) == 0 && !strcmp(path, second)); /* saved before it was asked */
    tap_widget(MODS_UI_CLOSE); /* Cancel */
    said = on_screen(1600, 900);
    assert(strstr(said, "Without All files access the game cannot read it.") && !access_asks);
    reopen(); /* shown again whenever the folder is */
    assert(strstr(on_screen(1600, 900), "Without All files access the game cannot read it."));
    /* Allowed on Android's page. */
    choose_folder(1, second);
    tap_widget(MODS_UI_APPLY); /* Allow */
    assert(access_asks == 1);
    access_answer = 0;
    ModsWindow_Tick();
    assert(strstr(on_screen(1600, 900), "Allow All files access for the game"));
    access_answer = 1;
    access_held = 1;
    assert(ModsWindow_Tick());
    said = on_screen(1600, 900);
    assert(strstr(said, "All files access is on.") && strstr(said, "after a restart: Apply changes."));
    tap_widget(MODS_UI_APPLY);
    assert(strstr(on_screen(1600, 900), "restarts the game"));
    tap_widget(MODS_UI_CLOSE);
    /* Not on Android's page: the page's own refusal, or none to show. */
    access_held = 0;
    access_answer = -1;
    choose_folder(1, second);
    tap_widget(MODS_UI_APPLY);
    ModsWindow_Tick();
    assert(strstr(on_screen(1600, 900), "Without All files access the game cannot read it."));
    access_answer = -2;
    snprintf(access_failure, sizeof(access_failure), "This phone has no settings page for All files access.");
    choose_folder(1, second);
    tap_widget(MODS_UI_APPLY);
    ModsWindow_Tick();
    said = on_screen(1600, 900);
    assert(strstr(said, "This phone has no settings page") && !strstr(said, "Without All files access"));
    /* A device with no page for the right: no question, and the way out. */
    access_possible = 0;
    access_asks = 0;
    choose_folder(1, second);
    said = on_screen(1600, 900);
    assert(!strstr(said, "Allow it?") && !access_asks && strstr(said, "This device cannot give the game All files access.") &&
           strstr(said, "Use Import mod... instead."));
    reopen();
    assert(strstr(on_screen(1600, 900), "This device cannot give the game All files access."));
    access_possible = 1;
    /* Before Android 11 the right is Storage. */
    access_name = "Storage";
    reopen();
    said = on_screen(1600, 900);
    assert(strstr(said, "Without Storage the game cannot read it.") && !strstr(said, "All files access"));
    choose_folder(1, second);
    assert(strstr(on_screen(1600, 900), "the game needs Storage, which Android asks for"));
    tap_widget(MODS_UI_CLOSE);
    access_name = "All files access";
    /* Given in Settings with the game behind it, and noticed when it is back in front... */
    reopen();
    assert(strstr(on_screen(1600, 900), "Without All files access"));
    access_held = 1;
    ModsWindow_RefreshAccess();
    assert(ModsWindow_Tick());
    said = on_screen(1600, 900);
    assert(strstr(said, "All files access is on.") && !strstr(said, "Without"));
    assert(!ModsWindow_Tick()); /* once */
    /* ...but not over a question: a Remove keeps its text and its target, and the right counts once it is answered. */
    {
        static const ZipItem rm[] = {{"rmacc/mod.json", "{\"id\": \"rmacc\", \"name\": \"Access me\"}", 1, 0, 0, 0, 0, 0}};
        char question[4096];
        access_held = 0;
        ModsWindow_SetImport(fake_pick, fake_picked, fake_fetch);
        reopen();
        snprintf(import_source, sizeof(import_source), "%s/picked.zip", root);
        assert(strstr(import_zip(rm, 1), "Imported Access me."));
        tap_widget(MODS_UI_REMOVE);
        said = on_screen(1600, 900);
        assert(strstr(said, "Remove Access me? Its folder is deleted"));
        snprintf(question, sizeof(question), "%s", said);
        access_held = 1;
        ModsWindow_RefreshAccess();
        assert(ModsWindow_Tick());
        said = on_screen(1600, 900);
        assert(!strcmp(said, question) && !strstr(said, "is on."));
        assert(!ModsWindow_Tick());
        tap_widget(MODS_UI_CLOSE); /* Cancel */
        said = on_screen(1600, 900);
        assert(!strstr(said, "Remove Access me?") && !strstr(said, "Without All files access"));
        tap_widget(MODS_UI_APPLY); /* lit: the folder is read after a restart */
        assert(strstr(on_screen(1600, 900), "restarts the game"));
        tap_widget(MODS_UI_CLOSE);
        ModsWindow_SetImport(NULL, NULL, NULL);
    }
    /* ...or noticed while the panel shows it, within a second. */
    access_held = 0;
    reopen();
    access_held = 1;
    {
        int ticks = 0;
        while (!ModsWindow_Tick() && ticks < 40)
            ticks++;
        assert(ticks > 0 && ticks < 40 && strstr(on_screen(1600, 900), "All files access is on."));
    }
    /* The start's own folder, unread then and readable now: a restart reads it. */
    {
        char chosen[1024];
        snprintf(chosen, sizeof(chosen), "%s/chosen", root);
        access_held = 1;
        choose_folder(1, chosen);
        said = on_screen(1600, 900);
        assert(strstr(said, "The game reads it after a restart: Apply changes."));
        tap_widget(MODS_UI_APPLY);
        said = on_screen(1600, 900);
        assert(strstr(said, "Applying these changes restarts the game.") && strstr(said, "Apply & restart"));
        assert(restarts == before);
        tap_widget(MODS_UI_APPLY);
        assert(restarts == before + 1);
    }
    /* The folder question on a phone (pending 6) shows whole beside its buttons:
     * the A22's 1600 x 720 at twice the unit, and a small screen, with the folder
     * unreadable (the longest message), and Use no folder in reach. */
    {
        static const int sizes[][3] = {{1600, 720, 2}, {853, 400, 2}, {1280, 600, 1}};
        static const char hint[] = "Without All files access the game cannot read it. Allow it in this app's settings.",
                          cannot[] = "This device cannot give the game All files access. Use Import mod... instead.";
        char deep[1024], question[1400];
        snprintf(deep, sizeof(deep), "/storage/emulated/0/Download/Second"); /* a phone's, not the test's long scratch path */
        access_held = 0;
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            test_scale = sizes[i][2];
            ModsWindow_Init();
            ModsWindow_Resize(sizes[i][0], sizes[i][1]);
            choose_folder(1, deep);
            tap_widget(MODS_UI_CLOSE); /* Cancel the question for the right */
            on_screen(sizes[i][0], sizes[i][1]);
            assert(message_lines(hint) >= 1 && message_lines(hint) <= 4); /* the hint is whole too */
            access_possible = 0;
            ModsWindow_Init();
            ModsWindow_Resize(sizes[i][0], sizes[i][1]);
            on_screen(sizes[i][0], sizes[i][1]);
            assert(message_lines(cannot) >= 1 && message_lines(cannot) <= 4);
            access_possible = 1;
            ModsWindow_Init();
            ModsWindow_Resize(sizes[i][0], sizes[i][1]);
            tap_widget(MODS_UI_MODSDIR);
            said = on_screen(sizes[i][0], sizes[i][1]);
            assert(strstr(said, "Choose another folder, or use none?") && strstr(said, "Use no folder"));
            snprintf(question, sizeof(question), "Mods folder: %s The game cannot read it yet. Choose another folder, "
                     "or use none?", deep);
            assert(message_lines(question) >= 1 && message_lines(question) <= 4);
            assert(ModsWindow_Locate(MODS_UI_MODSDIR, &x, &y) && !ModsWindow_Locate(MODS_UI_FOLDER, &x, &y));
            tap_widget(MODS_UI_CLOSE);
        }
        test_scale = 1;
        ModsWindow_Resize(1600, 900);
    }
    assert(restarts == before + 1);
    ModsWindow_SetFolderAccess(NULL, NULL, NULL, NULL, NULL);
    ModsWindow_SetFolderPicker(NULL, NULL);
    test_touch = 0;
}
static int folder_main(void)
{
    char path[1024];
    assert(scratch_dir(root, sizeof(root), "memories-mod-window-folder"));
#ifdef _WIN32
    for (char *c = root; *c; c++) /* Mods_Folder gives a path with slashes; so must the paths compared to it */
        if (*c == '\\') *c = '/';
#endif
    make_dir("prog");
    make_dir("prog/mods");
    make_dir("user");
    make_dir("user/mods");
    make_dir("chosen");
    make_dir("second");
    folder_mod("prog/mods/shipped", "{\"id\": \"shipped\", \"name\": \"Shipped\"}");
    folder_mod("chosen/mine", "{\"id\": \"mine\", \"name\": \"Mine\"}");
    snprintf(path, sizeof(path), "%s/prog", root);
    assert(!setenv("MEMORIES_PROGRAM_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user", root);
    assert(!setenv("MEMORIES_USER_DIR", path, 1));
    snprintf(path, sizeof(path), "%s/user/settings.txt", root);
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    unsetenv("MEMORIES_MODS_DIR");
    unsetenv("MEMORIES_MODS_FOLDER");
    unsetenv("MEMORIES_MODS");
    snprintf(path, sizeof(path), "%s/chosen", root);
    {
        char why[400];
        assert(!Mods_SetFolder(path, why, sizeof(why)));
    }
    Settings_Load();
    Mods_Load();
    assert(Mods_Count() == 2);
    folder_choices(0);
    folder_choices(48);
    folder_access_case();
    return 0;
}
int main(int argc, char **argv)
{
    char path[1024];
    int w, h;
    if (argc > 1 && !strcmp(argv[1], "folder"))
        return folder_main();
    test_text_cuts();
    assert(scratch_dir(root, sizeof(root), "memories-mod-window"));
    make_dir("mods");
    for (int i = 0; i < 80; i++) {
        snprintf(path, sizeof(path), "mods/mod%02d", i);
        make_dir(path);
        snprintf(path, sizeof(path), "mods/mod%02d/mod.json", i);
        if (i == 2 || i == 3) { /* six fusion pairs both set: overlaps; mod03's first by its setting "on" */
            char json[2048] = "{\"settings\":[{\"key\":\"on\",\"type\":\"bool\",\"default\":1}],\"fusions\":[";
            for (int j = 0; j < 6; j++)
                snprintf(json + strlen(json), sizeof(json) - strlen(json), "%s{\"with\":[%d,%d],\"result\":%d%s}",
                         j ? "," : "", 100 + j, 200 + j, i, i == 3 && !j ? ",\"setting\":\"on\"" : "");
            strcat(json, "]}");
            write_text(path, json);
        } else if (i == 1) { /* more settings than the details show at once */
            char json[2048] = "{\"settings\":[";
            for (int j = 0; j < 12; j++)
                snprintf(json + strlen(json), sizeof(json) - strlen(json), "%s{\"key\":\"s%d\",\"type\":\"bool\"}",
                         j ? "," : "", j);
            strcat(json, "]}");
            write_text(path, json);
        } else if (i == 5) /* code, named only under "libraries" (an aarch64 entry) */
            write_text(path, "{\"libraries\":{\"aarch64\":\"other.aarch64.o\"}}");
        else
            write_text(path, i ? "{}"
                               : "{\"restart\":true,\"settings\":[{\"key\":\"speed\",\"label\":\"Speed\",\"default\":5,"
                                 "\"min\":0,\"max\":10}]}");
    }
    snprintf(path, sizeof(path), "%s/mods", root);
    setenv("MEMORIES_MODS_DIR", path, 1);
    snprintf(path, sizeof(path), "%s/settings.txt", root);
    setenv("MEMORIES_SETTINGS", path, 1);
    setenv("MEMORIES_USER_DIR", root, 1);
    Settings_Load();
    Mods_Load();
    assert(Mods_Count() == 80);
    assert(find("mod00") == 0);
    ModsWindow_Init();
    ModsWindow_Size(&w, &h);
    assert(w == 920 && h == 640);
    click(32, 160);
    assert(!Mods_Enabled(0)); /* staged */
    click(800, 610);
    assert(!restarts && !Mods_Enabled(0)); /* restart warning */
    click(680, 610);
    assert(!restarts); /* cancel warning */
    click(800, 610);
    click(800, 610);
    assert(restarts == 1 && Mods_Enabled(0));
    ModsWindow_Init();
    click(620, 238); /* settings tab */
    click(800, 357);
    input(MENU_EVENT_MOTION, 878, 357, MENU_KEY_OTHER, NULL);
    input(MENU_EVENT_BUTTON_UP, 878, 357, MENU_KEY_OTHER, NULL);
    assert(Mods_OptionValue(0, 0) == 5); /* slider edits are staged */
    click(800, 610);
    click(800, 610);
    assert(Mods_OptionValue(0, 0) == 10);
    click(790, 80); /* save Default profile */
    click(800, 165); /* disable */
    click(800, 610);
    click(800, 610);
    assert(!Mods_Enabled(0));
    click(860, 80);
    assert(!Mods_Enabled(0)); /* loading a profile is staged too */
    click(800, 610);
    click(800, 610);
    assert(Mods_Enabled(0) && Mods_OptionValue(0, 0) == 10);
    ModsWindow_Init();
    {
        /* Twelve settings scroll: by wheel, then by dragging the scrollbar. */
        int many = find("mod01");
        MenuEvent wheel = {0};
        wheel.type = MENU_EVENT_WHEEL;
        wheel.x = 600;
        wheel.y = 450;
        click(100, 142 + many * 58 + 20);
        click(620, 238);
        wheel.wheel = -1;
        for (int i = 0; i < 20; i++)
            ModsWindow_Event(&wheel);
        click(858, 505); /* the last setting's + */
        click(800, 610);
        assert(Mods_OptionValue(many, 11) == 1 && Mods_OptionValue(many, 0) == 0);
        wheel.wheel = 1;
        for (int i = 0; i < 20; i++)
            ModsWindow_Event(&wheel);
        click(889, 310);
        input(MENU_EVENT_MOTION, 889, 600, MENU_KEY_OTHER, NULL);
        input(MENU_EVENT_BUTTON_UP, 889, 600, MENU_KEY_OTHER, NULL);
        click(858, 505);
        click(800, 610);
        assert(Mods_OptionValue(many, 11) == 0 && Mods_OptionValue(many, 0) == 0);
    }
    ModsWindow_Init();
    click(50, 80);
    input(MENU_EVENT_TEXT, 0, 0, MENU_KEY_OTHER, "mod79");
    click(32, 160);
    click(800, 610);
    assert(Mods_Enabled(find("mod79"))); /* search actually filters */
    ModsWindow_Init();
    click(32, 160);
    assert(!input(MENU_EVENT_KEY_DOWN, 0, 0, MENU_KEY_ESCAPE, NULL));
    assert(input(MENU_EVENT_KEY_DOWN, 0, 0, MENU_KEY_ENTER, NULL)); /* explicit discard */
    assert(Mods_Enabled(0));
    ModsWindow_Init();
    click(50, 80); /* a focused search field does not hold the window open */
    assert(ModsWindow_RequestClose());
    ModsWindow_Init();
    click(32, 160);
    assert(!ModsWindow_RequestClose()); /* unsaved changes: asks first */
    assert(ModsWindow_RequestClose());  /* the second close discards */
    {
        MenuEvent motion = {0};
        motion.type = MENU_EVENT_MOTION;
        assert(!ModsWindow_Redraws(&motion)); /* plain pointer motion draws nothing */
    }
    ModsWindow_Init();
    click(550, 600); /* Open mods folder: the folder new mods are installed in */
    assert(!strcmp(opened, getenv("MEMORIES_MODS_DIR")));
    {
        /* What two enabled mods both change: in the header, and by kind in
         * the Compatibility tab, the first few lines, then the rest on a
         * press. The load order staged in the window decides who wins. */
        int more;
        ModsWindow_Init();
        click(32, 142 + 2 * 58 + 20); /* mod02 and mod03, staged */
        click(32, 142 + 3 * 58 + 20);
        click(800, 238); /* Compatibility */
        draw(920, 640);
        assert(strstr(drawn, "/  6 overlaps")); /* with the warnings when the line has room */
        assert(strstr(drawn, "Fusions: 6 (6 warnings)"));
        assert(strstr(drawn, "f4bd6a Fusion #100 + #200 (mod02, mod03): mod03 wins (later in load order)") ||
               strstr(drawn, "(mod02, mod03): mod03 wins (later in load order)"));
        assert(strstr(drawn, "...and 2 more: show them") && !strstr(drawn, "#105 + #205"));
        more = drawn_y("...and 2 more");
        click(600, 142 + 124 + more);
        draw(920, 640);
        assert(strstr(drawn, "#105 + #205") && strstr(drawn, "Show fewer"));
        click(100, 142 + 2 * 58 + 20); /* mod02 selected, and loaded after mod03 */
        click(856 + 14, 186 + 13);
        draw(920, 640);
        assert(strstr(drawn, "(mod03, mod02): mod02 wins (later in load order)"));
        click(32, 142 + 3 * 58 + 20); /* one of them off: nothing in common */
        draw(920, 640);
        assert(!strstr(drawn, "overlap") && strstr(drawn, "Enable this mod"));
        {
            /* A setting staged in the window, before Apply, switches mod03's
             * first fusion off: one overlap fewer. */
            int both[MODS_MAX] = {0}, off = 0, on = 1;
            const int *staged[MODS_MAX] = {0};
            both[2] = both[3] = 1;
            staged[3] = &on;
            assert(Mods_OverlapCount(Mods_Overlaps(both, NULL, staged)) == 6);
            staged[3] = &off;
            assert(Mods_OverlapCount(Mods_Overlaps(both, NULL, staged)) == 5);
            assert(Mods_OverlapCount(Mods_Overlaps(both, NULL, NULL)) == 6); /* saved: its default, on */
        }
    }
    {
        /* With a finger, every press ModsWindow_Grabs hands over as held
         * is the list scrollbar's: none beside it opens the row's page. */
        int x, y, grabbed = 0;
        test_touch = 48;
        ModsWindow_Init();
        ModsWindow_Resize(600, 400); /* a phone: two pages */
        draw(600, 400);
        assert(ModsWindow_Locate(MODS_UI_ROW_FIRST, &x, &y) && !ModsWindow_Locate(MODS_UI_BACK, &x, &x));
        for (x = 599; x >= 300; x--) {
            if (!ModsWindow_Grabs(x, y))
                continue;
            grabbed++;
            click(x, y);
            input(MENU_EVENT_BUTTON_UP, x, y, MENU_KEY_OTHER, NULL);
            assert(!ModsWindow_Locate(MODS_UI_BACK, &w, &h)); /* still the list */
        }
        assert(grabbed > 10);
        test_touch = 0;
    }
    test_folder_override();
    test_import();
    test_remove();
    test_hd_pack();
    ModsWindow_Init();
    {
        /* The Details tab calls a mod with only "libraries" a code mod, as
         * the loader does; one with neither key a content mod. */
        click(100, 142 + find("mod05") * 58 + 20);
        draw(920, 640);
        assert(strstr(drawn, "Native code mod") && !strstr(drawn, "Content mod"));
        click(100, 142 + find("mod04") * 58 + 20);
        draw(920, 640);
        assert(strstr(drawn, "Content mod") && !strstr(drawn, "Native code mod"));
    }
    ModsWindow_Init();
    ModsWindow_Resize(720, 480);
    ModsWindow_Size(&w, &h);
    assert(w == 720 && h == 480);
    MenuCanvas canvas = {0};
    canvas.width = w;
    canvas.height = h;
    canvas.stride = w;
    canvas.pixels = calloc((size_t)w * h, 4);
    assert(canvas.pixels);
    ModsWindow_Draw(&canvas);
    free(canvas.pixels);
    return 0;
}
