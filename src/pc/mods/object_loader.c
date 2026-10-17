/* A code mod's object file, loaded (object_loader.h). Everything read from
 * the file is checked before it is used: offsets and sizes against the file,
 * indexes against their tables, strings against their sections. Values are
 * copied out with memcpy, so nothing in the file needs to be aligned.
 *
 * Two containers, one per game width: the 32-bit games read ELF32 i386
 * objects (REL relocations, the addend in place), the 64-bit games ELF64
 * ones (RELA, the addend in the table), x86-64 on Windows and AArch64 on
 * Android, that carry a `.memories.abi` section naming their ABI, which
 * build_mod.py adds. The tag is what tells a Windows-ABI object from a
 * Linux-ABI one: both are EM_X86_64, and their machine code calls functions
 * differently. */
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS */
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
#define MEMORIES_TRANSLATED_MMAN_IMPLEMENTATION
#endif
#include "object_loader.h"
#include "pc/compat/mman.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
#include <dlfcn.h>
#include <mach-o/loader.h>
#include "pc/compat/fs.h"
#endif
#if defined(__aarch64__) && !defined(_WIN32)
#include <unistd.h>   /* sysconf */
#endif

#define PAGE 4096u   /* the most a section may ask to be aligned to, on every target */
#define IMAGE_MAX (64u << 20)   /* far more than any mod; keeps the arithmetic small */

/* ELF, as far as a relocatable object needs it. */
#define ET_REL 1
#define EM_386 3
#define EM_X86_64 62
#define EM_AARCH64 183
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS 0x400
#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define STB_LOCAL 0
#define STB_WEAK 2
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_GOT32 3
#define R_386_PLT32 4
#define R_386_GOTOFF 9
#define R_386_GOTPC 10
#define R_386_GOT32X 43
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42
#define R_AARCH64_NONE 256
#define R_AARCH64_ABS64 257
#define R_AARCH64_ABS32 258
#define R_AARCH64_PREL64 260
#define R_AARCH64_PREL32 261
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADR_PREL_PG_HI21_NC 276
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC 278
#define R_AARCH64_TSTBR14 279
#define R_AARCH64_CONDBR19 280
#define R_AARCH64_JUMP26 282
#define R_AARCH64_CALL26 283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299
#define R_AARCH64_ADR_GOT_PAGE 311
#define R_AARCH64_LD64_GOT_LO12_NC 312

/* What this game loads: the container's class, the machine, and for a
 * 64-bit object the ABI its tag must name (build_mod.py's target). */
#if defined(__x86_64__) || defined(__aarch64__)
#define WIDE 1
#else
#define WIDE 0
#endif
#if defined(__x86_64__)
#define MACHINE EM_X86_64
#elif defined(__aarch64__)
#define MACHINE EM_AARCH64
#else
#define MACHINE EM_386
#endif
#define ABI OBJECT_LOADER_TARGET
#define ABI_SECTION ".memories.abi"
/* The Android app's translated game (object_loader.h): its objects come out
 * of the guest pass, which pairs each unit's globals with a registration
 * routine (MemoriesModRegisterGlobals, build_mod.py), and live anywhere in
 * the host's address space, never below 4 GB, where the translated runtime
 * takes an address for a guest one. */
#if defined(__aarch64__) && defined(MEMORIES_TRANSLATED) && !defined(__APPLE__)
#define TRANSLATED_OBJECTS 1
#include "pc/guest/translated_runtime.h"   /* GuestRuntime_AutomaticSpace */
#else
#define TRANSLATED_OBJECTS 0
#endif
/* A 64-bit object's calls to a host function too far for the call's offset
 * (32 bits on x86-64, 28 on AArch64: the C library, far above 4 GB) go
 * through a veneer each, `jmp *[rip]` or `ldr x16, 8; br x16` and the
 * address; GOT-relative loads go through a GOT entry each. */
#define VENEER 16

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Section;

typedef struct {
    uint32_t name;
    uint64_t value, size;
    unsigned type, bind, index;
} Symbol;

typedef struct {
    const unsigned char *file;
    size_t file_size;
    int wide;                 /* ELF64 */
    Section *sections;
    unsigned section_count;
    uint64_t *place;          /* each section's offset in the image, or NOT_LOADED */
    const Section *symtab, *strtab;
    unsigned symtab_index, symbol_count, symbol_size;
    uintptr_t *values;        /* each symbol's address, once bound */
    unsigned char *bound;     /* whether it has one a relocation may use */
    unsigned char *host;      /* bound outside the object: may be far away */
    uint32_t *veneer, *got;   /* per symbol: its veneer's or GOT entry's offset + 1, 0 for none yet */
    uint64_t veneer_at, veneer_end, got_at, got_end;
    char *error;
    size_t error_size;
} Loader;

#define NOT_LOADED UINT64_MAX

static int fail(Loader *loader, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(loader->error, loader->error_size, format, arguments);
    va_end(arguments);
    return -1;
}

static uint16_t u16(const unsigned char *at) { uint16_t v; memcpy(&v, at, 2); return v; }
static uint32_t u32(const unsigned char *at) { uint32_t v; memcpy(&v, at, 4); return v; }
static uint64_t u64(const unsigned char *at) { uint64_t v; memcpy(&v, at, 8); return v; }

