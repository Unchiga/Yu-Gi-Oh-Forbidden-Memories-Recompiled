#ifndef MEMORIES_PC_MODS_IMPORT_JOB_H
#define MEMORIES_PC_MODS_IMPORT_JOB_H
/* Import mod... off the game's thread (notes/mods-window.md, "Import
 * mod..."): the copy of the chosen document, the reading of the .zip's
 * directory and the unpacking of its mods run on a thread of their own, as
 * the HD pack's steps do (hd_pack.h), so the panel keeps drawing, shows how
 * far they are and can stop them. Platform-independent; the tests drive it
 * with a fake copy.
 *
 * One job at a time, in two steps with the game thread's questions between
 * them (the mods the .zip would replace, the free names, the Replace
 * question: they read the mod list, which only the game thread may):
 *   ImportJob_Read: `fetch` (the platform's copy of the picked document
 *     into the mods folder, Platform_FetchModZip) and Mods_ImportOpen
 *     -> IMPORT_JOB_READ, the import in ImportJob_Take. A .zip with no mod
 *     in it, or one the importer refuses, ends in IMPORT_JOB_FAILED with
 *     the reason (the same words the player always had), the copy removed.
 *   ImportJob_Install: Mods_ImportInstall on the import ImportJob_Take gave
 *     (its hidden folder, then the rename into place) -> IMPORT_JOB_UNPACKED.
 * ImportJob_Cancel stops either step at its next chunk of 64 KB; a failure
 * or a cancel removes what the step wrote (the copy, the hidden folder: the
 * mods folder as it was) and ends in IMPORT_JOB_FAILED, ImportJob_Cancelled.
 * The caller reads the result and ImportJob_Reset ends the job. */
#include "import.h"
#include <stddef.h>

enum {
    IMPORT_JOB_IDLE,
    IMPORT_JOB_READING,   /* the copy and the reading are under way */
    IMPORT_JOB_READ,      /* the .zip is open, with at least one mod */
    IMPORT_JOB_UNPACKING,
    IMPORT_JOB_UNPACKED,  /* every mod is in place in the mods folder */
    IMPORT_JOB_FAILED
};

/* Copies the picked document to `path` (`size` bytes) in the mods folder:
 * 1, or 0 with the reason in `why`. It adds the bytes it copied to *done as
 * it goes and sets *total to the file's size when it knows it (0: it does
 * not), and a nonzero *cancel stops it (0, "Cancelled.", nothing left of
 * the copy). All three are read and written with atomic operations. */
typedef int (*ImportJobFetch)(char *path, size_t size, char *why, size_t why_size, unsigned *done, unsigned *total,
                              const int *cancel);

/* Starts the first step: `fetch` puts the .zip in `zip` (`zip_size` bytes,
 * the caller's, not touched by the caller until the job ends). 1 when the
 * thread runs; 0 when a job is under way already or no thread could start
 * (IMPORT_JOB_FAILED, ImportJob_Why). */
int ImportJob_Read(ImportJobFetch fetch, char *zip, size_t zip_size);
/* The opened import, once the state is IMPORT_JOB_READ; the caller owns it
 * (Mods_ImportClose) and the job is IDLE again. */
ModsImport *ImportJob_Take(void);
/* Starts the second step on `import` (the caller's, left alone until the
 * job ends) into the mods folder `mods`. 1 when the thread runs; 0 when it
 * does not: a Stop that came first (IMPORT_JOB_FAILED, nothing is
 * unpacked), or no thread. */
int ImportJob_Install(ModsImport *import, const char *mods);
/* IDLE again, after the caller read the result of UNPACKED or FAILED. */
void ImportJob_Reset(void);

int ImportJob_State(void);
int ImportJob_Busy(void);                  /* a step runs on its thread */
unsigned long ImportJob_Done(void);        /* the step's bytes so far */
unsigned long ImportJob_Total(void);       /* and all of them, 0 when not known */
void ImportJob_Cancel(void);               /* asked for by the game thread */
int ImportJob_StopAsked(void);             /* a Stop came, in any state */
int ImportJob_Cancelled(void);             /* the job ended in IMPORT_JOB_FAILED because of it */
const char *ImportJob_Why(void);           /* the reason of IMPORT_JOB_FAILED */
/* Tests: called by the unpacking's thread after each chunk it wrote
 * (Mods_ImportStepped). */
void ImportJob_SetStepHook(void (*hook)(void));
#endif
