/* Declarative configuration, dependency ordering and portable profiles. */
#define _POSIX_C_SOURCE 200809L
#include "../../types.h"
#include "json.h"
#include "mods.h"
#include "overlap.h"
#include "hooks.h"
#include "events.h"
#include "pc/debug/log.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/compat/posix.h" /* rename() that replaces, on Windows too */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const JsonValue *member(int mod, const char *key) { return Json_Member(Mods_Manifest(mod), key); }
static int find(const char *id)
{
    int i;
    for (i = 0; i < Mods_Count(); i++)
        if (!strcmp(Mods_Id(i), id))
            return i;
    return -1;
}
static const char *require_id(const JsonValue *value)
{
    return Json_String(value, Json_String(Json_Member(value, "id"), ""));
}
static int version_compare(const char *a, const char *b)
{
    /* Numeric dotted versions; bounds are inclusive. */
    int n;
    for (n = 0; n < 4; n++) {
        char *ae, *be;
        long av = strtol(a, &ae, 10), bv = strtol(b, &be, 10);
        if (av != bv)
            return av < bv ? -1 : 1;
        a = *ae == '.' ? ae + 1 : "";
        b = *be == '.' ? be + 1 : "";
    }
    return 0;
}
int Mods_Compatible(int mod, const int *enabled, char *error, size_t size)
{
    const JsonValue *list = member(mod, "requires");
    int i;
    long api = Json_Number(member(mod, "min_api"), 1);
    const char *game = Mods_Metadata(mod, "game");
    /* Said apart: a mod for a newer release is the usual case, and the
     * player can fix it by updating the game. */
    if (*game && strcmp(game, "slus_01411")) {
        snprintf(error, size, "%s is made for another game (%s)", Mods_Name(mod), game);
        return 0;
    }
    if (api > MEMORIES_MOD_API) {
        snprintf(error, size, "%s needs a newer game: mod API %ld, this one has %d. Update the game",
                 Mods_Name(mod), api, MEMORIES_MOD_API);
        return 0;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *req = Json_At(list, i);
        const char *id = require_id(req), *min = Json_String(Json_Member(req, "min_version"), ""),
                   *max = Json_String(Json_Member(req, "max_version"), "");
        int dep = find(id);
        if (dep < 0 || !enabled[dep] || (*min && version_compare(Mods_Metadata(dep, "version"), min) < 0) ||
            (*max && version_compare(Mods_Metadata(dep, "version"), max) > 0)) {
            snprintf(error, size, "%s requires %s%s%s%s%s", Mods_Name(mod), id, *min ? " >= " : "", min,
                     *max ? " <= " : "", max);
            return 0;
        }
    }
    list = member(mod, "conflicts");
    for (i = 0; i < Json_Count(list); i++) {
        int other = find(Json_String(Json_At(list, i), ""));
        if (other >= 0 && enabled[other]) {
            snprintf(error, size, "%s conflicts with %s", Mods_Name(mod), Mods_Name(other));
            return 0;
        }
    }
    return 1;
}
/* Each mod is looked at once: a cycle (which Mods_Order reports), or a mod
 * required along many paths, would otherwise be walked again and again. */
static int waits_for_restart(int mod, const int *enabled, unsigned char *seen)
{
    const JsonValue *list = member(mod, "requires"), *req;
    if (seen[mod])
        return -1;
    seen[mod] = 1;
    for (req = Json_At(list, 0); req; req = Json_Next(req)) {
        int dep = find(require_id(req));
        if (dep >= 0 && enabled[dep] && !Mods_Active(dep) &&
            (Mods_RequiresRestart(dep) || waits_for_restart(dep, enabled, seen) >= 0))
            return dep;
    }
    return -1;
}
int Mods_WaitsForRestart(int mod, const int *enabled)
{
    unsigned char seen[MODS_MAX] = {0};
    return mod >= 0 && mod < MODS_MAX ? waits_for_restart(mod, enabled, seen) : -1;
}
static int order_ranked(const int *enabled, const int *ranks, int *order, char *error, size_t size);
int Mods_Order(const int *enabled, int *order, char *error, size_t size)
{
    return order_ranked(enabled, NULL, order, error, size);
}
/* The load order, each mod's rank its "Load order" (`ranks`, staged in the
 * Mods window), or the saved one when `ranks` is NULL. */