/* Whether [offset, offset + length) lies inside [0, limit). */
static int inside(uint64_t offset, uint64_t length, uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

/* A NUL-terminated string at `offset` in a string table, or NULL. */
static const char *string_at(const Loader *loader, const Section *table, uint64_t offset)
{
    const char *start;
    if (!table || table->type != SHT_STRTAB || offset >= table->size) return NULL;
    start = (const char *)loader->file + table->offset + offset;
    return memchr(start, '\0', (size_t)(table->size - offset)) ? start : NULL;
}

static const char *section_name(const Loader *loader, const Section *section, const Section *names)
{
    const char *name = string_at(loader, names, section->name);
    return name ? name : "";
}

/* Symbol `i` of the symbol table, whose bounds read_sections checked. */
static Symbol symbol_at(const Loader *loader, unsigned i)
{
    const unsigned char *at = loader->file + loader->symtab->offset + (uint64_t)i * loader->symbol_size;
    Symbol symbol;
    symbol.name = u32(at);
    if (loader->wide) {
        symbol.type = at[4] & 0xf;
        symbol.bind = at[4] >> 4;
        symbol.index = u16(at + 6);
        symbol.value = u64(at + 8);
        symbol.size = u64(at + 16);
    } else {
        symbol.value = u32(at + 4);
        symbol.size = u32(at + 8);
        symbol.type = at[12] & 0xf;
        symbol.bind = at[12] >> 4;
        symbol.index = u16(at + 14);
    }
    return symbol;
}

#if defined(__x86_64__) || defined(__aarch64__)
/* A symbol's name for a message: its own, or its number. */
static const char *symbol_name(const Loader *loader, unsigned i, char *buffer, size_t size)
{
    const char *name = i < loader->symbol_count ? string_at(loader, loader->strtab, symbol_at(loader, i).name) : NULL;
    if (name && *name) return name;
    snprintf(buffer, size, "symbol %u", i);
    return buffer;
}
#endif

/* The object's ABI tag (.memories.abi), against this game's. */
static int check_abi(Loader *loader, const Section *names)
{
    unsigned i;
    for (i = 0; i < loader->section_count; i++) {
        const Section *section = &loader->sections[i];
        const char *text;
        size_t length;
        if (strcmp(section_name(loader, section, names), ABI_SECTION)) continue;
        if (section->type != SHT_PROGBITS || !section->size || section->size > 64) {
            return fail(loader, "has a damaged " ABI_SECTION " section");
        }
        text = (const char *)loader->file + section->offset;
        length = strnlen(text, (size_t)section->size);
        if (length != strlen(ABI) || memcmp(text, ABI, length)) {
#if TRANSLATED_OBJECTS
            /* An object of the fixed-memory Android game (v0.3.0 and the
             * builds before the translated one): its code reads guest
             * memory at a fixed host address, which this game has not. */
            if (length == 7 && !memcmp(text, "aarch64", 7)) {
                return fail(loader, "was built for the fixed-memory Android game, which this one replaces: ask the "
                                    "mod's author to rebuild it (build_mod.py --target aarch64-translated)");
            }
#endif
            return fail(loader, "was built for %.*s, and this game is " ABI "; build it with tools/pc/build_mod.py",
                        (int)length, text);
        }
        return 0;
    }
    return fail(loader, "has no " ABI_SECTION " section, so its ABI is unknown; build it with tools/pc/build_mod.py");
}

static int read_sections(Loader *loader)
{
    const unsigned char *file = loader->file;
    uint64_t section_offset;
    unsigned i, names_index, symtabs = 0, header_size, entry_size;
    const Section *names;
    if (loader->file_size < 52 || memcmp(file, "\177ELF", 4)) return fail(loader, "is not an ELF object file");
    if (file[5] != 1 || (file[4] != 1 && file[4] != 2)) return fail(loader, "is not a little-endian ELF object");
    loader->wide = file[4] == 2;
    if (loader->wide != WIDE) {
        return fail(loader, WIDE ? "is 32-bit code, and this game is 64-bit; it needs a 64-bit build of the mod"
                                 : "is 64-bit code, and this game is 32-bit; it needs the mod's 32-bit object");
    }
    if (loader->wide && loader->file_size < 64) return fail(loader, "is not an ELF object file");
    if (u16(file + 16) != ET_REL) return fail(loader, "is not a relocatable object (.o); build it with tools/pc/build_mod.py");
    if (u16(file + 18) != MACHINE) {
        unsigned machine = u16(file + 18);
        return fail(loader, "is %s code, and this game runs %s code", machine == EM_386 ? "32-bit x86" :
                    machine == EM_X86_64 ? "x86-64" : machine == EM_AARCH64 ? "arm64" : "another machine's",
                    MACHINE == EM_386 ? "32-bit x86" : MACHINE == EM_X86_64 ? "x86-64" : "arm64");
    }
    if (loader->wide) {
        section_offset = u64(file + 40);
        entry_size = u16(file + 58);
        loader->section_count = u16(file + 60);
        names_index = u16(file + 62);
        header_size = 64;
    } else {
        section_offset = u32(file + 32);
        entry_size = u16(file + 46);
        loader->section_count = u16(file + 48);
        names_index = u16(file + 50);
        header_size = 40;
    }
    if (entry_size != header_size || !loader->section_count) return fail(loader, "has no section table");
    if (!inside(section_offset, (uint64_t)loader->section_count * header_size, loader->file_size)) {
        return fail(loader, "is cut short (the section table runs past the end)");
    }
    if (names_index >= loader->section_count) return fail(loader, "has a bad section name table");
    loader->sections = calloc(loader->section_count, sizeof(*loader->sections));
    loader->place = malloc(loader->section_count * sizeof(*loader->place));
    if (!loader->sections || !loader->place) return fail(loader, "out of memory");
    for (i = 0; i < loader->section_count; i++) {
        const unsigned char *at = file + section_offset + (uint64_t)i * header_size;
        Section *section = &loader->sections[i];
        section->name = u32(at);
        section->type = u32(at + 4);
        if (loader->wide) {
            section->flags = u64(at + 8);
            section->addr = u64(at + 16);
            section->offset = u64(at + 24);
            section->size = u64(at + 32);
            section->link = u32(at + 40);
            section->info = u32(at + 44);
            section->addralign = u64(at + 48);
            section->entsize = u64(at + 56);
        } else {
            section->flags = u32(at + 8);
            section->addr = u32(at + 12);
            section->offset = u32(at + 16);
            section->size = u32(at + 20);
            section->link = u32(at + 24);
            section->info = u32(at + 28);
            section->addralign = u32(at + 32);
            section->entsize = u32(at + 36);
        }
        loader->place[i] = NOT_LOADED;
        if (section->type != SHT_NOBITS && !inside(section->offset, section->size, loader->file_size)) {
            return fail(loader, "is cut short (section %u runs past the end)", i);
        }
    }
    names = &loader->sections[names_index];
    if (names->type != SHT_STRTAB) return fail(loader, "has a bad section name table");
    for (i = 0; i < loader->section_count; i++) {
        const Section *section = &loader->sections[i];
        const char *name = section_name(loader, section, names);
        if (section->type == SHT_INIT_ARRAY || section->type == SHT_FINI_ARRAY || section->type == SHT_PREINIT_ARRAY ||
            !strncmp(name, ".ctors", 6) || !strncmp(name, ".dtors", 6)) {
            return fail(loader, "has constructors or destructors (%s), which mods may not have; "
                                "do that work in MemoriesModInit", name);
        }
        if (section->flags & SHF_TLS) return fail(loader, "has thread-local storage (%s), which mods may not have", name);
        if (section->type == (loader->wide ? SHT_REL : SHT_RELA) && section->info < loader->section_count &&
            (loader->sections[section->info].flags & SHF_ALLOC)) {
            return fail(loader, loader->wide ? "has REL relocations, which 64-bit objects do not use"
                                             : "has RELA relocations, which 32-bit x86 objects do not use");
        }
        if (section->type == SHT_SYMTAB) {
            symtabs++;
            loader->symtab = section;
            loader->symtab_index = i;
        }
        /* SHT_GROUP (a COMDAT group): one object, nothing to fold; its
         * sections load as any others. */
    }
    if (symtabs != 1) return fail(loader, "has %s symbol table", symtabs ? "more than one" : "no");
    loader->symbol_size = loader->wide ? 24 : 16;
    if (loader->symtab->entsize != loader->symbol_size || loader->symtab->size % loader->symbol_size ||
        loader->symtab->link >= loader->section_count || loader->sections[loader->symtab->link].type != SHT_STRTAB) {
        return fail(loader, "has a bad symbol table");
    }
    loader->strtab = &loader->sections[loader->symtab->link];
    loader->symbol_count = (unsigned)(loader->symtab->size / loader->symbol_size);
    if (loader->wide && check_abi(loader, names)) return -1;
    return 0;
}

/* The page code and data each start on, so that mprotect makes the code
 * executable and nothing else: 4 KiB, except on AArch64, whose kernels run
 * 4, 16 or 64 KiB pages (Android 15 phones can run 16 KiB ones). There it
 * is the system's page, 16 KiB at least, so that an image, and the hash of
 * it a save state holds, is the same on every Android phone. */
static uint64_t layout_page(void)
{
#if defined(__aarch64__) && !defined(_WIN32)
    static uint64_t page;
    if (!page) {
        long system = sysconf(_SC_PAGESIZE);
        page = system > 16384 ? (uint64_t)system : 16384;
    }
    return page;
#else
    return PAGE;
#endif
}

/* Code first, then everything else, each part starting on a page, so the
 * code pages can be made executable and the rest left writable. A 64-bit
 * object's veneers end the code, and its GOT the rest: room for one each
 * per symbol, of which only those out of reach are used. */
static int lay_out(Loader *loader, size_t *code_size, size_t *total)
{
    int pass;
    uint64_t cursor = 0, page = layout_page();
    for (pass = 0; pass < 2; pass++) {
        unsigned i;
        for (i = 0; i < loader->section_count; i++) {
            const Section *section = &loader->sections[i];
            uint64_t align = section->addralign ? section->addralign : 1;
            if (!(section->flags & SHF_ALLOC) || !!(section->flags & SHF_EXECINSTR) != !pass) continue;
            if (section->type != SHT_PROGBITS && section->type != SHT_NOBITS) continue;
            if (align & (align - 1) || align > PAGE) {
                return fail(loader, "section %u has an alignment of %llu", i, (unsigned long long)align);
            }
            cursor = (cursor + align - 1) & ~(align - 1);
            /* A .bss size is not bounded by the file, and a 64-bit one
             * could wrap the cursor back round to a small image. */
            if (cursor > IMAGE_MAX || section->size > IMAGE_MAX - cursor) {
                return fail(loader, "is larger than %u MiB", IMAGE_MAX >> 20);
            }
            loader->place[i] = cursor;
            cursor += section->size;
        }
        if (loader->wide) {
            cursor = (cursor + 15) & ~(uint64_t)15;
            if (!pass) {
                loader->veneer_at = loader->veneer_end = cursor;
                cursor += (uint64_t)loader->symbol_count * VENEER;
            } else {
                loader->got_at = loader->got_end = cursor;
                cursor += (uint64_t)loader->symbol_count * 8;
            }
            if (cursor > IMAGE_MAX) return fail(loader, "is larger than %u MiB", IMAGE_MAX >> 20);
        }
        cursor = (cursor + page - 1) & ~(page - 1);
        if (!pass) *code_size = (size_t)cursor;
    }
    *total = (size_t)cursor;
    return *total ? 0 : fail(loader, "has no code or data");
}

static int bind_symbols(Loader *loader, unsigned char *image, ObjectResolver resolve, void *context)
{
    unsigned i, count = loader->symbol_count;
    loader->values = calloc(count ? count : 1, sizeof(*loader->values));
    loader->bound = calloc(count ? count : 1, 1);
    loader->host = calloc(count ? count : 1, 1);
    loader->veneer = calloc(count ? count : 1, sizeof(*loader->veneer));
    loader->got = calloc(count ? count : 1, sizeof(*loader->got));
    if (!loader->values || !loader->bound || !loader->host || !loader->veneer || !loader->got) {
        return fail(loader, "out of memory");
    }
    for (i = 1; i < count; i++) {
        Symbol symbol = symbol_at(loader, i);
        const char *name = string_at(loader, loader->strtab, symbol.name);
        if (!name) return fail(loader, "symbol %u has a bad name", i);
        if (symbol.type == STT_TLS) return fail(loader, "%s is thread-local, which mods may not have", name);
        if (symbol.index == SHN_UNDEF) {
            void *address;
            if (!*name) return fail(loader, "symbol %u is undefined and has no name", i);
            if (!strcmp(name, "_GLOBAL_OFFSET_TABLE_")) {
                return fail(loader, "is position-independent code; build it with tools/pc/build_mod.py");
            }
            if (!strncmp(name, "__stack_chk_", 12)) {
                return fail(loader, "uses the stack protector, which reads Linux thread storage; "
                                    "build it with tools/pc/build_mod.py");
            }
            address = resolve ? resolve(name, context) : NULL;
            if (!address && symbol.bind != STB_WEAK) return fail(loader, "needs %s, which this game does not provide", name);
#if TRANSLATED_OBJECTS
            /* Below 4 GB the runtime would take it for a guest address (a
             * pinned name the guest pass did not lower). */
            if (address && (uintptr_t)address < 0x100000000ull) {
                return fail(loader, "reaches %s at a guest address, which its code cannot use: ask the mod's author "
                                    "to rebuild it with the current SDK", name);
            }
#endif
            loader->values[i] = (uintptr_t)address;
            loader->bound[i] = 1;
            loader->host[i] = 1;
        } else if (symbol.index == SHN_ABS) {
            loader->values[i] = (uintptr_t)symbol.value;
            loader->bound[i] = 1;
        } else if (symbol.index == SHN_COMMON) {
            return fail(loader, "%s is a COMMON symbol; build with -fno-common (tools/pc/build_mod.py does)", name);
        } else if (symbol.index >= SHN_LORESERVE || symbol.index >= loader->section_count) {
            return fail(loader, "symbol %s is in section %u, which does not exist", name, symbol.index);
        } else if (loader->place[symbol.index] != NOT_LOADED) {
            void *host = NULL;
            if (symbol.value > loader->sections[symbol.index].size) {
                return fail(loader, "symbol %s lies outside its section", name);
            }
#if defined(__aarch64__)
            /* clang's -mharden-sls=blr puts a weak copy of each thunk it
             * calls (__llvm_slsblr_thunk_xN, a plain `br xN`) in every unit.
             * The game's own thunks of those names send a guest address to
             * its native function (branch_thunks.c), as a dynamic link would
             * pick the strong definition: those names, and only those, are
             * bound to the game's. */
            if (symbol.bind == STB_WEAK && !strncmp(name, "__llvm_slsblr_thunk_x", 21) && resolve) {
                host = resolve(name, context);
            }
#endif
            if (host) {
                loader->values[i] = (uintptr_t)host;
                loader->host[i] = 1;
            } else {
                loader->values[i] = (uintptr_t)(image + loader->place[symbol.index] + symbol.value);
            }
            loader->bound[i] = 1;
        }
        /* A symbol in a section that is not loaded (debugging information)
         * stays unbound; only a relocation in loaded code could use it. */
    }
    return 0;
}

static int relocate32(Loader *loader, unsigned char *image)
{
    unsigned i;
    for (i = 0; i < loader->section_count; i++) {
        const Section *table = &loader->sections[i], *target;
        uint64_t r;
        if (table->type != SHT_REL) continue;
        if (table->info >= loader->section_count || loader->place[table->info] == NOT_LOADED) continue;
        target = &loader->sections[table->info];
        if (table->link != loader->symtab_index || table->entsize != 8 || table->size % 8) {
            return fail(loader, "has a bad relocation table (section %u)", i);
        }
        for (r = 0; r < table->size / 8; r++) {
            const unsigned char *at = loader->file + table->offset + r * 8;
            uint32_t offset = u32(at), info = u32(at + 4), symbol = info >> 8, type = info & 0xff, word;
            unsigned char *place;
            uintptr_t value;
            if (type == R_386_NONE) continue;
            if (!inside(offset, 4, target->size) || target->type == SHT_NOBITS) {
                return fail(loader, "has a relocation outside its section (section %u)", table->info);
            }
            if (symbol >= loader->symbol_count || (symbol && !loader->bound[symbol])) {
                return fail(loader, "has a relocation against a symbol it does not load");
            }
            place = image + loader->place[table->info] + offset;
            value = symbol ? loader->values[symbol] : 0;
            memcpy(&word, place, 4);   /* REL: the addend is in place */
            switch (type) {
            case R_386_32:
                word += (uint32_t)value;
                break;
            case R_386_PC32:
            case R_386_PLT32:   /* nothing goes through a PLT here: a direct call */
                word += (uint32_t)value - (uint32_t)(uintptr_t)place;
                break;
            case R_386_GOT32:
            case R_386_GOT32X:
            case R_386_GOTOFF:
            case R_386_GOTPC:
                return fail(loader, "is position-independent code; build it with tools/pc/build_mod.py");
            default:
                return fail(loader, "has relocation type %u, which the mod loader does not handle", type);
            }
            memcpy(place, &word, 4);
        }
    }
    return 0;
}

#if defined(__x86_64__) || defined(__aarch64__)
static uintptr_t veneer_for(Loader *loader, unsigned char *image, unsigned symbol, uintptr_t target)
{
    unsigned char *veneer;
    if (loader->veneer[symbol]) return (uintptr_t)(image + loader->veneer[symbol] - 1);
    veneer = image + loader->veneer_end;
#if defined(__x86_64__)
    veneer[0] = 0xFF;
    veneer[1] = 0x25;          /* jmp *[rip+0]: the address right after it */
    memset(veneer + 2, 0, 4);
    memcpy(veneer + 6, &target, 8);
    veneer[14] = veneer[15] = 0xCC;
#else
    {
        /* ldr x16, #8; br x16; the address. x16 (IP0) is the linker's own
         * register for this: free at every call. */
        uint32_t code[2] = {0x58000050u, 0xD61F0200u};
        memcpy(veneer, code, sizeof(code));
        memcpy(veneer + 8, &target, 8);
    }
#endif
    loader->veneer[symbol] = (uint32_t)(loader->veneer_end + 1);
    loader->veneer_end += VENEER;
    return (uintptr_t)veneer;
}

static uintptr_t got_for(Loader *loader, unsigned char *image, unsigned symbol, uintptr_t target)
{
    if (!loader->got[symbol]) {
        memcpy(image + loader->got_end, &target, 8);
        loader->got[symbol] = (uint32_t)(loader->got_end + 1);
        loader->got_end += 8;
    }
    return (uintptr_t)(image + loader->got[symbol] - 1);
}

#endif

#if defined(__x86_64__)
static int fits32(int64_t value) { return value >= INT32_MIN && value <= INT32_MAX; }


static int relocate64(Loader *loader, unsigned char *image)
{
    unsigned i;
    char buffer[32];
    for (i = 0; i < loader->section_count; i++) {
        const Section *table = &loader->sections[i], *target;
        uint64_t r;
        if (table->type != SHT_RELA) continue;
        if (table->info >= loader->section_count || loader->place[table->info] == NOT_LOADED) continue;
        target = &loader->sections[table->info];
        if (table->link != loader->symtab_index || table->entsize != 24 || table->size % 24) {
            return fail(loader, "has a bad relocation table (section %u)", i);
        }
        for (r = 0; r < table->size / 24; r++) {
            const unsigned char *at = loader->file + table->offset + r * 24;
            uint64_t offset = u64(at), info = u64(at + 8);
            int64_t addend = (int64_t)u64(at + 16), distance;
            unsigned symbol = (unsigned)(info >> 32), type = (unsigned)(info & 0xffffffffu);
            unsigned width = type == R_X86_64_64 || type == R_X86_64_PC64 ? 8 : 4;
            unsigned char *place;
            uintptr_t value;
            if (type == R_X86_64_NONE) continue;
            if (!inside(offset, width, target->size) || target->type == SHT_NOBITS) {
                return fail(loader, "has a relocation outside its section (section %u)", table->info);
            }
            if ((info >> 32) >= loader->symbol_count || (symbol && !loader->bound[symbol])) {
                return fail(loader, "has a relocation against a symbol it does not load");
            }
            place = image + loader->place[table->info] + offset;
            value = symbol ? loader->values[symbol] : 0;
            switch (type) {
            case R_X86_64_64: {
                uint64_t word = (uint64_t)value + (uint64_t)addend;
                memcpy(place, &word, 8);
                break;
            }
            case R_X86_64_PC64: {
                uint64_t word = (uint64_t)value + (uint64_t)addend - (uint64_t)(uintptr_t)place;
                memcpy(place, &word, 8);
                break;
            }
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int32_t word;
                distance = (int64_t)(value + (uint64_t)addend - (uintptr_t)place);
                /* A call or jump to a host function out of reach (the C
                 * library, in a DLL far above the game) goes through a
                 * veneer; a far access to data has no such way round. */
                if (!fits32(distance) && loader->host[symbol] &&
                    (type == R_X86_64_PLT32 || (offset && (place[-1] == 0xE8 || place[-1] == 0xE9)))) {
                    distance = (int64_t)(veneer_for(loader, image, symbol, value) + (uint64_t)addend - (uintptr_t)place);
                }
                if (!fits32(distance)) {
                    return fail(loader, "reaches %s (at %p) from %p, too far for a 32-bit offset",
                                symbol_name(loader, symbol, buffer, sizeof(buffer)), (void *)value, (void *)place);
                }
                word = (int32_t)distance;
                memcpy(place, &word, 4);
                break;
            }
            case R_X86_64_GOTPCREL:
            case R_X86_64_GOTPCRELX:
            case R_X86_64_REX_GOTPCRELX: {
                int32_t word;
                distance = (int64_t)(got_for(loader, image, symbol, value) + (uint64_t)addend - (uintptr_t)place);
                if (!fits32(distance)) return fail(loader, "has a GOT entry out of reach");
                word = (int32_t)distance;
                memcpy(place, &word, 4);
                break;
            }
            case R_X86_64_32:
            case R_X86_64_32S: {
                uint64_t sum = (uint64_t)value + (uint64_t)addend;
                uint32_t word = (uint32_t)sum;
                if (type == R_X86_64_32 ? sum > 0xffffffffu : !fits32((int64_t)sum)) {
                    return fail(loader, "reaches %s (at %p) through a 32-bit address, and it is above that",
                                symbol_name(loader, symbol, buffer, sizeof(buffer)), (void *)value);
                }
                memcpy(place, &word, 4);
                break;
            }
            default:
                return fail(loader, "has relocation type %u (against %s), which the mod loader does not handle", type,
                            symbol_name(loader, symbol, buffer, sizeof(buffer)));
            }
        }
    }
    return 0;
}
#endif

