#ifndef MEMORIES_PC_PATHS_H
#define MEMORIES_PC_PATHS_H
/* Where the port's files live. Two roots:
 *
 *  - the program directory, which holds the executable and the mods shipped
 *    with the release; nothing is ever written there (outside user/ in
 *    portable mode, below);
 *  - the user directory, which holds everything the player owns: settings,
 *    controls, save slots, save states, screenshots, their own mods and
 *    whatever a mod stores. On Windows that is
 *    Documents\My Games\YFM Re-Decomp; elsewhere $XDG_DATA_HOME/YFM Re-Decomp
 *    (~/.local/share/YFM Re-Decomp). A file named portable.txt in the
 *    program directory makes it user/ there instead (portable mode).
 *    MEMORIES_USER_DIR names another and wins over both. When that folder
 *    has no saves but saves/ beside the game does (where the port keeps
 *    everything when it cannot make the folder), what is there is copied
 *    in, and used where it is only when it cannot be.
 *
 * MEMORIES_PROGRAM_DIR names the program directory where there is no
 * executable to find it by (an Android app is the system's app_process;
 * android.c points it at the build's files it unpacks).
 */
#include <stddef.h>

const char *Paths_UserDir(void);
/* The Switch's one folder for everything: the program directory, with the
 * player's files in user/ under it. */
#define PATHS_SWITCH_DIR "sdmc:/switch/yfm-redecomp"
const char *Paths_ProgramDir(void);
/* Join a relative path onto a root, creating the directories above it in the
 * user root's case. Both return 0 on success, -1 if it would not fit. */
int Paths_User(char *out, size_t size, const char *relative);
int Paths_Program(char *out, size_t size, const char *relative);
/* Create a directory and every directory above it. 0 on success. */
int Paths_MakeDirs(const char *path);
/* Nonzero when a relative path stays inside its directory: not empty, not
 * absolute, no "." or ".." component, no backslashes or drive letters. What
 * a mod is allowed to name (Mods_OpenAsset, Mods_OpenData). */
int Paths_Contained(const char *relative);
/* Carry what older builds left in ./saves into the user directory. Copies a
 * file only when the destination is missing, so it is safe to call always. */
void Paths_MigrateLegacySaves(void);

/* Why writing `path` just failed, in words for the player:
 * "<path>: <reason>." The path is written as the system shows it (in full,
 * with backslashes, on Windows) and the reason is the operating system's own
 * (FormatMessage, in the user's language, on Windows; strerror elsewhere).
 * When Windows denied access inside Documents a short hint follows: an
 * antivirus's ransomware protection or Controlled folder access is what
 * usually does that. Call it straight after the failing call, before
 * anything else (removing the partial file) changes errno or the last error;
 * Paths_WriteBegin before the write's first call keeps an older failure from
 * being taken for this one. Returns `out`. */
void Paths_WriteBegin(void);
const char *Paths_WriteError(char *out, size_t size, const char *path);
/* The same without the path: "<reason>." and the hint, if any. */
const char *Paths_WriteReason(char *out, size_t size, const char *path);
/* A write under the user directory succeeded: said after one did (the
 * failures are known from Paths_WriteError and Paths_WriteReason). Leaves
 * errno and the last error as they were. */
void Paths_WriteDone(const char *path);
/* `watch` hears what writes under the user directory show: writable (1), or
 * not (0) with Paths_WriteReason's text, each time the last outcome changes.
 * Nothing is written to find out: the crash reports' "user dir" fact says
 * "not tried yet" until the game itself saves something (main.c). */
void Paths_WatchUserDir(void (*watch)(int writable, const char *why));

#endif