static int order_ranked(const int *enabled, const int *ranks, int *order, char *error, size_t size)
{
    int done[MODS_MAX] = {0}, total = 0, wanted = 0, i;
    for (i = 0; i < Mods_Count(); i++)
        wanted += !!enabled[i];
    while (total < wanted) {
        int best = -1, priority = INT_MAX;
        for (i = 0; i < Mods_Count(); i++)
            if (enabled[i] && !done[i]) {
                const char *keys[] = {"requires", "after"};
                int k, blocked = 0, rank;
                char key[160];
                for (k = 0; k < 2; k++) {
                    const JsonValue *list = member(i, keys[k]);
                    int j;
                    for (j = 0; j < Json_Count(list); j++) {
                        int dep = find(require_id(Json_At(list, j)));
                        if (dep >= 0 && enabled[dep] && !done[dep])
                            blocked = 1;
                    }
                }
                snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(i));
                rank = ranks ? ranks[i] : Settings_GetNamed(key, (int)Json_Number(member(i, "priority"), 0));
                if (!blocked && (best < 0 || rank < priority)) {
                    best = i;
                    priority = rank;
                }
            }
        if (best < 0) {
            /* order[] keeps the mods that could be placed, ended by -1. */
            order[total] = -1;
            snprintf(error, size, "Dependency/load-order cycle: check requires and after");
            return -1;
        }
        order[total++] = best;
        done[best] = 1;
    }
    return total;
}
int Mods_Validate(const int *enabled, char *error, size_t size)
{
    int i, order[MODS_MAX];
    for (i = 0; i < Mods_Count(); i++)
        if (enabled[i]) {
            if (Mods_Failed(i)) {
                snprintf(error, size, "%s: %s", Mods_Name(i), Mods_Status(i));
                return 0;
            }
            if (!Mods_Compatible(i, enabled, error, size))
                return 0;
        }
    return Mods_Order(enabled, order, error, size) >= 0;
}
/* The mods, applied last first: removing them in this order never leaves a
 * mod running without one it was applied after (and may call into). */
static int newest_first(int *out)
{
    int count = Mods_Count(), i, j;
    for (i = 0; i < count; i++) {
        for (j = i; j > 0 && Mods_Sequence(out[j - 1]) < Mods_Sequence(i); j--)
            out[j] = out[j - 1];
        out[j] = i;
    }
    return count;
}
static int apply_set(const int *enabled, char *error, size_t size);

/* The models' module notes are worked out once, after the whole set
 * (Mods_HoldModuleNotes), not after each mod. */
int Mods_Apply(const int *enabled, char *error, size_t size)
{
    int applied;
    Mods_HoldModuleNotes(1);
    applied = apply_set(enabled, error, size);
    Mods_HoldModuleNotes(0);
    return applied;
}