#if defined(__aarch64__)
static int fits(int64_t value, int bits) { return value >= -((int64_t)1 << (bits - 1)) && value < ((int64_t)1 << (bits - 1)); }

static int relocate_a64(Loader *loader, unsigned char *image)
{
    unsigned i;
    char buffer[32];
    for (i = 0; i < loader->section_count; i++) {
        const Section *table = &loader->sections[i], *target;
        uint64_t r;
        if (table->type != SHT_RELA) continue;
        if (table->info >= loader->section_count || loader->place[table->info] == NOT_LOADED) continue;
        target = &loader->sections[table->info];
        if (table->link != loader->symtab_index || table->entsize != 24 || table->size % 24) {
            return fail(loader, "has a bad relocation table (section %u)", i);
        }
        for (r = 0; r < table->size / 24; r++) {
            const unsigned char *at = loader->file + table->offset + r * 24;
            uint64_t offset = u64(at), info = u64(at + 8);
            int64_t addend = (int64_t)u64(at + 16), v;
            unsigned symbol = (unsigned)(info >> 32), type = (unsigned)(info & 0xffffffffu);
            unsigned width = type == R_AARCH64_ABS64 || type == R_AARCH64_PREL64 ? 8 : 4;
            unsigned char *place;
            uintptr_t value, to;
            uint32_t insn;
            const char *name;
            if (type == 0 || type == R_AARCH64_NONE) continue;
            if (!inside(offset, width, target->size) || target->type == SHT_NOBITS) {
                return fail(loader, "has a relocation outside its section (section %u)", table->info);
            }
            if ((info >> 32) >= loader->symbol_count || (symbol && !loader->bound[symbol])) {
                return fail(loader, "has a relocation against a symbol it does not load");
            }
            place = image + loader->place[table->info] + offset;
            value = symbol ? loader->values[symbol] : 0;
            name = symbol_name(loader, symbol, buffer, sizeof(buffer));
            memcpy(&insn, place, 4);
            switch (type) {
            case R_AARCH64_ABS64: {
                uint64_t word = (uint64_t)value + (uint64_t)addend;
                memcpy(place, &word, 8);
                continue;
            }
            case R_AARCH64_PREL64: {
                uint64_t word = (uint64_t)value + (uint64_t)addend - (uint64_t)(uintptr_t)place;
                memcpy(place, &word, 8);
                continue;
            }
            case R_AARCH64_ABS32:
            case R_AARCH64_PREL32: {
                uint64_t sum = (uint64_t)value + (uint64_t)addend;
                int64_t relative = (int64_t)(sum - (uintptr_t)place);
                uint32_t word = type == R_AARCH64_ABS32 ? (uint32_t)sum : (uint32_t)relative;
                if (type == R_AARCH64_ABS32 ? sum > 0xffffffffu : !fits(relative, 32)) {
                    return fail(loader, "reaches %s (at %p) through 32 bits, and it is too far", name, (void *)value);
                }
                memcpy(place, &word, 4);
                continue;
            }
            case R_AARCH64_ADR_PREL_LO21:
                v = (int64_t)(value + (uint64_t)addend - (uintptr_t)place);
                if (!fits(v, 21)) return fail(loader, "reaches %s (at %p) with ADR, too far", name, (void *)value);
                insn = (insn & ~((3u << 29) | (0x7ffffu << 5))) | (((uint32_t)v & 3u) << 29) |
                       ((((uint32_t)v >> 2) & 0x7ffffu) << 5);
                break;
            case R_AARCH64_ADR_PREL_PG_HI21:
            case R_AARCH64_ADR_PREL_PG_HI21_NC:
            case R_AARCH64_ADR_GOT_PAGE:
                to = type == R_AARCH64_ADR_GOT_PAGE ? got_for(loader, image, symbol, value) : value + (uint64_t)addend;
                v = ((int64_t)(to & ~(uintptr_t)0xfff) - (int64_t)((uintptr_t)place & ~(uintptr_t)0xfff)) >> 12;
                if (type != R_AARCH64_ADR_PREL_PG_HI21_NC && !fits(v, 21)) {
                    return fail(loader, "reaches %s (at %p) with ADRP, more than 4 GB away", name, (void *)value);
                }
                insn = (insn & ~((3u << 29) | (0x7ffffu << 5))) | (((uint32_t)v & 3u) << 29) |
                       ((((uint32_t)v >> 2) & 0x7ffffu) << 5);
                break;
            case R_AARCH64_ADD_ABS_LO12_NC:
            case R_AARCH64_LDST8_ABS_LO12_NC:
            case R_AARCH64_LDST16_ABS_LO12_NC:
            case R_AARCH64_LDST32_ABS_LO12_NC:
            case R_AARCH64_LDST64_ABS_LO12_NC:
            case R_AARCH64_LDST128_ABS_LO12_NC:
            case R_AARCH64_LD64_GOT_LO12_NC: {
                unsigned shift = type == R_AARCH64_LDST16_ABS_LO12_NC ? 1 : type == R_AARCH64_LDST32_ABS_LO12_NC ? 2
                               : type == R_AARCH64_LDST64_ABS_LO12_NC || type == R_AARCH64_LD64_GOT_LO12_NC ? 3
                               : type == R_AARCH64_LDST128_ABS_LO12_NC ? 4 : 0;
                uint32_t low;
                to = type == R_AARCH64_LD64_GOT_LO12_NC ? got_for(loader, image, symbol, value) : value + (uint64_t)addend;
                low = (uint32_t)(to & 0xfff);
                if (low & ((1u << shift) - 1)) {
                    return fail(loader, "has a misaligned access to %s (relocation type %u)", name, type);
                }
                insn = (insn & ~(0xfffu << 10)) | ((low >> shift) << 10);
                break;
            }
            case R_AARCH64_JUMP26:
            case R_AARCH64_CALL26:
                v = (int64_t)(value + (uint64_t)addend - (uintptr_t)place);
                if (!fits(v, 28) && loader->host[symbol]) {
                    /* A host function out of the branch's 128 MB (bionic, far
                     * above the game): through a veneer in the image. */
                    v = (int64_t)(veneer_for(loader, image, symbol, value + (uint64_t)addend) - (uintptr_t)place);
                }
                if (!fits(v, 28)) return fail(loader, "branches to %s (at %p), out of reach", name, (void *)value);
                insn = (insn & 0xfc000000u) | (((uint32_t)(v >> 2)) & 0x03ffffffu);
                break;
            case R_AARCH64_CONDBR19:
                v = (int64_t)(value + (uint64_t)addend - (uintptr_t)place);
                if (!fits(v, 21)) return fail(loader, "branches to %s (at %p), out of reach", name, (void *)value);
                insn = (insn & ~(0x7ffffu << 5)) | ((((uint32_t)(v >> 2)) & 0x7ffffu) << 5);
                break;
            case R_AARCH64_TSTBR14:
                v = (int64_t)(value + (uint64_t)addend - (uintptr_t)place);
                if (!fits(v, 16)) return fail(loader, "branches to %s (at %p), out of reach", name, (void *)value);
                insn = (insn & ~(0x3fffu << 5)) | ((((uint32_t)(v >> 2)) & 0x3fffu) << 5);
                break;
            default:
                return fail(loader, "has relocation type %u (against %s), which the mod loader does not handle", type,
                            name);
            }
            memcpy(place, &insn, 4);
        }
    }
    return 0;
}
#endif

