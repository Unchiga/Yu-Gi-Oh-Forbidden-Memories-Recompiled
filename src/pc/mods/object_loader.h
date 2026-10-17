#ifndef MEMORIES_PC_MODS_OBJECT_LOADER_H
#define MEMORIES_PC_MODS_OBJECT_LOADER_H
/* The loader for code mods: one ELF relocatable object (`.o`) per target
 * (notes/modding.md). The 32-bit games, Linux and Windows, read the same
 * 32-bit x86 object: both are 32-bit x86 with the same calling convention,
 * so the machine code in a mod runs on either; only the container differs,
 * and this reads the one container on both. The 64-bit Windows game reads
 * an x86-64 object of the Windows ABI in the same container (ELF64), whose
 * `.memories.abi` section names it (build_mod.py --target x86_64-windows).
 *
 * Loading lays the object's allocated sections out in fresh memory, binds
 * each name it leaves undefined through `resolve` (the game's export table
 * and the host's C library, exports.h), applies its relocations, and then
 * makes the code executable and nothing else. The file is untrusted input:
 * anything malformed, and anything this loader does not do, fails with a
 * message and leaves nothing behind. What it does not do, on purpose:
 * position-independent code (a GOT), thread-local storage, COMMON symbols,
 * and constructors. tools/pc/build_mod.py builds objects without any of
 * them. */
#include <stddef.h>
#include <stdint.h>

/* The target this game loads mods for: build_mod.py's name for it, the
 * `.memories.abi` tag a 64-bit object carries, and the middle of its file
 * name (`<library>.<target>.o`; the 32-bit games' is `<library>.o`). The
 * macOS ARM64 game links a dylib instead (build_mod.py --target macos,
 * ObjectLoader_LoadPath, `<library>.dylib`): its "libraries" key is "macos",
 * so an Android object named under "aarch64" is never handed to it. */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
#define OBJECT_LOADER_TARGET "macos"
#elif defined(__x86_64__) && defined(_WIN32)
#define OBJECT_LOADER_TARGET "x86_64-windows"
#elif defined(__x86_64__)
#define OBJECT_LOADER_TARGET "x86_64-linux"   /* no game is built so; the loader's tests are */
#elif defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
/* The Android app's game: the translated one (notes/pc-build.md, "Android
 * arm64"). Its mods go through the same guest pass as the game's own code
 * (build_mod.py --target aarch64-translated) and register their globals
 * with the translated runtime; an "aarch64" object, made for the
 * fixed-memory game, is refused with what to do. */
#define OBJECT_LOADER_TARGET "aarch64-translated"
#elif defined(__aarch64__)
#define OBJECT_LOADER_TARGET "aarch64"
#else
#define OBJECT_LOADER_TARGET "i386"
#endif

typedef void *(*ObjectResolver)(const char *name, void *context);

typedef struct {
    unsigned char *image;   /* code, then data, in one block */
    size_t size, code_size;
    /* The object's own global and local functions and objects, for finding
     * its entry point and for crash reports. Names point into `strings`. */
    struct ObjectSymbol { const char *name; uintptr_t address, size; int function, global; } *symbols;
    size_t symbol_count;
    char *strings;
    /* A hash of what was loaded: the allocated sections, their relocations
     * and the global names, and nothing else. Debugging information (which
     * holds the build folder and each header's MD5) and .comment are left
     * out, so a rebuild that makes the same code hashes the same. */
    uint32_t hash;
    void *native_handle;   /* macOS ARM64 dylib, retained while callbacks exist */
    /* An aarch64-translated object's MemoriesModUnregisterGlobals, called
     * on ObjectLoader_Free: its globals leave the translated runtime
     * before the memory they are in is given back. */
    void (*unregister)(void);
} LoadedObject;

/* Returns 0 and fills `object`, or -1 with the reason in `error`. */
int ObjectLoader_Load(const void *data, size_t size, ObjectResolver resolve, void *context,
                      LoadedObject *object, char *error, size_t error_size);
/* A global symbol the object defines, or NULL. */
void *ObjectLoader_Symbol(const LoadedObject *object, const char *name);
/* Gives the memory back. The game never does this for a mod (code it may
 * still hold pointers into); the tests do. */
void ObjectLoader_Free(LoadedObject *object);
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
int ObjectLoader_LoadPath(const char *path, LoadedObject *object, char *error, size_t error_size);
#endif

#endif
