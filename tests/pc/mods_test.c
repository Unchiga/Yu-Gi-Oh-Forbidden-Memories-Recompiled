/* Finding mods, applying them, and what a mod's data overrides do to the
 * sectors the drive model delivers. The disc is a stub here: the only thing
 * that matters is that a file lookup answers, since everything past that is
 * sector arithmetic. */
#define _POSIX_C_SOURCE 200809L
#include "pc/mods/mods.h"
#include "pc/mods/events.h"
#include "pc/mods/exports.h"
#include "pc/mods/json.h"
#include "pc/debug/symbols.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>

#define CARD_LBA 5000
#define CARD_SIZE 5000u
#define XA_LBA 9000
#define XA_SIZE (3u * 2048u)
#define MODEL_LBA 8000                    /* three records (mods.c, check_disc_models) */
#define MODEL_SIZE (3u * 276u * 2048u)

/* --- the port around the mod system ---------------------------------- */

int Log_Enabled(int channel) { (void)channel; return 0; }
int Log_Wanted(int channel) { (void)channel; return 0; }
void Log_Printf(int channel, const char *format, ...) { (void)channel; (void)format; }
unsigned short Platform_Pad(int port) { (void)port; return 0; }
int Symbols_Add(const SymbolsEntry *entries, size_t count) { (void)entries; (void)count; return 0; }
#ifndef MODS_REAL_DISC
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    (void)lba;
    memset(out, 0, (size_t)sectors * 2048);
    return sectors;
}
int Memories_DiscSectorCount(void) { return 10000; }
int Memories_DiscOriginalFileInfo(const char *path, int *lba, unsigned *size)
{
    int xa = !strcmp(path, "\\DATA\\MASTER.XA;1"), model = !strcmp(path, "\\DATA\\MODEL.MRG;1");
    if (!xa && !model && strcmp(path, "\\DATA\\CARD.MRG;1")) return -1;
    if (lba) *lba = xa ? XA_LBA : model ? MODEL_LBA : CARD_LBA;
    if (size) *size = xa ? XA_SIZE : model ? MODEL_SIZE : CARD_SIZE;
    return 0;
}
int Memories_DiscFileInfo(const char *path, int *lba, unsigned *size)
{
    int retail;
    unsigned bytes;
    if (Memories_DiscOriginalFileInfo(path, &retail, &bytes)) return -1;
    if (lba) *lba = retail;
    if (size) *size = bytes;
    Mods_DiscFileInfo(retail, lba, size);
    return 0;
}
int Memories_DiscFileStart(const char *path)
{
    int lba = -1;
    return Memories_DiscFileInfo(path, &lba, NULL) ? -1 : lba;
}
#endif

/* The table build_game32.py generates for the game: sorted by name. */
static char exported_function[16];
static int exported_variable = 42;
const MemoriesModExport Memories_ModExports[] = {
    {"D_80010000", (void *)0x80010000u},
    {"Duel_DrawFieldCards", exported_function},
    {"gDuel_wSceneStateFlags", &exported_variable},
};
const unsigned Memories_ModExportCount = sizeof(Memories_ModExports) / sizeof(Memories_ModExports[0]);

/* --- fixtures -------------------------------------------------------- */

static char root[SCRATCH_MAX];

static void write_file(const char *relative, const void *data, size_t size)
{
    char path[2048];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(!fclose(file));
}

static void write_text(const char *relative, const char *text)
{
    write_file(relative, text, strlen(text));
}

static void make_dir(const char *relative)
{
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(!mkdir(path, 0777));
}

/* The audio replacement an "audio" mod goes through (src/pc/audio/replace.h). */
static int audio_loads, audio_unloads, audio_mod = -1;
static int fake_audio_load(int mod, const char *id, const char *directory, const struct JsonValue *audio,
                           char *error, size_t size)
{
    (void)directory;
    assert(!strcmp(id, "sounds") && Json_Count(Json_Member(audio, "music")) == 1);
    audio_loads++;
    audio_mod = mod;
    snprintf(error, size, "audio: gone.wav cannot be read");
    return 1;
}
static void fake_audio_unload(int mod) { assert(mod == audio_mod); audio_unloads++; }

static int find(const char *id)
{
    int i;
    for (i = 0; i < Mods_Count(); i++) {
        if (!strcmp(Mods_Id(i), id)) return i;
    }
    return -1;
}