static int relocate(Loader *loader, unsigned char *image)
{
    if (!loader->wide) return relocate32(loader, image);
#if defined(__x86_64__)
    return relocate64(loader, image);
#elif defined(__aarch64__)
    return relocate_a64(loader, image);
#else
    (void)image;
    return fail(loader, "is 64-bit code, which this game cannot link");
#endif
}

/* The object's own functions and variables, kept for ObjectLoader_Symbol
 * and for crash reports. Their names point into a copy of the string table,
 * whose entries bind_symbols has already found terminated. */
static int keep_symbols(Loader *loader, LoadedObject *object)
{
    unsigned i, count = loader->symbol_count;
    object->strings = malloc(loader->strtab->size ? (size_t)loader->strtab->size : 1);
    object->symbols = calloc(count ? count : 1, sizeof(*object->symbols));
    if (!object->strings || !object->symbols) return fail(loader, "out of memory");
    memcpy(object->strings, loader->file + loader->strtab->offset, (size_t)loader->strtab->size);
    for (i = 1; i < count; i++) {
        Symbol symbol = symbol_at(loader, i);
        struct ObjectSymbol *kept;
        if ((symbol.type != STT_FUNC && symbol.type != STT_OBJECT) || symbol.index == SHN_UNDEF ||
            symbol.index >= loader->section_count || loader->place[symbol.index] == NOT_LOADED ||
            !object->strings[symbol.name]) {
            continue;
        }
        kept = &object->symbols[object->symbol_count++];
        kept->name = object->strings + symbol.name;
        kept->address = loader->values[i];
        kept->size = (uintptr_t)symbol.size;
        kept->function = symbol.type == STT_FUNC;
        kept->global = symbol.bind != STB_LOCAL;
    }
    return 0;
}

