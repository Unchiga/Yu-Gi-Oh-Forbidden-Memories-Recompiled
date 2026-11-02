#ifndef MEMORIES_PC_MODS_IMPORT_H
#define MEMORIES_PC_MODS_IMPORT_H
/* A mod's .zip put into the mods folder (notes/mods-window.md, "Import
 * mod..."): the Android Mods panel's Import button, which has no folder to
 * copy a mod into by hand. Platform-independent, so the tests run it on a
 * desktop; nothing on a desktop calls it.
 *
 * Mods_ImportOpen reads the .zip's central directory and checks every
 * entry before anything is written: no name that leaves the folder (an
 * absolute path, a drive letter, a ".." or "." part; backslashes count as
 * slashes), no symbolic link or other special file, no encryption, only
 * stored and deflated entries, at most MODS_IMPORT_ENTRIES of them and
 * MODS_IMPORT_BYTES once unpacked. The mods are the folders holding a
 * mod.json: the .zip's root itself (the folder is then named by the
 * manifest's id), the folders at its top, or the folders one wrapper folder
 * down; a mod.json inside a mod is that mod's file. __MACOSX/ is left out.
 *
 * Mods_ImportInstall unpacks them into a new hidden folder of the mods
 * folder (".import-XXXXXX", which the mod scan skips), checking each
 * entry's size and CRC, and then renames each mod into place, after moving
 * the folder it replaces (ModsImportMod.replace) aside. Any failure puts
 * back what it moved and removes the hidden folder: nothing is written
 * outside the mods folder, and nothing half unpacked is left in it. An old
 * folder that cannot be put back is kept in ".recovered-XXXXXX" (named in
 * the reason), never removed. Names are at most 32 folders deep; two mods
 * with one folder or one id are refused. */
#include <limits.h>
#include <stddef.h>

#define MODS_IMPORT_MAX 256                   /* mods in one .zip: as many as the game takes (MODS_MAX) */
#define MODS_IMPORT_ENTRIES 65534             /* files and folders in one .zip: the plain form's */
/* Unpacked, which the free space must hold too: 4 GB, less the byte that
 * keeps the count of bytes unpacked so far (Mods_ImportWatch) in 32 bits. */
#define MODS_IMPORT_BYTES 0xFFFFFFFFu
/* The .zip itself: the plain form's 4 GB where a long holds its offsets
 * (fseek, ftell), else 2 GB (a 32-bit long: Windows, and 32-bit builds). */
#define MODS_IMPORT_ZIP_BYTES ((unsigned long)(LONG_MAX < 0xFFFFFFFFL ? LONG_MAX : 0xFFFFFFFFL))

typedef struct ModsImport ModsImport;
typedef struct {
    char folder[128];   /* its folder in the mods folder */
    char id[64];        /* the manifest's id, else the folder's name */
    char name[128];     /* the manifest's name, else the id */
    char prefix[512];   /* its folder inside the .zip, with a slash ("" for the root) */
    int code;           /* it has code: "library" or "libraries", as the loader reads it (mods.c) */
    char replace[1024]; /* set by the caller: the folder it replaces, or "" */
} ModsImportMod;

/* NULL with the reason in `why` (plain words for the player) when the file
 * is not a .zip this can read or breaks a rule above. A .zip without a mod
 * opens with Mods_ImportCount 0. */
ModsImport *Mods_ImportOpen(const char *zip, char *why, size_t why_size);
int Mods_ImportCount(const ModsImport *import);
ModsImportMod *Mods_ImportMod(ModsImport *import, int index);
/* 1 when every mod is in place in `mods`, else 0 with the reason in `why`
 * and the mods folder as it was. */
int Mods_ImportInstall(ModsImport *import, const char *mods, char *why, size_t why_size);
/* The bytes its mods take unpacked (what Mods_ImportInstall writes). */
unsigned long long Mods_ImportBytes(const ModsImport *import);
/* For Mods_ImportInstall on a thread of its own (the HD pack's download,
 * hd_pack.h): it adds each file's size to *done as it is written, and a
 * nonzero *cancel stops it before the next file (0, "Cancelled.", the mods
 * folder as it was). Both are read and written with atomic operations;
 * NULL for neither. */
void Mods_ImportWatch(ModsImport *import, unsigned *done, const int *cancel);
void Mods_ImportClose(ModsImport *import);
/* Whether `mods`/`folder` is taken (a folder or a file is there); its
 * path in `path` either way. */
int Mods_ImportTaken(const char *mods, const char *folder, char *path, size_t size);
/* Whether `directory` holds a mod (a mod.json), with its id in `id`: the
 * manifest's, else the folder's name, as the loader takes it. */
int Mods_ImportFolderId(const char *directory, char *id, size_t size);
/* Removes what an import cut short left in the mods folder (".import-*"
 * folders and ".incoming*.zip" files). */
void Mods_ImportCleanup(const char *mods);
/* Removes a folder and everything in it, never following a link (on
 * Windows, where only the tests run it, a junction is followed). 0 when it
 * is gone. */
int Mods_ImportRemoveTree(const char *path);
/* Removes the mod folder `directory` (a mod's Mods_Directory) of `mods`,
 * the Android Mods panel's Remove. It must be a folder of its own right in
 * `mods`: <mods>/<name> with one plain name (no separator, no ":", no
 * leading "."), a real folder and not a link; anything else is refused and
 * nothing is touched. The folder is renamed to ".import-removing-<name>"
 * (hidden, which the mod scan skips) and then deleted; what that leaves
 * after a failure or an interruption is swept by Mods_ImportCleanup, as the
 * panel's opening does. 0 when it is gone (it was already), else -1 with the
 * reason in `why`. Links are told by lstat where there is one; on Windows
 * (which has none here, and where nothing but the tests runs this: the
 * button exists only on Android) stat follows a junction. */
int Mods_ImportRemoveInstalled(const char *mods, const char *directory, char *why, size_t why_size);
#endif