static void preview_probe(MemoriesModEvent *event) { (void)event; }

/* The game's check of a mod's model modules (Mods_SetModuleCheck), as the
 * stubbed disc reads: a module counts when any of its bytes is not zero, and
 * the sectors are read as the game would, with the mods' changes in. */
static int module_check_mips(const unsigned char *stages, char *detail, size_t size, int *mod_native,
                             const char **mod_owner)
{
    static const int first[6] = {0, 10, 20, 30, 40, 42}, sectors[6] = {10, 10, 10, 10, 2, 2};
    int count = 0;
    *mod_native = 0;
    *mod_owner = NULL;
    for (int i = 0; i < 6; i++)
        for (size_t at = 0; at < (size_t)sectors[i] * 2048; at++)
            if (stages[(size_t)first[i] * 2048 + at]) {
                count++;
                break;
            }
    snprintf(detail, size, "%d", count);
    return count;
}

static int module_check_read(int lba, int sectors, void *out)
{
    memset(out, 0, (size_t)sectors * 2048);
    for (int i = 0; i < sectors; i++) Mods_DiscSector(lba + i, (unsigned char *)out + (size_t)i * 2048);
    return sectors;
}

int main(void)
{
    unsigned char sector[2048], replacement[3000];
    char settings[1024];
    int patcher, replacer, broken, camera, sounds, i;

    /* A relative path may not leave the directory it is relative to. */
    assert(Paths_Contained("card.mrg") && Paths_Contained("art/monster.tim"));
    assert(!Paths_Contained("../card.mrg") && !Paths_Contained("art/../../card.mrg"));
    assert(!Paths_Contained("/etc/passwd") && !Paths_Contained("") && !Paths_Contained("."));
    assert(!Paths_Contained("art//monster.tim") && !Paths_Contained("art/"));
    assert(!Paths_Contained("art\\monster.tim") && !Paths_Contained("C:/art"));

    /* The names a code mod binds to. */
    assert(Mods_Lookup("D_80010000") == (void *)0x80010000u);
    assert(Mods_Lookup("Duel_DrawFieldCards") == exported_function);
    assert(Mods_Lookup("gDuel_wSceneStateFlags") == &exported_variable);
    assert(!Mods_Lookup("Duel_DrawFieldCard") && !Mods_Lookup("") && !Mods_Lookup(NULL));
    /* And the C library the host lends, which is searched first. */
    assert(Mods_LibcSorted());
    assert(Mods_LibcLookup("memcpy") == (void (*)(void))memcpy);
    assert(Mods_Lookup("vsnprintf") && Mods_Lookup("strtol"));
    assert(!Mods_Lookup("fopen") && !Mods_Lookup("system") && !Mods_Lookup("getenv"));

    assert(scratch_dir(root, sizeof(root), "memories-mods"));
    make_dir("mods");
    /* A patch mod: bytes at a file offset, which crosses a sector boundary. */
    make_dir("mods/patcher");
    write_text("mods/patcher/mod.json",
               "{ \"id\": \"patcher\", \"name\": \"Patcher\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\CARD.MRG;1\","
               "               \"patch\": [ { \"at\": \"0x7FF\", \"bytes\": \"AABBCC\" } ] } ] }");
    /* A replacement mod: a file shorter than the one it stands in for. */
    make_dir("mods/replacer");
    for (i = 0; i < (int)sizeof(replacement); i++) replacement[i] = (unsigned char)(i & 0xff);
    write_file("mods/replacer/card.mrg", replacement, sizeof(replacement));
    write_text("mods/replacer/mod.json",
               "{ \"id\": \"replacer\", \"name\": \"Replacer\","
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\CARD.MRG;1\", \"replace\": \"card.mrg\" } ] }");
    /* A manifest that is not JSON at all, and one that is not a mod. */
    make_dir("mods/broken");
    write_text("mods/broken/mod.json", "{ \"id\": \"broken\", ");
    make_dir("mods/not-a-mod");
    write_text("mods/not-a-mod/readme.txt", "nothing to see");
    /* A mod with a library that is not there, applied from the settings. */
    make_dir("mods/camera");
    write_text("mods/camera/mod.json",
               "{ \"id\": \"camera\", \"name\": \"Camera\", \"library\": \"camera\","
               "  \"legacy_setting\": \"hand_camera\" }");

    /* XA sectors hold 2304 bytes, not a data sector's 2048: "data" may not
     * replace or patch them, by name or by sector. */
    make_dir("mods/xa-named");
    write_file("mods/xa-named/master.xa", replacement, sizeof(replacement));
    write_text("mods/xa-named/mod.json",
               "{ \"id\": \"xa-named\", \"enabled\": true,"
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\MASTER.XA;1\", \"replace\": \"master.xa\" } ] }");
    make_dir("mods/xa-raw");
    write_text("mods/xa-raw/mod.json",
               "{ \"id\": \"xa-raw\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"lba\": 8999, \"patch\": [ { \"at\": \"0x7FF\", \"bytes\": \"0102\" } ] } ] }");
    /* Two mods over the same raw sectors, and over the same bytes: the
     * later one wins and the Mods window says so beside it. */
    make_dir("mods/raw-a");
    write_file("mods/raw-a/a.bin", replacement, 100);
    write_text("mods/raw-a/mod.json",
               "{ \"id\": \"raw-a\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"lba\": 7000, \"sectors\": 2, \"replace\": \"a.bin\" },"
               "              { \"lba\": 7100, \"patch\": [ { \"at\": 4, \"bytes\": \"AAAA\" } ] } ] }");
    make_dir("mods/raw-b");
    write_file("mods/raw-b/b.bin", replacement + 1, 100);
    write_text("mods/raw-b/mod.json",
               "{ \"id\": \"raw-b\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"lba\": 7001, \"sectors\": 1, \"replace\": \"b.bin\" },"
               "              { \"lba\": 7100, \"patch\": [ { \"at\": 5, \"bytes\": \"BB\" } ] } ] }");

    /* Replacement sounds, which apply without a restart. */
    make_dir("mods/sounds");
    write_text("mods/sounds/mod.json",
               "{ \"id\": \"sounds\", \"enabled\": true, \"audio\": { \"music\": { \"0x2D0\": \"gone.wav\" } } }");
    Mods_SetAudio(fake_audio_load, fake_audio_unload);

    /* A translation whose files the mod's settings switch: "text" and
     * "font" entries are a name or {"file", "setting"}. */
    make_dir("mods/wording");
    write_text("mods/wording/mod.json",
               "{ \"id\": \"wording\", \"enabled\": true,"
               "  \"settings\": [ { \"key\": \"names\", \"type\": \"bool\", \"default\": 1 },"
               "                  { \"key\": \"menus\", \"type\": \"bool\", \"default\": 0 } ],"
               "  \"text\": [ \"base.txt\", { \"file\": \"names.txt\", \"setting\": \"names\" },"
               "            { \"file\": \"menus.txt\", \"setting\": \"menus\" },"
               "            { \"file\": \"lost.txt\", \"setting\": \"nope\" } ],"
               "  \"font\": { \"file\": \"letters.png\", \"setting\": \"menus\" } }");
    /* One file per choice of a "choice" setting ("value"), in a mod the
     * player has not applied: its settings then read as they are now. */
    make_dir("mods/people");
    write_text("mods/people/mod.json",
               "{ \"id\": \"people\","
               "  \"settings\": [ { \"key\": \"names\", \"type\": \"choice\", \"default\": 0,"
               "                    \"choices\": [ \"US\", \"JP\", \"BR\" ] },"
               "                  { \"key\": \"limit\", \"type\": \"int\", \"max\": 1000 } ],"
               "  \"text\": [ { \"file\": \"us.txt\", \"setting\": \"names\", \"value\": 0 },"
               "            { \"file\": \"jp.txt\", \"setting\": \"names\", \"value\": 1 },"
               "            { \"file\": \"br.txt\", \"setting\": \"names\", \"value\": 2 },"
               "            { \"file\": \"any.txt\", \"setting\": \"names\" } ],"
               "  \"font\": [ { \"file\": \"jp.png\", \"setting\": \"names\", \"value\": 1 } ] }");
    make_dir("mods/plain");
    write_text("mods/plain/mod.json", "{ \"id\": \"plain\", \"enabled\": true, \"text\": \"text.txt\" }");
    /* A data mod over MODEL.MRG: a byte in card 2's control modules (record
     * 1, sector 185) and one in card 3's model data (record 2, sector 10). */
    make_dir("mods/modeler");
    write_text("mods/modeler/mod.json",
               "{ \"id\": \"modeler\", \"name\": \"Modeler\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\MODEL.MRG;1\","
               "               \"patch\": [ { \"at\": \"0xE6800\", \"bytes\": \"01\" },"
               "                            { \"at\": \"0x119000\", \"bytes\": \"01\" } ] } ] }");
    Mods_SetModuleCheck(module_check_mips, module_check_read, NULL);

    snprintf(settings, sizeof(settings), "%s/settings.txt", root);
    write_text("settings.txt", "hand_camera=1\nmod.replacer=1\n");
    assert(!setenv("MEMORIES_SETTINGS", settings, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    Settings_Load();
    Mods_Load();

    patcher = find("patcher");
    replacer = find("replacer");
    broken = find("broken");
    camera = find("camera");
    assert(patcher >= 0 && replacer >= 0 && broken >= 0 && camera >= 0);
    assert(find("not-a-mod") < 0);          /* no manifest, no mod */
    sounds = find("sounds");
    assert(Mods_Count() == 13);
    /* Only the record whose modules the mod patched is the mod's MIPS, said
     * once in its module note (never its status); a mod that does not touch
     * MODEL.MRG has none. */
    assert(Mods_Active(find("modeler")) &&
           strstr(Mods_ModuleNote(find("modeler")), "1 attack module in MODEL.MRG card 2 is MIPS code") &&
           Mods_ModuleMips(find("modeler")) == 1 && !strstr(Mods_Status(find("modeler")), "attack module"));
    assert(!Mods_ModuleNote(patcher)[0] && !Mods_ModuleNote(replacer)[0] && !Mods_ModuleMips(patcher));
    assert(!Mods_Active(find("xa-named")) && strstr(Mods_Status(find("xa-named")), "MASTER.XA") &&
           strstr(Mods_Status(find("xa-named")), "\"audio\""));
    assert(!Mods_Active(find("xa-raw")) && strstr(Mods_Status(find("xa-raw")), "MASTER.XA"));
    assert(Mods_Active(find("raw-a")) && !strstr(Mods_Status(find("raw-a")), "wins"));
    assert(Mods_Active(find("raw-b")) && strstr(Mods_Status(find("raw-b")), "same sectors as raw-a") &&
           strstr(Mods_Status(find("raw-b")), "same bytes as raw-a"));
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(7001, sector) && sector[0] == replacement[1]);
    assert(Mods_DiscSector(7100, sector) && sector[4] == 0xAA && sector[5] == 0xBB);
    assert(!Mods_DiscSector(XA_LBA - 1, sector));
    /* An audio mod is live: applied now, its skipped files noted beside it. */
    assert(sounds >= 0 && Mods_Active(sounds) && !Mods_RequiresRestart(sounds));
    assert(audio_loads == 1 && audio_mod == sounds && strstr(Mods_Status(sounds), "gone.wav"));
    Mods_SetEnabled(sounds, 0);
    assert(audio_unloads == 1 && !Mods_Active(sounds));
    Mods_SetEnabled(sounds, 1);
    assert(audio_loads == 2 && Mods_Active(sounds));
    /* A "text" entry with a setting is read only while that setting is not
     * 0, as the mod was applied: the text is built once, so the mod asks for
     * a restart without saying so and a later change waits for it. A setting
     * the mod does not declare is noted and its file used; a name as a
     * string is read as it always was. */
    {
        int wording = find("wording"), plain = find("plain");
        char path[1024];
        const char *name;
        assert(wording >= 0 && plain >= 0 && Mods_Active(wording) && Mods_RequiresRestart(wording));
        assert(Mods_File(wording, "text", 0, path, sizeof(path), &name) && !strcmp(name, "base.txt") &&
               strstr(path, "/wording/base.txt"));
        assert(Mods_File(wording, "text", 1, path, sizeof(path), &name) && !strcmp(name, "names.txt") &&
               strstr(path, "/wording/names.txt"));
        assert(Mods_File(wording, "text", 2, path, sizeof(path), &name) && !strcmp(name, "menus.txt") && !path[0]);
        assert(Mods_File(wording, "text", 3, path, sizeof(path), &name) && strstr(path, "/wording/lost.txt"));
        assert(strstr(Mods_Status(wording), "does not declare (nope)"));
        assert(!Mods_File(wording, "text", 4, path, sizeof(path), &name));
        assert(Mods_File(wording, "font", 0, path, sizeof(path), &name) && !path[0]);
        assert(!Mods_File(wording, "font", 1, path, sizeof(path), &name));
        assert(Mods_File(plain, "text", 0, path, sizeof(path), &name) && !strcmp(name, "text.txt") &&
               strstr(path, "/plain/text.txt") && !Mods_Status(plain)[0]);
        assert(!Mods_File(plain, "text", 1, path, sizeof(path), &name));
        assert(!Mods_File(plain, "font", 0, path, sizeof(path), &name));
        /* A setting the player has not touched reads as the "default" its
         * manifest declares -- the value the Mods window shows -- whatever
         * the caller's fallback, so a card_layout's "full_bleed" declared on
         * is on at the first run. Only a key the mod does not declare (or
         * a mod that is not there) answers with the caller's fallback. */
        assert(Mods_Setting("wording", "names", 0) == 1 && Mods_Setting("wording", "names", 5) == 1);
        assert(Mods_Setting("wording", "menus", 1) == 0 && Mods_Setting("wording", "menus", 0) == 0);
        assert(Mods_Setting("people", "names", 9) == 0);
        assert(Mods_Setting("wording", "undeclared", 7) == 7 && Mods_Setting("nosuch", "names", 5) == 5);
        assert(Mods_Setting("plain", "names", 3) == 3);
        /* A key declared with no "default" is not given one: the caller's
         * fallback stands (a card entry's count_setting keeps its "count"),
         * until a value is stored. Any stored number, the extremes of an int
         * included, is what was stored. */
        assert(Mods_Setting("people", "limit", 100) == 100 && Mods_Setting("people", "limit", -7) == -7);
        Settings_SetNamed("mod.people.limit", 0);
        assert(Mods_Setting("people", "limit", 100) == 0);
        Settings_SetNamed("mod.people.limit", INT_MIN);
        assert(Mods_Setting("people", "limit", 100) == INT_MIN);
        Settings_SetNamed("mod.people.limit", INT_MAX);
        assert(Mods_Setting("people", "limit", 100) == INT_MAX);
        Settings_SetNamed("mod.people.limit", 250);
        assert(Mods_Setting("people", "limit", 100) == 250);
        assert(Mods_OptionValue(wording, 0) == 1 && Mods_OptionValue(wording, 1) == 0);
        /* What the player chose beats the default, 0 as much as any other
         * number; and the environment beats both. */
        Settings_SetNamed("mod.wording.names", 0);
        Settings_SetNamed("mod.wording.menus", 1);
        assert(Mods_Setting("wording", "names", 1) == 0 && Mods_Setting("wording", "menus", 0) == 1);
        assert(!setenv("MEMORIES_MOD_WORDING_NAMES", "1", 1) && Mods_Setting("wording", "names", 0) == 1);
        assert(!unsetenv("MEMORIES_MOD_WORDING_NAMES"));
        assert(!setenv("MEMORIES_MOD_WORDING_NAMES", "0", 1) && !setenv("MEMORIES_MOD_WORDING_MENUS", "1", 1));
        assert(Mods_OptionValue(wording, 0) == 0 && Mods_OptionValue(wording, 1) == 1);
        assert(Mods_File(wording, "text", 1, path, sizeof(path), &name) && path[0]);
        assert(Mods_File(wording, "text", 2, path, sizeof(path), &name) && !path[0]);
    }
    /* With "value", an entry is read only while its setting is that value. */
    {
        static const char *const files[] = {"us.txt", "jp.txt", "br.txt"};
        int people = find("people"), value, entry;
        char path[1024];
        const char *name;
        assert(people >= 0 && !Mods_Active(people) && Mods_RequiresRestart(people));
        for (value = 0; value < 3; value++) {
            char number[4];
            snprintf(number, sizeof(number), "%d", value);
            assert(!setenv("MEMORIES_MOD_PEOPLE_NAMES", number, 1));
            for (entry = 0; entry < 3; entry++) {
                assert(Mods_File(people, "text", entry, path, sizeof(path), &name) && !strcmp(name, files[entry]));
                assert(entry == value ? strstr(path, files[entry]) != NULL : !path[0]);
            }
            /* Without "value", any choice but the first. */
            assert(Mods_File(people, "text", 3, path, sizeof(path), &name) && !path[0] == !value);
            assert(Mods_File(people, "font", 0, path, sizeof(path), &name) && !path[0] == (value != 1));
            assert(!Mods_File(people, "text", 4, path, sizeof(path), &name));
        }
        assert(!Mods_Status(people)[0]);
    }
    assert(!strcmp(Mods_Name(patcher), "Patcher"));
    assert(!strcmp(Mods_Name(broken), "broken") && Mods_Status(broken)[0]);
    /* The manifest's own default, and what the settings say instead. */
    assert(Mods_Enabled(patcher) && Mods_Enabled(replacer));
    /* The key this mod's choice used to live under is still read. */
    assert(Mods_Enabled(camera));
    /* Data overrides only hold from a fresh launch, so they ask for one
     * unless the manifest says otherwise; code mods take the manifest's. */
    assert(Mods_RequiresRestart(replacer) && !Mods_RequiresRestart(patcher));
    /* A library that cannot be opened leaves the mod with a reason. */
    assert(Mods_Status(camera)[0]);

    /* Both mods are applied, so the replacement stands in for the file and
     * the patch is written over it: a sector replaced, then patched. The
     * patch straddles the boundary, one byte in one sector and two in the
     * next, and the sectors past the replacement's end read as zeroes rather
     * than as whatever the disc still holds there. */
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector));
    assert(!memcmp(sector, replacement, 2047) && sector[2047] == 0xAA);
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA + 1, sector));
    assert(sector[0] == 0xBB && sector[1] == 0xCC);
    assert(!memcmp(sector + 2, replacement + 2050, sizeof(replacement) - 2050));
    assert(!sector[sizeof(replacement) - 2048] && !sector[2047]);
    assert(!Mods_DiscSector(CARD_LBA - 1, sector));
    assert(!Mods_DiscSector(CARD_LBA + 3, sector)); /* past both mods' reach */

    /* A speculative helper can detect code rules without dispatching them. */
    assert(!Mods_HasSubscribers(MEMORIES_EVENT_FUSION));
    int preview_token = Mods_Subscribe(patcher, MEMORIES_EVENT_FUSION, 0, preview_probe);
    assert(preview_token && Mods_HasSubscribers(MEMORIES_EVENT_FUSION));
    Mods_Unsubscribe(patcher, preview_token);
    assert(!Mods_HasSubscribers(MEMORIES_EVENT_FUSION));
    preview_token = Mods_Subscribe(patcher, MEMORIES_EVENT_FUSION, 0, preview_probe);
    assert(preview_token);

    /* Removing a mod takes its overrides out with it, the moment it can:
     * the patch mod goes now, and the replacement, which asked for a
     * restart, stays in place until the game is launched again. */
    Mods_SetEnabled(patcher, 0);
    assert(!Mods_HasSubscribers(MEMORIES_EVENT_FUSION));
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector) && sector[2047] == replacement[2047]);
    Mods_SetEnabled(replacer, 0);
    assert(!Mods_Enabled(replacer) && !Settings_GetNamed("mod.replacer", 1));
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector));
    Mods_SetEnabled(patcher, 1);
    assert(Settings_GetNamed("mod.patcher", 0) == 1);
    memset(sector, 0, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector) && sector[2047] == 0xAA);

    /* The settings are saved with the mod keys in them, and read back. */
    assert(Settings_Save());
    Settings_Load();
    assert(Settings_GetNamed("mod.patcher", 0) == 1 && !Settings_GetNamed("mod.replacer", 1));
    /* A mod can be settled from the environment, for a test run. Loading
     * the settings again (Reload settings) takes the live mod out at once,
     * but the replacement, which asked for a restart, is only recorded as
     * removed: it stays in place until the next launch. */
    assert(!setenv("MEMORIES_MOD_PATCHER", "0", 1));
    Mods_Load();
    assert(!Mods_Enabled(patcher) && !Mods_Active(patcher));
    assert(!Mods_Enabled(replacer) && Mods_Active(replacer));
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector) && sector[2047] == replacement[2047]);

    Mods_Shutdown();
    return 0;
}