static uint32_t mix(uint32_t hash, const void *data, size_t size)
{
    const unsigned char *at = data;
    while (size--) hash = (hash ^ *at++) * 16777619u;
    return hash;
}

static uint32_t mix32(uint32_t hash, uint32_t value)
{
    unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16),
                              (unsigned char)(value >> 24)};
    return mix(hash, bytes, 4);
}

static uint32_t mix64(uint32_t hash, uint64_t value)
{
    return mix32(mix32(hash, (uint32_t)value), (uint32_t)(value >> 32));
}

/* A symbol as a relocation sees it, in terms that do not depend on the
 * symbol table's order: a host name, a constant, or a place in the image.
 * (Places and section sizes are below IMAGE_MAX, so 32 bits hold them.) */
static uint32_t mix_symbol(const Loader *loader, uint32_t hash, unsigned i)
{
    Symbol symbol;
    if (!i) return mix32(hash, 0);
    symbol = symbol_at(loader, i);
    if (symbol.index == SHN_UNDEF) {
        const char *name = string_at(loader, loader->strtab, symbol.name);
        return mix(mix32(hash, 1), name, strlen(name) + 1);
    }
    if (symbol.index == SHN_ABS) {
        hash = mix32(mix32(hash, 2), (uint32_t)symbol.value);
        return loader->wide ? mix32(hash, (uint32_t)(symbol.value >> 32)) : hash;
    }
    return mix32(mix32(mix32(hash, 3), (uint32_t)loader->place[symbol.index]), (uint32_t)symbol.value);
}