static int apply_set(const int *enabled, char *error, size_t size)
{
    int old[MODS_MAX], stored[MODS_MAX], order[MODS_MAX], newest[MODS_MAX], i, n;
    if (!Mods_Validate(enabled, error, size))
        return 0;
    /* Only what the player changed is written: a mod MEMORIES_MODS=0 or
     * MEMORIES_MOD_<ID> turned on or off for this run keeps its choice. */
    for (i = 0; i < Mods_Count(); i++) {
        char key[160];
        old[i] = Mods_Enabled(i);
        snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
        stored[i] = Settings_GetNamed(key, old[i]);
        if (enabled[i] != old[i])
            Settings_SetNamed(key, enabled[i]);
    }
    if (!Settings_Save()) {
        for (i = 0; i < Mods_Count(); i++) {
            char key[160];
            snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
            if (enabled[i] != old[i])
                Settings_SetNamed(key, stored[i]);
        }
        /* Where and why (settings.h); told here, so no notice repeats it. */
        snprintf(error, size, "%s Changes cancelled.", Settings_LastError());
        Settings_TakeNewError();
        return 0;
    }
    n = newest_first(newest);
    for (i = 0; i < n; i++)
        if (!enabled[newest[i]])
            Mods_SetEnabled(newest[i], 0);
    n = Mods_Order(enabled, order, error, size);
    for (i = 0; i < n; i++) {
        int mod = order[i];
        Mods_SetEnabled(mod, 1);
        if (Mods_Failed(mod))
            break;
    }
    for (i = 0; i < Mods_Count(); i++)
        if (enabled[i] && Mods_Failed(i)) {
            snprintf(error, size, "%s: %s", Mods_Name(i), Mods_Status(i));
            /* Back to the old set the way a launch builds it: everything off
             * in reverse, then the old mods in dependency order. */
            char ignored[128];
            n = newest_first(newest);
            for (int j = 0; j < n; j++)
                Mods_SetEnabled(newest[j], 0);
            n = Mods_Order(old, order, ignored, sizeof(ignored));
            for (int j = 0; n >= 0 ? j < n : order[j] >= 0; j++)
                Mods_SetEnabled(order[j], 1);
            for (int j = 0; j < Mods_Count(); j++) {
                char key[160];
                snprintf(key, sizeof(key), "mod.%s", Mods_Id(j));
                Settings_SetNamed(key, stored[j]);
            }
            if (!Settings_Save()) {
                snprintf(error, size, "Mod failed and preferences could not be restored; check settings before "
                         "restarting. %s", Settings_LastError());
                Settings_TakeNewError();
            }
            return 0;
        }
    return 1;
}
int Mods_OptionCount(int mod) { return Json_Count(member(mod, "settings")); }
const JsonValue *Mods_Option(int mod, int option) { return Json_At(member(mod, "settings"), option); }
int Mods_OptionValue(int mod, int option)
{
    const JsonValue *spec = Mods_Option(mod, option);
    return Mods_Setting(Mods_Id(mod), Json_String(Json_Member(spec, "key"), ""),
                        (int)Json_Number(Json_Member(spec, "default"), 0));
}
int Mods_SettingKeyValid(const char *key)
{
    /* "order" is the manager's own (mod.<id>.order, Mods_Order). */
    return key && *key && strcmp(key, "order") &&
           strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == strlen(key);
}
int Mods_OptionValid(int mod, int option, int value)
{
    const JsonValue *spec = Mods_Option(mod, option);
    const char *key = Json_String(Json_Member(spec, "key"), ""), *type = Json_String(Json_Member(spec, "type"), "int");
    int low = (int)Json_Number(Json_Member(spec, "min"), 0), high = (int)Json_Number(Json_Member(spec, "max"), 100);
    char setting[256];
    if (!strcmp(type, "bool")) {
        low = 0;
        high = 1;
    }
    if (!strcmp(type, "choice")) {
        low = 0;
        high = Json_Count(Json_Member(spec, "choices")) - 1;
    }
    if (!strcmp(type, "key")) {
        low = 0;
        high = 0xffff;
    }
    if (!Mods_SettingKeyValid(key) || value < low || value > high || snprintf(setting, sizeof(setting), "mod.%s.%s", Mods_Id(mod), key) >= (int)sizeof(setting))
        return 0;
    return 1;
}
int Mods_OptionSet(int mod, int option, int value)
{
    char key[256];
    int old;
    if (!Mods_OptionValid(mod, option, value))
        return 0;
    old = Mods_OptionValue(mod, option);
    snprintf(key, sizeof(key), "mod.%s.%s", Mods_Id(mod),
             Json_String(Json_Member(Mods_Option(mod, option), "key"), ""));
    Settings_SetNamed(key, value);
    if (value != old)
        Mods_OptionChanged(mod, option);
    return 1;
}

