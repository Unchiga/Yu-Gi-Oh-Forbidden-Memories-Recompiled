#ifndef MEMORIES_PC_MODS_MOD_BUDGET_H
#define MEMORIES_PC_MODS_MOD_BUDGET_H
/* What a mod may take, so that one too big for this machine is refused by name and
 * the game goes on: at the scan by the size of its mod.json, and, when memory runs out
 * while it is applied, by a line in refused-mods.txt in the user folder that the
 * next start reads. A line holds while mod.json and the build are the ones it names. */
#include <stddef.h>

/* The memory mods may take, in all (MEMORIES_MOD_BUDGET_MB sets it). */
long long ModBudget_Bytes(void);
/* What a mod.json of this size takes at its worst: its fusions' table being sorted. */
long long ModBudget_Need(long long manifest_bytes);

/* `path`: refused-mods.txt. 1 and the reason when this mod.json is refused. */
int ModBudget_Refused(const char *path, const char *id, long long size, long long time, unsigned build, char *why,
                      size_t why_size);
/* Whether it got to the disk, these two and the last. */
int ModBudget_Refuse(const char *path, const char *id, long long size, long long time, unsigned build, const char *why);
int ModBudget_Forget(const char *path, const char *id);
/* The start after a refusal may not restart again (a line of its own). */
int ModBudget_Restarted(const char *path);
int ModBudget_SetRestarted(const char *path, int restarted);
#endif