/* LoadedObject.hash. Only called once relocate and keep_symbols have
 * checked every index and name it reads. A 32-bit object hashes as it
 * always has (save states keep the hash); a 64-bit one also takes in each
 * relocation's addend, which its RELA table holds instead of the code. */
static uint32_t fingerprint(const Loader *loader)
{
    uint32_t hash = 2166136261u;
    unsigned i, count = loader->symbol_count;
    unsigned relocations = loader->wide ? SHT_RELA : SHT_REL, size = loader->wide ? 24 : 8;
    for (i = 0; i < loader->section_count; i++) {
        const Section *section = &loader->sections[i];
        if (loader->place[i] == NOT_LOADED) continue;
        hash = mix32(mix32(mix32(hash, (uint32_t)loader->place[i]), (uint32_t)section->size), section->type);
        if (section->type == SHT_PROGBITS) hash = mix(hash, loader->file + section->offset, (size_t)section->size);
    }
    for (i = 0; i < loader->section_count; i++) {
        const Section *table = &loader->sections[i];
        uint64_t r;
        if (table->type != relocations || table->info >= loader->section_count ||
            loader->place[table->info] == NOT_LOADED) {
            continue;
        }
        for (r = 0; r < table->size / size; r++) {
            const unsigned char *at = loader->file + table->offset + r * size;
            uint32_t offset = loader->wide ? (uint32_t)u64(at) : u32(at);
            uint32_t type = loader->wide ? u32(at + 8) : (u32(at + 4) & 0xff);
            unsigned symbol = loader->wide ? u32(at + 12) : u32(at + 4) >> 8;
            if (type == 0) continue;   /* R_386_NONE, R_X86_64_NONE */
            hash = mix32(mix32(mix32(hash, (uint32_t)loader->place[table->info]), offset), type);
            hash = mix_symbol(loader, hash, symbol);
            if (loader->wide) hash = mix64(hash, u64(at + 16));
        }
    }
    /* The names the game looks the mod up by (MemoriesModInit). */
    for (i = 1; i < count; i++) {
        Symbol symbol = symbol_at(loader, i);
        const char *name = string_at(loader, loader->strtab, symbol.name);
        if (symbol.bind == STB_LOCAL || symbol.index == SHN_UNDEF || symbol.index >= loader->section_count ||
            loader->place[symbol.index] == NOT_LOADED) {
            continue;
        }
        hash = mix(hash, name, strlen(name) + 1);
        hash = mix_symbol(loader, hash, i);
    }
    return hash;
}