static int profile_path(char *path, size_t size, const char *name)
{
    char relative[160];
    if (!name || !*name || strlen(name) > 64 ||
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456"
                     "789_- ") != strlen(name))
        return 0;
    snprintf(relative, sizeof(relative), "mod-profiles/%s.txt", name);
    return Paths_User(path, size, relative) == 0;
}
static void profile_write(const char *key, int value, void *context)
{
    if (!strncmp(key, "mod.", 4))
        fprintf(context, "%s=%d\n", key, value);
}
static char profile_error[1200];
const char *Mods_ProfileSaveError(void) { return profile_error; }
int Mods_ProfileSave(const char *name)
{
    char path[1024], temp[1040];
    FILE *file;
    int i, failed;
    profile_error[0] = 0;
    if (!profile_path(path, sizeof(path), name))
        return 0;
    snprintf(temp, sizeof(temp), "%s", path);
    *strrchr(temp, '/') = 0;
    Paths_MakeDirs(temp);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    Paths_WriteBegin();
    file = fopen(temp, "w");
    if (!file) {
        Paths_WriteError(profile_error, sizeof(profile_error), path);
        return 0;
    }
    Settings_VisitNamed(profile_write, file);
    /* Include defaults, so a profile is complete even before any edit. */
    for (i = 0; i < Mods_Count(); i++) {
        int j;
        fprintf(file, "mod.%s=%d\n", Mods_Id(i), Mods_Enabled(i));
        for (j = 0; j < Mods_OptionCount(i); j++)
            fprintf(file, "mod.%s.%s=%d\n", Mods_Id(i), Json_String(Json_Member(Mods_Option(i, j), "key"), ""),
                    Mods_OptionValue(i, j));
    }
    failed = ferror(file);
    if (fclose(file))
        failed = 1;
    if (failed || rename(temp, path)) {
        Paths_WriteError(profile_error, sizeof(profile_error), path);
        remove(temp);
        return 0;
    }
    return 1;
}
int Mods_ProfileValue(const char *name, const char *key, int fallback)
{
    char path[1024], line[512], found[256];
    int value;
    FILE *file;
    if (!profile_path(path, sizeof(path), name) || !(file = fopen(path, "r")))
        return fallback;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "%255[^=]=%d", found, &value) == 2 && !strcmp(key, found))
            fallback = value;
    fclose(file);
    return fallback;
}
int Mods_ProfileRead(const char *name, int *enabled)
{
    char path[1024];
    FILE *file;
    int i;
    if (!profile_path(path, sizeof(path), name) || !(file = fopen(path, "r")))
        return 0;
    {
        char line[512], key[256];
        int on;
        while (fgets(line, sizeof(line), file)) {
            if (sscanf(line, "mod.%255[^=]=%d", key, &on) == 2 && !strchr(key, '.') && on && find(key) < 0) {
                fclose(file);
                return 0;
            }
        }
    }
    fclose(file);
    for (i = 0; i < Mods_Count(); i++) {
        char key[160];
        snprintf(key, sizeof(key), "mod.%s", Mods_Id(i));
        enabled[i] = !!Mods_ProfileValue(name, key, 0);
    }
    return 1;
}
static unsigned hash_text(unsigned hash, const char *s)
{
    while (*s) {
        hash ^= (unsigned char)*s++;
        hash *= 16777619u;
    }
    return (hash ^ 255u) * 16777619u;
}
static unsigned hash_json(unsigned hash, const JsonValue *value)
{
    const JsonValue *child;
    char number[40];
    hash = (hash ^ (unsigned)Json_TypeOf(value)) * 16777619u;
    hash = hash_text(hash, Json_Name(value) ? Json_Name(value) : "");
    hash = hash_text(hash, Json_String(value, ""));
    snprintf(number, sizeof(number), "%ld", Json_Number(value, Json_Bool(value, 0)));
    hash = hash_text(hash, number);
    for (child = Json_At(value, 0); child; child = Json_Next(child))
        hash = hash_json(hash, child);
    return hash;
}
static void hash_setting(const char *key, int value, void *context)
{
    unsigned *hash = context;
    char number[40];
    if (strncmp(key, "mod.", 4))
        return;
    for (int i = 0; i < Mods_Count(); i++)
        if (Mods_Active(i)) {
            char prefix[80];
            snprintf(prefix, sizeof(prefix), "mod.%s.", Mods_Id(i));
            if (!strncmp(key, prefix, strlen(prefix))) {
                const char *suffix = key + strlen(prefix);
                if (!strcmp(suffix, "order"))
                    return;
                for (int j = 0; j < Mods_OptionCount(i); j++)
                    if (!strcmp(suffix, Json_String(Json_Member(Mods_Option(i, j), "key"), "")))
                        return;
                snprintf(number, sizeof(number), "%d", value);
                *hash ^= hash_text(hash_text(2166136261u, key), number);
                break;
            }
        }
}
unsigned Mods_Signature(void)
{
    unsigned hash = 2166136261u, previous = 0;
    for (;;) {
        int i = -1;
        for (int candidate = 0; candidate < Mods_Count(); candidate++)
            if (Mods_Active(candidate) && Mods_Sequence(candidate) > previous &&
                (i < 0 || Mods_Sequence(candidate) < Mods_Sequence(i)))
                i = candidate;
        if (i < 0)
            break;
        previous = Mods_Sequence(i);
        char number[40];
        hash = hash_text(hash, Mods_Id(i));
        hash = hash_json(hash, Mods_Manifest(i));
        /* Only a mod with these lists: the signature of one without stays what it was,
         * and with it the save states made under it. */
        if (Mods_FusionsHash(i)) hash = (hash ^ Mods_FusionsHash(i)) * 16777619u;
        if (Mods_CardsHash(i)) hash = (hash ^ Mods_CardsHash(i)) * 16777619u;
        hash = (hash ^ Mods_CodeHash(i)) * 16777619u;
        for (int j = 0; j < Mods_OptionCount(i); j++) {
            snprintf(number, sizeof(number), "%d", Mods_RuntimeOption(i, j));
            hash = hash_text(hash, number);
        }
    }
    {
        unsigned settings_hash = 0;
        Settings_VisitNamed(hash_setting, &settings_hash);
        hash ^= settings_hash;
    }
    return hash ^ Mods_CardSignature() ^ Mods_PackSignature() ^ Mods_DiscSignature();
}