#if WIDE && TRANSLATED_OBJECTS
/* The image's memory in the translated game: anywhere the system puts it,
 * as long as it is above 4 GB. The code reaches the game through its GOT
 * entries and veneers, and a guest slot holds a token for one of its
 * functions, not the function's address. */
static unsigned char *map_image(size_t size)
{
    void *got = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got == MAP_FAILED) return NULL;
    if ((uintptr_t)got < 0x100000000ull) {
        munmap(got, size);
        return NULL;
    }
    return got;
}
#elif WIDE
/* Where a 64-bit game wants mod code: src/pc/guest/image.c holds a range
 * for it below 4 GB, so that a mod's function fits a game's 4-byte slot as
 * the game's own do. It returns 1 for a range compat/mman.h's mmap takes
 * back piece by piece (Windows), 2 for one the game holds as an
 * inaccessible mapping of its own, which images are mapped over in turn
 * (Linux kernels: MAP_FIXED_NOREPLACE refuses it). Weak: the loader's tests
 * have no game. */
int Memories_ModCodeRange(uintptr_t *start, uintptr_t *end) __attribute__((weak));
static uintptr_t held_next;   /* the next free place in a held range */

/* The branch thunks' fast path (src/pc/guest/branch_thunks.c) takes an
 * address for host code when any of bits 21-28 or 30 is set: every 2 MiB
 * piece of the image must pass that, as the game's own code does. */
static int low_and_fast(uintptr_t start, size_t size)
{
    uintptr_t at;
    if ((uint64_t)start + size > 0x100000000ull) return 0;
    for (at = start; at < start + size; at += 0x200000u - (at & 0x1fffffu)) {
        if (!(at & 0x5fe00000u)) return 0;
    }
    return 1;
}