/* --- overlaps (overlap.h) --------------------------------------------------
 *
 * What the enabled mods change in common, in the order they would load, for
 * the Mods window and MEMORIES_TRACE=mods. Worked out again only when the set,
 * the order, a mod's settings, or what the code mods hook changes. */
static int (*overlap_card)(const char *text, long number);
static int (*overlap_card_name)(int id, char *out, size_t size);
static int (*overlap_card_info)(int id, int *base, int *type, int *attribute);
static int (*overlap_duelist)(const char *text);
static const char *(*overlap_function)(uintptr_t address, uintptr_t *offset);
static unsigned overlap_generation = 1;
static ModsOverlaps *overlaps;
static unsigned overlaps_key;
static int overlap_place[MODS_MAX], overlap_mod[MODS_MAX], overlap_count;
static const int *const *overlap_values; /* the settings staged in the window, or NULL: the saved ones */

static int source_card(const char *text, long number, void *context)
{
    (void)context;
    return overlap_card(text, number);
}
static int source_card_name(int id, char *out, size_t size, void *context)
{
    (void)context;
    return overlap_card_name(id, out, size);
}
static int source_card_info(int id, int *base, int *type, int *attribute, void *context)
{
    (void)context;
    return overlap_card_info(id, base, type, attribute);
}
static int source_duelist(const char *text, void *context)
{
    (void)context;
    return overlap_duelist(text);
}
static int source_setting(int place, const char *key, void *context)
{
    int mod = overlap_mod[place];
    (void)context;
    for (int j = 0; j < Mods_OptionCount(mod); j++)
        if (!strcmp(key, Json_String(Json_Member(Mods_Option(mod, j), "key"), "")))
            return overlap_values && overlap_values[mod] ? overlap_values[mod][j] : Mods_OptionValue(mod, j);
    return -1;
}
static int source_hook(int index, int *place, uint64_t *what, char *label, size_t size, void *context)
{
    int owner;
    const void *function;
    uintptr_t offset = 0;
    const char *name;
    (void)context;
    if (!Hooks_At(index, &owner, &function)) return 0;
    *place = owner >= 0 && owner < MODS_MAX ? overlap_place[owner] : -1;
    *what = (uint64_t)(uintptr_t)function;
    name = overlap_function ? overlap_function((uintptr_t)function, &offset) : NULL;
    if (name && !offset) snprintf(label, size, "%s", name);
    else snprintf(label, size, "at 0x%llx", (unsigned long long)(uintptr_t)function);
    return 1;
}
static int source_event(int index, int *place, uint64_t *what, char *label, size_t size, void *context)
{
    static const char *const names[MEMORIES_EVENT_COUNT] = {
        "INPUT", "DAMAGE", "REWARD",   "FUSION", "EFFECT",    "AI",        "SCENE",    "SAVE",
        "LOAD",  "SETTINGS", "EQUIP", "SLOT_SAVE", "SLOT_LOAD", "STARCHIP", "MENU", "MONSTER"};
    int owner;
    unsigned event;
    (void)context;
    if (!Mods_SubscriptionAt(index, &owner, &event)) return 0;
    *place = owner >= 0 && owner < MODS_MAX ? overlap_place[owner] : -1;
    *what = event;
    snprintf(label, size, "%s", event < MEMORIES_EVENT_COUNT && names[event] ? names[event] : "?");
    return 1;
}