static unsigned char *map_at(uintptr_t at, size_t size)
{
    void *got = Memories_MapFixed((void *)at, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return got == MAP_FAILED ? NULL : got;
}

/* The image's memory: the first free place in the game's range, in 64 KiB
 * steps (Windows' allocation granularity). A game that could not hold its
 * range loads no code mod: anywhere else a mod's function might not fit a
 * 4-byte slot. Without a game (the tests), the first free place after this
 * code, which its calls into the host reach. */
static unsigned char *map_image(size_t size)
{
    uintptr_t start, end, at;
    int kind = Memories_ModCodeRange ? Memories_ModCodeRange(&start, &end) : 0;
    if (kind == 2) {
        /* Ours to map over, in turn: images never share a page. */
        void *got;
        at = held_next > start ? held_next : (start + 0xffffu) & ~(uintptr_t)0xffffu;
        if (at + size > end || at + size < at || !low_and_fast(at, size)) return NULL;
        got = mmap((void *)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (got != (void *)at) return NULL;
        held_next = (at + size + 0xffffu) & ~(uintptr_t)0xffffu;
        return got;
    }
    if (kind) {
        for (at = (start + 0xffffu) & ~(uintptr_t)0xffffu; at + size <= end && at + size > at; at += 0x10000u) {
            unsigned char *image;
            if (!low_and_fast(at, size)) continue;
            if ((image = map_at(at, size)) != NULL) return image;
        }
        return NULL;
    }
    if (Memories_ModCodeRange) return NULL;
    start = ((uintptr_t)map_image + 0xffffu) & ~(uintptr_t)0xffffu;
    for (at = start; at - start < (1u << 30); at += 0x10000u) {
        unsigned char *image = map_at(at, size);
        if (image) return image;
    }
    return NULL;
}
#endif

int ObjectLoader_Load(const void *data, size_t size, ObjectResolver resolve, void *context,
                      LoadedObject *object, char *error, size_t error_size)
{
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
    /* This loader's object format and relocations are i386. */
    (void)data; (void)size; (void)resolve; (void)context;
    memset(object, 0, sizeof(*object));
    if (error_size) snprintf(error, error_size, "requires a macOS ARM64 dylib (build_mod.py --target macos)");
    return -1;
#else
    Loader loader;
    unsigned char *image = NULL;
    size_t code_size = 0, total = 0;
    unsigned i;
    int result = -1;
    memset(object, 0, sizeof(*object));
    memset(&loader, 0, sizeof(loader));
    loader.file = data;
#ifdef MEMORIES_NO_CODE_MODS
    /* Mods.c refuses code mods before this, in a build made without them. */
    snprintf(error, error_size, "code mods are not loaded by this game yet");
    return result;
#endif
    loader.file_size = size;
    loader.error = error;
    loader.error_size = error_size;
    if (error_size) error[0] = '\0';
    if (!data) {
        fail(&loader, "is empty");
        goto done;
    }
    if (read_sections(&loader) || lay_out(&loader, &code_size, &total)) goto done;
#if WIDE
    image = map_image(total);
    if (!image) {
#if TRANSLATED_OBJECTS
        fail(&loader, "finds no memory for its %u bytes above 4 GB, where the translated game keeps mod code",
             (unsigned)total);
#else
        fail(&loader, "finds no room for its %u bytes where the game's code can call it", (unsigned)total);
#endif
        goto done;
    }
#else
    image = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image == MAP_FAILED) {
        image = NULL;
        fail(&loader, "does not fit in memory (%u bytes)", (unsigned)total);
        goto done;
    }
#endif
    for (i = 0; i < loader.section_count; i++) {
        const Section *section = &loader.sections[i];
        if (loader.place[i] != NOT_LOADED && section->type == SHT_PROGBITS && section->size) {
            memcpy(image + loader.place[i], loader.file + section->offset, (size_t)section->size);
        }
    }
    object->image = image;
    object->size = total;
    object->code_size = code_size;
    if (bind_symbols(&loader, image, resolve, context) || relocate(&loader, image) || keep_symbols(&loader, object)) {
        goto done;
    }
    object->hash = fingerprint(&loader);
    if (code_size && mprotect(image, code_size, PROT_READ | PROT_EXEC)) {
        fail(&loader, "could not make its code executable (%s)", strerror(errno));
        goto done;
    }
#if defined(__aarch64__)
    __builtin___clear_cache((char *)image, (char *)image + code_size);   /* AArch64's caches are not coherent */
#endif
#if TRANSLATED_OBJECTS
    {
        /* The guest pass's registration pair (build_mod.py): the globals
         * join the translated runtime before MemoriesModInit runs. */
        union { void *pointer; void (*function)(void); } registration, cleanup;
        registration.pointer = ObjectLoader_Symbol(object, "MemoriesModRegisterGlobals");
        cleanup.pointer = ObjectLoader_Symbol(object, "MemoriesModUnregisterGlobals");
        if (!registration.pointer || !cleanup.pointer) {
            fail(&loader, "was not built through the guest pass (no MemoriesModRegisterGlobals): ask the mod's "
                          "author to rebuild it (build_mod.py --target aarch64-translated)");
            goto done;
        }
        /* Registering past the end of the token arena aborts the game:
         * refused here instead, with every byte the image's data could
         * need (each global starts on 16 bytes). */
        if (total - code_size + 16u * loader.symbol_count > GuestRuntime_AutomaticSpace()) {
            fail(&loader, "has more data (%u bytes) than the game has room for, with the mods loaded before it",
                 (unsigned)(total - code_size));
            goto done;
        }
        registration.function();
        object->unregister = cleanup.function;
    }
#endif
    result = 0;
done:
    if (result) ObjectLoader_Free(object);
    free(loader.sections);
    free(loader.place);
    free(loader.values);
    free(loader.bound);
    free(loader.host);
    free(loader.veneer);
    free(loader.got);
    return result;
#endif
}

void *ObjectLoader_Symbol(const LoadedObject *object, const char *name)
{
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
    if (object && object->native_handle && name) return dlsym(object->native_handle, name);
#endif
    size_t i;
    for (i = 0; object && name && i < object->symbol_count; i++) {
        if (object->symbols[i].global && !strcmp(object->symbols[i].name, name)) {
            return (void *)object->symbols[i].address;
        }
    }
    return NULL;
}

void ObjectLoader_Free(LoadedObject *object)
{
#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
    if (object->native_handle) {
        void (*cleanup)(void) = dlsym(object->native_handle, "MemoriesModUnregisterGlobals");
        if (cleanup) cleanup();
        dlclose(object->native_handle);
    }
#endif
    if (object->unregister) object->unregister();
    if (object->image) munmap(object->image, object->size);
    free(object->symbols);
    free(object->strings);
    memset(object, 0, sizeof(*object));
}

#if defined(__APPLE__) && defined(__aarch64__) && defined(MEMORIES_TRANSLATED)
int ObjectLoader_LoadPath(const char *path, LoadedObject *object, char *error, size_t error_size)
{
    struct mach_header_64 header;
    unsigned char buffer[4096];
    size_t count;
    size_t total = sizeof(header);
    uint32_t hash = 2166136261u;
    FILE *file;
    memset(object, 0, sizeof(*object));
    if (error_size) error[0] = 0;
    file = fopen(path, "rb");
    if (!file) goto invalid;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.filetype != MH_DYLIB) {
        fclose(file);
        goto invalid;
    }
    hash = mix(hash, (const unsigned char *)&header, sizeof(header));
    while ((count = fread(buffer, 1, sizeof(buffer), file))) {
        if (count > IMAGE_MAX - total) { fclose(file); goto invalid; }
        total += count;
        hash = mix(hash, buffer, count);
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) goto invalid;
    object->native_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!object->native_handle) {
        if (error_size) snprintf(error, error_size, "cannot load ARM64 code: %s", dlerror());
        return -1;
    }
    void (*registration)(void) = dlsym(object->native_handle, "MemoriesModRegisterGlobals");
    if (!registration || !dlsym(object->native_handle, "MemoriesModInit")) {
        if (error_size) snprintf(error, error_size, "missing ARM64 mod entry/registration; rebuild with build_mod.py --target macos");
        dlclose(object->native_handle);
        memset(object, 0, sizeof(*object));
        return -1;
    }
    registration();
    object->hash = hash;
    return 0;
invalid:
    if (error_size) snprintf(error, error_size, "requires a macOS ARM64 dylib (build_mod.py --target macos)");
    return -1;
}
#endif