static unsigned overlap_key(const int *enabled, const int *ranks, const int *const *values)
{
    unsigned hash = 2166136261u ^ overlap_generation;
    int i, owner;
    const void *function;
    unsigned event;
    for (int mod = 0; mod < Mods_Count(); mod++) {
        char key[160];
        int rank;
        snprintf(key, sizeof(key), "mod.%s.order", Mods_Id(mod));
        rank = ranks ? ranks[mod] : Settings_GetNamed(key, (int)Json_Number(member(mod, "priority"), 0));
        hash = (hash ^ (unsigned)!!enabled[mod]) * 16777619u;
        hash = (hash ^ (unsigned)rank) * 16777619u;
        for (int j = 0; j < Mods_OptionCount(mod); j++)
            hash = (hash ^ (unsigned)(values && values[mod] ? values[mod][j] : Mods_OptionValue(mod, j))) * 16777619u;
    }
    for (i = 0; Hooks_At(i, &owner, &function); i++) {
    }
    hash = (hash ^ (unsigned)i) * 16777619u;
    for (i = 0; Mods_SubscriptionAt(i, &owner, &event); i++) {
    }
    return (hash ^ (unsigned)i) * 16777619u;
}

static void log_overlaps(const ModsOverlaps *found)
{
    char line[1024];
    int n = Mods_OverlapCount(found), warnings = 0;
    for (int i = 0; i < n; i++) warnings += Mods_OverlapSeverity(found, i) == MODS_OVERLAP_WARNING;
    LOG(LOG_MODS, "overlaps: %d between %d enabled mods (%d warnings)", n, overlap_count, warnings);
    for (int i = 0; i < n; i++) {
        Mods_OverlapText(found, i, line, sizeof(line));
        LOG(LOG_MODS, "overlap: %s: %s: %s", Mods_OverlapSeverity(found, i) ? "warning" : "info",
            Mods_OverlapKindName(Mods_OverlapKind(found, i)), line);
    }
}

static long source_fusions(int mod, int (*each)(const JsonValue *, long, void *), void *each_context, void *context)
{
    char ignored[128];
    (void)context;
    return Mods_EachFusion(Mods_Id(overlap_mod[mod]), each, each_context, ignored, sizeof(ignored));
}
static JsonDocument *source_cards(int mod, void *context)
{
    (void)context;
    return Mods_CardsDocument(overlap_mod[mod]);
}
static long source_fusion_bytes(int mod, void *context)
{
    (void)context;
    return Mods_FusionsSize(overlap_mod[mod]);
}
const ModsOverlaps *Mods_Overlaps(const int *enabled, const int *ranks, const int *const *values)
{
    ModsOverlapMod list[MODS_MAX];
    ModsOverlapSource source = {0};
    char ignored[160];
    int order[MODS_MAX + 1], n;
    unsigned key = overlap_key(enabled, ranks, values);
    if (overlaps && key == overlaps_key) return overlaps;
    Mods_OverlapFree(overlaps);
    overlaps = NULL;
    overlaps_key = key;
    n = order_ranked(enabled, ranks, order, ignored, sizeof(ignored));
    if (n < 0) /* a cycle: the mods that could be placed (Mods_Order) */
        for (n = 0; order[n] >= 0; n++) {
        }
    for (int i = 0; i < MODS_MAX; i++) overlap_place[i] = -1;
    for (int i = 0; i < n; i++) {
        overlap_place[order[i]] = i;
        overlap_mod[i] = order[i];
        list[i].id = Mods_Id(order[i]);
        list[i].name = Mods_Name(order[i]);
        list[i].directory = Mods_Directory(order[i]);
        list[i].manifest = Mods_Manifest(order[i]);
    }
    overlap_count = n;
    overlap_values = values;
    if (overlap_card) source.card = source_card;
    if (overlap_card_info) source.card_info = source_card_info;
    if (overlap_card_name) source.card_name = source_card_name;
    if (overlap_duelist) source.duelist = source_duelist;
    source.setting = source_setting;
    source.fusions = source_fusions;
    source.fusion_bytes = source_fusion_bytes;
    source.cards = source_cards;
    source.hook = source_hook;
    source.event = source_event;
    overlaps = Mods_OverlapCompute(list, n, &source);
    overlap_values = NULL; /* the window's, only while it is read */
    if (overlaps && Mods_OverlapCount(overlaps) && Log_Enabled(LOG_MODS)) log_overlaps(overlaps);
    return overlaps;
}
int Mods_OverlapPlace(int mod) { return mod >= 0 && mod < MODS_MAX && overlaps ? overlap_place[mod] : -1; }

/* The mods applied now: their overlaps in the log, once the cards and the
 * duelists (and so their names) are known. */
static void log_applied(void)
{
    int enabled[MODS_MAX];
    if (!Log_Enabled(LOG_MODS)) return;
    for (int i = 0; i < Mods_Count(); i++) enabled[i] = Mods_Active(i);
    Mods_Overlaps(enabled, NULL, NULL);
}
void Mods_OverlapsForget(void) { overlap_generation++; }
void Mods_SetOverlapCards(int (*card)(const char *text, long number), int (*name)(int id, char *out, size_t size),
                          int (*info)(int id, int *base, int *type, int *attribute))
{
    overlap_card = card;
    overlap_card_name = name;
    overlap_card_info = info;
    overlap_generation++;
    if (overlap_duelist) log_applied();
}
void Mods_SetOverlapDuelists(int (*duelist)(const char *text))
{
    overlap_duelist = duelist;
    overlap_generation++;
    if (overlap_card) log_applied();
}
void Mods_SetFunctionNames(const char *(*lookup)(uintptr_t address, uintptr_t *offset))
{
    overlap_function = lookup;
    overlap_generation++;
}

int Mods_CheckManifest(int mod, char *error, size_t size)
{
    const char *arrays[] = {"settings", "requires", "after", "conflicts"};
    for (int i = 0; i < 4; i++) {
        const JsonValue *v = member(mod, arrays[i]);
        if (v && Json_TypeOf(v) != JSON_ARRAY) {
            snprintf(error, size, "%s must be an array", arrays[i]);
            return 0;
        }
    }
    for (int i = 0; i < Mods_OptionCount(mod); i++) {
        const JsonValue *spec = Mods_Option(mod, i);
        const char *type = Json_String(Json_Member(spec, "type"), "int");
        const char *key = Json_String(Json_Member(spec, "key"), "");
        long low = Json_Number(Json_Member(spec, "min"), 0), high = Json_Number(Json_Member(spec, "max"), 100);
        long def = Json_Number(Json_Member(spec, "default"), 0), step = Json_Number(Json_Member(spec, "step"), 1);
        if ((strcmp(type, "int") && strcmp(type, "bool") && strcmp(type, "choice") && strcmp(type, "key")) ||
            low < INT_MIN || high > INT_MAX || low > high || step < 1 || step > INT_MAX || def < INT_MIN ||
            def > INT_MAX || !Mods_OptionValid(mod, i, (int)def)) {
            snprintf(error, size, "Invalid setting schema: %s", key);
            return 0;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(key, Json_String(Json_Member(Mods_Option(mod, j), "key"), ""))) {
                snprintf(error, size, "Duplicate setting: %s", key);
                return 0;
            }
    }
    return 1;
}
