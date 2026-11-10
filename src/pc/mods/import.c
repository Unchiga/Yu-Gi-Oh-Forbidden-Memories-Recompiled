/* A mod's .zip into the mods folder (import.h). The .zip is read through
 * its central directory, which is where the names, sizes and file types
 * are; each entry's data is found through its local header and inflated
 * with zlib (raw deflate), which every build links already (libpng's).
 * ZIP64 and multi-disk archives are refused: they start where the plain
 * form's 4 GB and 65,535 entries end, which MODS_IMPORT_BYTES and
 * MODS_IMPORT_ENTRIES keep below. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "import.h"
#include "json.h"
#include "manifest_code.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#define ZIP_MAX MODS_IMPORT_ZIP_BYTES
#define DIRECTORY_MAX (32u << 20)       /* its central directory */
#define MANIFEST_MAX (1536u << 20)      /* a mod.json read to name the mod: the fusions of 25 More Cards */
#define NAME_MAX_ 512
#define CHUNK 65536
#define PARTS_MAX 32                    /* folders deep, in a name */

typedef struct {
    char *name;   /* '/' between parts; a folder's ends with '/' */
    unsigned method, crc, packed, size;
    unsigned long offset;
    int folder;
} Entry;

struct ModsImport {
    FILE *file;
    unsigned long directory; /* where the central directory starts: the data ends before it */
    Entry *entries;
    int count;
    unsigned long long total;
    ModsImportMod mods[MODS_IMPORT_MAX];
    int mod_count;
    unsigned *done;     /* Mods_ImportWatch: bytes unpacked so far (atomic), or NULL */
    const int *cancel;  /* and nonzero (atomic) to stop, at the next chunk */
    void (*stepped)(void); /* Mods_ImportStepped: called after each chunk written */
};

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const unsigned char *p) { return le16(p) | (unsigned)le16(p + 2) << 16; }

static int read_at(FILE *file, unsigned long offset, void *out, size_t size)
{
    return offset <= ZIP_MAX && !fseek(file, (long)offset, SEEK_SET) &&
           fread(out, 1, size, file) == size;
}

/* Well-formed UTF-8: lead bytes with their continuations, nothing overlong
 * or past U+10FFFF, no surrogates. */
static int utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i], need, code;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
            need = 1, code = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 2, code = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 3, code = c & 0x07;
        else
            return 0;
        if (i + need >= n)
            return 0;
        for (unsigned k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            code = code << 6 | (s[i + k] & 0x3F);
        }
        if ((need == 2 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
            (need == 3 && (code < 0x10000 || code > 0x10FFFF)))
            return 0;
        i += need + 1;
    }
    return 1;
}

/* Windows' tools write a name in the system's OEM code page and leave out the
 * .zip's UTF-8 flag. The format says code page 437; code page 850, Western
 * Windows', keeps 437's accented letters where they are and adds the ones 437
 * lacks (ã, õ, Á) in place of its line art and Greek letters. */
static const unsigned short CP850[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
    0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
    0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
    0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
    0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
    0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
    0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

/* `raw` from code page 850 to UTF-8 in `out`; its length, 0 when it does not fit. */
static unsigned cp850_name(const unsigned char *raw, unsigned length, unsigned char *out, unsigned size)
{
    unsigned n = 0;
    for (unsigned i = 0; i < length; i++) {
        unsigned code = raw[i] < 0x80 ? raw[i] : CP850[raw[i] - 0x80];
        if (n + 3 >= size)
            return 0;
        if (code < 0x80) {
            out[n++] = (unsigned char)code;
        } else if (code < 0x800) {
            out[n++] = (unsigned char)(0xC0 | code >> 6);
            out[n++] = (unsigned char)(0x80 | (code & 0x3F));
        } else {
            out[n++] = (unsigned char)(0xE0 | code >> 12);
            out[n++] = (unsigned char)(0x80 | (code >> 6 & 0x3F));
            out[n++] = (unsigned char)(0x80 | (code & 0x3F));
        }
    }
    return n;
}

/* The entry's name as it goes on disk, or 0 when it would leave the folder
 * it is unpacked in or is not a name this writes: backslashes become
 * slashes; no leading slash, no drive letter or other ':', no empty, "."
 * or ".." part, no control characters, well-formed UTF-8. */
static int clean_name(const unsigned char *raw, unsigned length, char *out, int *folder)
{
    unsigned i, start = 0, parts;
    if (!length || length >= NAME_MAX_ || !utf8(raw, length))
        return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = raw[i];
        if (c < 0x20 || c == 0x7F || c == ':')
            return 0;
        out[i] = c == '\\' ? '/' : (char)c;
    }
    out[length] = 0;
    *folder = out[length - 1] == '/';
    if (*folder)
        length--;
    if (!length || out[0] == '/')
        return 0;
    for (i = 0, parts = 0; i <= length; i++) {
        if (i == length || out[i] == '/') {
            unsigned part = i - start;
            if (!part || (part == 1 && out[start] == '.') || (part == 2 && out[start] == '.' && out[start + 1] == '.'))
                return 0;
            start = i + 1;
            if (++parts > PARTS_MAX)
                return 0;
        }
    }
    return 1;
}

/* `text` cut to fit `size`, never inside a UTF-8 character (a manifest's
 * name may be longer than the room for it). */
static void copy(char *out, size_t size, const char *text)
{
    size_t n = strlen(text);
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, text, n);
    out[n] = 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* Whether `name` is inside the folder `prefix` (ending in '/', or "" for
 * the root), letters compared as a case-blind file system would. */
static int prefixed(const char *name, const char *prefix)
{
    for (; *prefix; prefix++, name++)
        if (lower((unsigned char)*prefix) != lower((unsigned char)*name))
            return 0;
    return 1;
}
/* Names compared as a case-blind file system would (Android's shared
 * storage is one): a folder's slash left out, ASCII letters folded. */
static int same_name(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return (!*a && (!*b || (*b == '/' && !b[1]))) || (!*b && *a == '/' && !a[1]);
}
static int name_order(const void *x, const void *y)
{
    const char *a = ((const Entry *)x)->name, *b = ((const Entry *)y)->name;
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    /* a folder sorts as its name, so "a/" is beside "a" */
    return (*a == '/' && !a[1] ? 0 : lower((unsigned char)*a)) - (*b == '/' && !b[1] ? 0 : lower((unsigned char)*b));
}

static int skipped(const char *name) { return !strncmp(name, "__MACOSX/", 9); }

/* Inflates (or copies) entry `e` to `out`, or to `memory` (size + 1 bytes,
 * the text ended) when `out` is NULL. Checks the local header, the sizes
 * and the CRC: nothing past the size the directory gives is written. */
typedef struct {
    FILE *out;
    unsigned char *memory;
    unsigned long written, size, crc;
    unsigned *progress; /* bytes written so far (atomic, added to as they are), or NULL */
    void (*stepped)(void); /* Mods_ImportStepped, or NULL */
} Sink;
/* Whether Mods_ImportWatch's cancel is set (the game thread's Stop). */
static int stop_asked(const ModsImport *import)
{
    return import->cancel && __atomic_load_n(import->cancel, __ATOMIC_ACQUIRE);
}
static int emit(Sink *sink, const unsigned char *bytes, size_t n)
{
    if (n > sink->size - sink->written)
        return -1;
    sink->crc = crc32(sink->crc, bytes, (uInt)n);
    if (sink->out ? fwrite(bytes, 1, n, sink->out) != n : (memcpy(sink->memory + sink->written, bytes, n), 0))
        return 0;
    sink->written += (unsigned long)n;
    if (sink->progress)
        __atomic_add_fetch(sink->progress, (unsigned)n, __ATOMIC_RELEASE);
    if (sink->stepped)
        sink->stepped();
    return 1;
}
static int unpack(ModsImport *import, const Entry *e, FILE *out, unsigned char *memory, char *why, size_t why_size)
{
    unsigned char header[30], *in = NULL, *buffer = NULL;
    unsigned long data, left = e->packed;
    Sink sink = {out, memory, 0, e->size, 0, NULL, NULL};
    z_stream stream;
    int ok = 0, started = 0, status = Z_OK, wrote;
    sink.crc = crc32(0L, Z_NULL, 0);
    sink.progress = out ? import->done : NULL;
    sink.stepped = out ? import->stepped : NULL;
    if (!read_at(import->file, e->offset, header, sizeof(header)) || le32(header) != 0x04034b50u) {
        snprintf(why, why_size, "The .zip is damaged (%s).", e->name);
        return 0;
    }
    data = e->offset + 30 + le16(header + 26) + le16(header + 28);
    if (data > import->directory || e->packed > import->directory - data ||
        (e->method == 0 && e->packed != e->size)) {
        snprintf(why, why_size, "The .zip is damaged (%s).", e->name);
        return 0;
    }
    if (!(in = malloc(CHUNK)) || !(buffer = malloc(CHUNK))) {
        snprintf(why, why_size, "Out of memory while unpacking the .zip.");
        goto done;
    }
    if (fseek(import->file, (long)data, SEEK_SET)) {
        snprintf(why, why_size, "The .zip could not be read (%s).", e->name);
        goto done;
    }
    memset(&stream, 0, sizeof(stream));
    if (e->method == 8) {
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            snprintf(why, why_size, "Could not start unpacking the .zip.");
            goto done;
        }
        started = 1;
    }
    for (;;) {
        size_t got = 0;
        if (stop_asked(import)) { /* a big file stops too */
            snprintf(why, why_size, "Cancelled.");
            goto done;
        }
        if (left) {
            got = fread(in, 1, left < CHUNK ? left : CHUNK, import->file);
            if (!got) {
                snprintf(why, why_size, "The .zip could not be read (%s).", e->name);
                goto done;
            }
            left -= (unsigned long)got;
        }
        if (e->method == 0) {
            if ((wrote = emit(&sink, in, got)) <= 0)
                goto failed;
            if (!left)
                break;
            continue;
        }
        stream.next_in = in;
        stream.avail_in = (uInt)got;
        do {
            if (stop_asked(import)) { /* a chunk of the .zip can unpack to many */
                snprintf(why, why_size, "Cancelled.");
                goto done;
            }
            stream.next_out = buffer;
            stream.avail_out = CHUNK;
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
                snprintf(why, why_size, "The .zip is damaged (%s).", e->name);
                goto done;
            }
            if ((wrote = emit(&sink, buffer, CHUNK - stream.avail_out)) <= 0)
                goto failed;
        } while (status != Z_STREAM_END && (stream.avail_in || !stream.avail_out));
        if (status == Z_STREAM_END)
            break;
        if (!left) { /* all of it read, and the stream not ended */
            snprintf(why, why_size, "The .zip is damaged (%s).", e->name);
            goto done;
        }
    }
    if (sink.written != e->size || sink.crc != e->crc) {
        snprintf(why, why_size, "The .zip is damaged: %s does not match its checksum.", e->name);
        goto done;
    }
    if (memory)
        memory[sink.written] = 0;
    ok = 1;
    goto done;
failed:
    if (wrote < 0)
        snprintf(why, why_size, "The .zip is damaged: %s is larger than it says.", e->name);
    else
        snprintf(why, why_size, "Could not write %s: %s.", e->name, strerror(errno));
done:
    if (started)
        inflateEnd(&stream);
    free(in);
    free(buffer);
    return ok;
}

static const Entry *find(const ModsImport *import, const char *name)
{
    for (int i = 0; i < import->count; i++)
        if (!strcmp(import->entries[i].name, name))
            return &import->entries[i];
    return NULL;
}

static int id_valid(const char *s)
{
    size_t n = strlen(s);
    return n && n < 64 &&
           strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == n;
}
/* A folder name every system takes as it is: no character Windows refuses,
 * not hidden, not ending in a dot or a space. */
static int folder_valid(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof(((ModsImportMod *)0)->folder) || s[0] == '.' || s[n - 1] == '.' || s[n - 1] == ' ')
        return 0;
    return !strpbrk(s, "<>:\"/\\|?*");
}

/* What the mod in `prefix` is called and whether it carries code, from its
 * mod.json, as the loader reads them (mods.c read_manifest): the id from the
 * manifest, else the folder; code by the loader's own test
 * (Manifest_HasCode): a "library", or "libraries" with any entry, whether
 * or not the .zip has an object for this game. */
static int describe(ModsImport *import, ModsImportMod *mod, const Entry *manifest, const char *zip, char *why,
                    size_t why_size)
{
    char unpacked[1200];
    FILE *out;
    static const char *const lists[] = {"fusions", "cards"};
    JsonDeferred deferred[2];
    JsonDocument *document;
    const JsonValue *root;
    const char *id, *name, *slash;
    char folder[NAME_MAX_], error[128];
    size_t plen = strlen(mod->prefix);
    /* The loader reads the whole file, so a mod.json this game would not
     * read is refused here, not named from its folder. */
    if (manifest->size > MANIFEST_MAX) {
        snprintf(why, why_size, "The mod.json in the .zip is too large (more than %u MB). Nothing was imported.",
                 MANIFEST_MAX >> 20);
        return 0;
    }
    /* Read as the loader reads it, which needs a file: it goes beside the .zip,
     * as the fetched .zip's own ".incoming" leftovers do, and is gone below. */
    if (snprintf(unpacked, sizeof(unpacked), "%s.mod-json", zip) >= (int)sizeof(unpacked) ||
        !(out = fopen(unpacked, "wb"))) {
        snprintf(why, why_size, "The mod.json in the .zip could not be unpacked: %s. Nothing was imported.",
                 strerror(errno));
        return 0;
    }
    if (!unpack(import, manifest, out, NULL, why, why_size)) {
        fclose(out);
        remove(unpacked);
        return 0;
    }
    if (fclose(out)) {
        snprintf(why, why_size, "The mod.json in the .zip could not be unpacked: %s. Nothing was imported.",
                 strerror(errno));
        remove(unpacked);
        return 0;
    }
    document = Json_ParseFileDeferring(unpacked, lists, 2, deferred, error, sizeof(error));
    remove(unpacked);
    /* Its copy did not fit (no reason given) or its nodes did not. */
    if (!document && (!error[0] || !strcmp(error, "out of memory"))) {
        snprintf(why, why_size, "The mod.json in the .zip is too large to read. Nothing was imported.");
        return 0;
    }
    root = Json_Root(document);
    /* The folder it is in, from the .zip ("" for a mod at its root). */
    folder[0] = 0;
    if (plen) {
        memcpy(folder, mod->prefix, plen - 1);
        folder[plen - 1] = 0;
        slash = strrchr(folder, '/');
        if (slash)
            memmove(folder, slash + 1, strlen(slash));
    }
    id = Json_String(Json_Member(root, "id"), NULL);
    if (!id || !*id)
        id = folder[0] ? folder : NULL;
    copy(mod->id, sizeof(mod->id), id && id_valid(id) ? id : "");
    if (folder_valid(folder))
        copy(mod->folder, sizeof(mod->folder), folder);
    else if (mod->id[0])
        copy(mod->folder, sizeof(mod->folder), mod->id);
    else if (!plen)
        snprintf(mod->folder, sizeof(mod->folder), "imported-mod");
    else {
        snprintf(why, why_size, "A mod's folder in the .zip has a name this game cannot use: %s.", folder);
        Json_Free(document);
        return 0;
    }
    if (!mod->id[0])
        copy(mod->id, sizeof(mod->id), mod->folder);
    name = Json_String(Json_Member(root, "name"), NULL);
    copy(mod->name, sizeof(mod->name), name && *name ? name : mod->id);
    mod->code = Manifest_HasCode(root);
    Json_Free(document);
    return 1;
}

/* Shallowest first (a mod before anything inside it), then by name. */
static int depth(const char *s)
{
    int n = 0;
    for (; *s; s++)
        n += *s == '/';
    return n;
}
static int by_depth(const void *x, const void *y)
{
    const char *a = ((const ModsImportMod *)x)->prefix, *b = ((const ModsImportMod *)y)->prefix;
    return depth(a) != depth(b) ? depth(a) - depth(b) : strcmp(a, b);
}

ModsImport *Mods_ImportOpen(const char *zip, char *why, size_t why_size)
{
    ModsImport *import = calloc(1, sizeof(*import));
    unsigned char *tail = NULL, *directory = NULL, *p, *end;
    ModsImportMod *found_mods = NULL;
    long size;
    unsigned long tail_size, eocd = 0, start, length;
    unsigned entries;
    int found = 0;
    if (!import) {
        snprintf(why, why_size, "Out of memory.");
        return NULL;
    }
    if (!(import->file = fopen(zip, "rb"))) {
        snprintf(why, why_size, "Could not read the .zip: %s.", strerror(errno));
        goto fail;
    }
    if (fseek(import->file, 0, SEEK_END) || (size = ftell(import->file)) < 0 || (unsigned long)size > ZIP_MAX) {
        snprintf(why, why_size, "The .zip is too large (more than %lu MB).", (ZIP_MAX >> 20) + 1);
        goto fail;
    }
    if (size < 22) {
        snprintf(why, why_size, "That file is not a .zip.");
        goto fail;
    }
    /* The end record, at most a comment (64 KB) from the end. */
    tail_size = (unsigned long)size < 65557ul ? (unsigned long)size : 65557ul;
    if (!(tail = malloc(tail_size)) || !read_at(import->file, (unsigned long)size - tail_size, tail, tail_size)) {
        snprintf(why, why_size, "Could not read the .zip.");
        goto fail;
    }
    for (unsigned long at = tail_size - 22 + 1; at-- > 0;)
        if (le32(tail + at) == 0x06054b50u) {
            eocd = at;
            found = 1;
            break;
        }
    if (!found) {
        snprintf(why, why_size, "That file is not a .zip.");
        goto fail;
    }
    p = tail + eocd;
    entries = le16(p + 10);
    length = le32(p + 12);
    start = le32(p + 16);
    if (le16(p + 4) || le16(p + 6) || le16(p + 8) != entries) {
        snprintf(why, why_size, "The .zip is split into parts; this game reads only a whole one.");
        goto fail;
    }
    if (entries == 0xFFFF || length == 0xFFFFFFFFul || start == 0xFFFFFFFFul) {
        snprintf(why, why_size, "The .zip is in ZIP64 form, which this game does not read; zip the mod again.");
        goto fail;
    }
    if (entries > MODS_IMPORT_ENTRIES) {
        snprintf(why, why_size, "The .zip has too many files (more than %d).", MODS_IMPORT_ENTRIES);
        goto fail;
    }
    if (length > DIRECTORY_MAX || start + length > (unsigned long)size - tail_size + eocd) {
        snprintf(why, why_size, "The .zip is damaged.");
        goto fail;
    }
    import->directory = start;
    if (!(directory = malloc(length ? length : 1)) || !(import->entries = calloc(entries ? entries : 1, sizeof(Entry))) ||
        (length && !read_at(import->file, start, directory, length))) {
        snprintf(why, why_size, "Could not read the .zip.");
        goto fail;
    }
    p = directory;
    end = directory + length;
    for (unsigned i = 0; i < entries; i++) {
        Entry *e = &import->entries[i];
        unsigned made_by, flags, name_length, raw_length, extra, comment, mode, type;
        char name[NAME_MAX_];
        unsigned char legacy[NAME_MAX_];
        const unsigned char *raw;
        if (end - p < 46 || le32(p) != 0x02014b50u) {
            snprintf(why, why_size, "The .zip is damaged.");
            goto fail;
        }
        made_by = le16(p + 4);
        flags = le16(p + 8);
        e->method = le16(p + 10);
        e->crc = le32(p + 16);
        e->packed = le32(p + 20);
        e->size = le32(p + 24);
        name_length = le16(p + 28);
        extra = le16(p + 30);
        comment = le16(p + 32);
        mode = le32(p + 38) >> 16;
        e->offset = le32(p + 42);
        if ((size_t)(end - p) < 46u + name_length + extra + comment) {
            snprintf(why, why_size, "The .zip is damaged.");
            goto fail;
        }
        raw = p + 46;
        raw_length = name_length;
        /* No UTF-8 flag, and not UTF-8 either (some tools write it unflagged). */
        if (!(flags & 0x800) && !utf8(raw, raw_length)) {
            raw_length = cp850_name(raw, raw_length, legacy, sizeof(legacy));
            raw = legacy;
        }
        if (!clean_name(raw, raw_length, name, &e->folder)) {
            /* never written: the name is shown cut at the first odd character */
            char shown[64];
            unsigned n = 0;
            for (; n < name_length && n < sizeof(shown) - 1 && p[46 + n] >= 0x20 && p[46 + n] < 0x7F; n++)
                shown[n] = (char)p[46 + n];
            shown[n] = 0;
            snprintf(why, why_size,
                     "The .zip has a file whose name points outside its folder or cannot be used (%s). Nothing "
                     "was imported.", shown);
            goto fail;
        }
        if (!(e->name = strdup(name))) {
            snprintf(why, why_size, "Out of memory.");
            goto fail;
        }
        import->count++;
        type = mode & 0170000u;
        /* Unix (3) and macOS (19) keep the file type in the high half. */
        if (((made_by >> 8) == 3 || (made_by >> 8) == 19) && type && type != 0100000u && type != 0040000u) {
            snprintf(why, why_size, "The .zip has a link or another special file (%s). Nothing was imported.",
                     e->name);
            goto fail;
        }
        if (flags & 1) {
            snprintf(why, why_size, "The .zip is password-protected; this game reads only an open one.");
            goto fail;
        }
        if (e->packed == 0xFFFFFFFFu || e->size == 0xFFFFFFFFu || e->offset == 0xFFFFFFFFul) {
            snprintf(why, why_size, "The .zip is in ZIP64 form, which this game does not read; zip the mod again.");
            goto fail;
        }
        if (e->folder ? e->size != 0 : e->method != 0 && e->method != 8) {
            snprintf(why, why_size, "The .zip uses a kind of compression this game cannot read (%s).", e->name);
            goto fail;
        }
        if (e->offset >= start) {
            snprintf(why, why_size, "The .zip is damaged.");
            goto fail;
        }
        if (e->method == 8 && e->size > (unsigned long long)e->packed * 1032u + 64) { /* deflate's best ratio */
            snprintf(why, why_size, "The .zip is damaged (%s).", e->name);
            goto fail;
        }
        import->total += e->size;
        if (import->total > MODS_IMPORT_BYTES) {
            snprintf(why, why_size, "The mod is too large: more than %u MB once unpacked.",
                     (MODS_IMPORT_BYTES >> 20) + 1);
            goto fail;
        }
        p += 46 + name_length + extra + comment;
    }
    /* No two entries that land on one file. */
    {
        Entry *sorted = malloc((size_t)(import->count ? import->count : 1) * sizeof(Entry));
        if (!sorted) {
            snprintf(why, why_size, "Out of memory.");
            goto fail;
        }
        memcpy(sorted, import->entries, (size_t)import->count * sizeof(Entry));
        qsort(sorted, (size_t)import->count, sizeof(Entry), name_order);
        for (int i = 1; i < import->count; i++)
            if (same_name(sorted[i - 1].name, sorted[i].name) && !(sorted[i - 1].folder && sorted[i].folder)) {
                snprintf(why, why_size, "The .zip lists %s twice. Nothing was imported.", sorted[i].name);
                free(sorted);
                goto fail;
            }
        free(sorted);
    }
    /* The mods: every mod.json at most one wrapper folder down, the
     * shallowest first; one inside a mod already found is that mod's. */
    {
        const Entry *manifests[MODS_IMPORT_MAX];
        int n = 0;
        if (!(found_mods = calloc(MODS_IMPORT_MAX, sizeof(*found_mods)))) {
            snprintf(why, why_size, "Out of memory.");
            goto fail;
        }
        for (int i = 0; i < import->count; i++) {
            const Entry *e = &import->entries[i];
            const char *base = strrchr(e->name, '/');
            int depth = 0;
            if (e->folder || skipped(e->name) || strcmp(base ? base + 1 : e->name, "mod.json"))
                continue;
            for (const char *c = e->name; *c; c++)
                depth += *c == '/';
            if (depth > 2)
                continue;
            if (n == MODS_IMPORT_MAX) {
                snprintf(why, why_size, "The .zip has more than %d mods, more than the game takes.",
                         MODS_IMPORT_MAX);
                goto fail;
            }
            memset(&found_mods[n], 0, sizeof(found_mods[n]));
            snprintf(found_mods[n].prefix, sizeof(found_mods[n].prefix), "%.*s",
                     (int)(base ? base + 1 - e->name : 0), e->name);
            n++;
        }
        qsort(found_mods, (size_t)n, sizeof(found_mods[0]), by_depth);
        for (int i = 0; i < n; i++) {
            int inside = 0;
            for (int j = 0; j < import->mod_count; j++)
                if (prefixed(found_mods[i].prefix, import->mods[j].prefix))
                    inside = 1;
            if (inside)
                continue;
            import->mods[import->mod_count] = found_mods[i];
            {
                char path[NAME_MAX_ + 16];
                snprintf(path, sizeof(path), "%smod.json", found_mods[i].prefix);
                manifests[import->mod_count] = find(import, path);
            }
            import->mod_count++;
        }
        for (int i = 0; i < import->mod_count; i++) {
            if (!describe(import, &import->mods[i], manifests[i], zip, why, why_size))
                goto fail;
            for (int j = 0; j < i; j++)
                if (same_name(import->mods[i].folder, import->mods[j].folder)) {
                    snprintf(why, why_size, "Two mods in the .zip go in one folder, %s.", import->mods[i].folder);
                    goto fail;
                } else if (!strcmp(import->mods[i].id, import->mods[j].id)) {
                    snprintf(why, why_size, "Two mods in the .zip have one id, %s.", import->mods[i].id);
                    goto fail;
                }
        }
    }
    free(tail);
    free(directory);
    free(found_mods);
    return import;
fail:
    free(tail);
    free(directory);
    free(found_mods);
    Mods_ImportClose(import);
    return NULL;
}

int Mods_ImportCount(const ModsImport *import) { return import ? import->mod_count : 0; }
unsigned long long Mods_ImportBytes(const ModsImport *import) { return import ? import->total : 0; }
void Mods_ImportWatch(ModsImport *import, unsigned *done, const int *cancel)
{
    if (!import)
        return;
    import->done = done;
    import->cancel = cancel;
}
void Mods_ImportStepped(ModsImport *import, void (*stepped)(void))
{
    if (import)
        import->stepped = stepped;
}
ModsImportMod *Mods_ImportMod(ModsImport *import, int index)
{
    return import && index >= 0 && index < import->mod_count ? &import->mods[index] : NULL;
}

void Mods_ImportClose(ModsImport *import)
{
    if (!import)
        return;
    if (import->file)
        fclose(import->file);
    for (int i = 0; i < import->count; i++)
        free(import->entries[i].name);
    free(import->entries);
    free(import);
}

int Mods_ImportRemoveTree(const char *path)
{
    struct stat info;
    DIR *directory;
    struct dirent *entry;
    /* lstat: a link is removed, never followed. Windows has none here (a
     * junction is followed: only the desktop tests run this there). */
#ifdef _WIN32
    if (stat(path, &info))
#else
    if (lstat(path, &info))
#endif
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(info.st_mode))
        return remove(path) ? -1 : 0;
    if ((directory = opendir(path))) {
        size_t size = strlen(path) + 2 + sizeof(entry->d_name);
        char *inner = malloc(size); /* off the stack: one per level of folders */
        while (inner && (entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            snprintf(inner, size, "%s/%s", path, entry->d_name);
            Mods_ImportRemoveTree(inner);
        }
        free(inner);
        closedir(directory);
    }
    return rmdir(path) ? -1 : 0;
}

int Mods_ImportTaken(const char *mods, const char *folder, char *path, size_t size)
{
    struct stat info;
    if (snprintf(path, size, "%s/%s", mods, folder) >= (int)size)
        return 0;
    return !stat(path, &info);
}

int Mods_ImportFolderId(const char *directory, char *id, size_t size)
{
    char path[2048], error[128];
    const char *slash = strrchr(directory, '/'), *text;
    static const char *const lists[] = {"fusions", "cards"};
    JsonDeferred deferred[2];
    JsonDocument *document;
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/mod.json", directory) >= (int)sizeof(path) || !(file = fopen(path, "rb")))
        return 0;
    fclose(file);
    /* Only the id: a mod with millions of fusions is named as cheaply as any. */
    document = Json_ParseFileDeferring(path, lists, 2, deferred, error, sizeof(error));
    text = Json_String(Json_Member(Json_Root(document), "id"), NULL);
    /* as the loader names it (mods.c read_manifest): the folder without one */
    copy(id, size, text && *text ? text : slash ? slash + 1 : directory);
    Json_Free(document);
    return 1;
}

/* The first name in `directory` that is not hidden, in `name`; 0 if none. */
static int first_visible(const char *directory, char *name, size_t size)
{
    DIR *handle = opendir(directory);
    struct dirent *entry;
    int found = 0;
    if (!handle)
        return 0;
    while (!found && (entry = readdir(handle)))
        if (entry->d_name[0] != '.' && strlen(entry->d_name) < size) {
            copy(name, size, entry->d_name);
            found = 1;
        }
    closedir(handle);
    return found;
}

/* The old mods a cut-short import had moved aside (Mods_ImportInstall puts
 * the old <replaced> of the new <target> in <temporary>/old/<target>/<replaced>)
 * go back to <mods>/<replaced> when neither it nor <mods>/<target> is
 * there: the new mod did not get in. A <target> that is there is the new mod
 * (which may have another name than the one it replaced: the same id under
 * another folder), so the old copy is left to be deleted. The number of old
 * mods that could not go back, or could not be looked at: their folder stays. */
static int restore_old(const char *mods, const char *temporary)
{
    char old[2048], targets[MODS_IMPORT_MAX][256];
    DIR *directory;
    struct dirent *entry;
    int n = 0, lost = 0;
    if (snprintf(old, sizeof(old), "%s/old", temporary) >= (int)sizeof(old) || !(directory = opendir(old)))
        return 0;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.') {
            if (n == MODS_IMPORT_MAX) {
                lost++; /* more than one import makes: keep them all */
                break;
            }
            copy(targets[n++], sizeof(targets[0]), entry->d_name);
        }
    closedir(directory);
    for (int i = 0; i < n; i++) {
        char inner[2400], replaced[256], from[2700], to[2400], target[2400];
        struct stat info;
        if (snprintf(inner, sizeof(inner), "%s/%s", old, targets[i]) >= (int)sizeof(inner) ||
            !first_visible(inner, replaced, sizeof(replaced)) ||
            snprintf(from, sizeof(from), "%s/%s", inner, replaced) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", mods, replaced) >= (int)sizeof(to) ||
            snprintf(target, sizeof(target), "%s/%s", mods, targets[i]) >= (int)sizeof(target) ||
            !stat(target, &info))
            continue;
        if (errno != ENOENT || !stat(to, &info) || errno != ENOENT) {
            lost++;
            continue;
        }
        lost += rename(from, to) != 0;
    }
    return lost;
}

void Mods_ImportCleanup(const char *mods)
{
    DIR *directory = opendir(mods);
    struct dirent *entry;
    char names[64][256];
    int n = 0;
    if (!directory)
        return;
    while ((entry = readdir(directory)) && n < 64)
        if (!strncmp(entry->d_name, ".import-", 8) || !strncmp(entry->d_name, ".incoming", 9))
            copy(names[n++], sizeof(names[0]), entry->d_name);
    closedir(directory);
    for (int i = 0; i < n; i++) {
        char path[2048];
        if (snprintf(path, sizeof(path), "%s/%s", mods, names[i]) >= (int)sizeof(path))
            continue;
        /* a removed mod's own folder (Mods_ImportRemoveInstalled) is not an
         * import's: nothing in it goes back */
        if (strncmp(names[i], ".import-removing-", 17) && restore_old(mods, path))
            continue; /* an old mod that cannot go back is not deleted */
        Mods_ImportRemoveTree(path);
    }
}

/* Makes the folders of `relative` (a cleaned entry name) below `base`,
 * which is there: the last part too when `all`, else up to it. Below a
 * folder this made itself, so no drive or share root is ever tried. */
static int make_below(const char *base, const char *relative, int all)
{
    char path[2048];
    struct stat info;
    size_t at = strlen(base) + 1, end;
    if (snprintf(path, sizeof(path), "%s/%s", base, relative) >= (int)sizeof(path))
        return -1;
    end = strlen(path);
    if (end > at && path[end - 1] == '/')
        path[--end] = 0; /* a folder entry's own slash */
    if (!all) {
        char *slash = strrchr(path + at, '/');
        if (!slash)
            return 0;
        *slash = 0;
        end = (size_t)(slash - path);
    }
    for (size_t i = at; i <= end; i++) {
        char kept = path[i];
        if (kept != '/' && kept)
            continue;
        path[i] = 0;
        if (mkdir(path, 0777) && (stat(path, &info) || !S_ISDIR(info.st_mode)))
            return -1;
        path[i] = kept;
    }
    return 0;
}

/* Where each mod is unpacked, its old folder moved and its place. */
typedef struct {
    char staged[1200], aside[1200], target[1200];
    int moved;
} Place;

int Mods_ImportInstall(ModsImport *import, const char *mods, char *why, size_t why_size)
{
    char temporary[1100];
    Place *places;
    int placed = 0, ok = 0, stranded = 0, left = -1, lefts = 0;
    if (!import || !import->mod_count) {
        snprintf(why, why_size, "This .zip has no mod in it.");
        return 0;
    }
    /* No folder replaced twice, or replaced by one mod and taken by another. */
    for (int m = 0; m < import->mod_count; m++)
        for (int n = 0; n < import->mod_count && import->mods[m].replace[0]; n++) {
            char other[1200];
            snprintf(other, sizeof(other), "%s/%s", mods, import->mods[n].folder);
            if (n != m && (!strcmp(import->mods[m].replace, import->mods[n].replace) ||
                           !strcmp(import->mods[m].replace, other))) {
                snprintf(why, why_size, "Two mods in the .zip would replace the same installed mod.");
                return 0;
            }
        }
    if (!(places = calloc((size_t)import->mod_count, sizeof(*places)))) {
        snprintf(why, why_size, "Out of memory.");
        return 0;
    }
    if (snprintf(temporary, sizeof(temporary), "%s/.import-XXXXXX", mods) >= (int)sizeof(temporary) ||
        !mkdtemp(temporary)) {
        snprintf(why, why_size, "Could not write in the mods folder (%s): %s.", mods, strerror(errno));
        free(places);
        return 0;
    }
#ifndef _WIN32
    {
        struct statvfs space;
        unsigned long long free_bytes;
        if (!statvfs(temporary, &space) &&
            (free_bytes = (unsigned long long)space.f_bavail * space.f_frsize) < import->total + (16u << 20)) {
            snprintf(why, why_size, "There is not enough free space for the mod: it needs %llu MB, and %llu MB are free.",
                     (import->total >> 20) + 1, free_bytes >> 20);
            goto done;
        }
    }
#endif
    for (int m = 0; m < import->mod_count; m++) {
        const ModsImportMod *mod = &import->mods[m];
        Place *at = &places[m];
        size_t plen = strlen(mod->prefix);
        char number[16];
        snprintf(number, sizeof(number), "%d", m);
        snprintf(at->staged, sizeof(at->staged), "%s/%d", temporary, m);
        if (snprintf(at->target, sizeof(at->target), "%s/%s", mods, mod->folder) >= (int)sizeof(at->target) ||
            make_below(temporary, number, 1)) {
            snprintf(why, why_size, "Could not write in the mods folder: %s.", strerror(errno));
            goto done;
        }
        for (int i = 0; i < import->count; i++) {
            const Entry *e = &import->entries[i];
            char path[2048];
            FILE *out;
            int written;
            /* inside the mod's folder, letter case aside (one folder on a
             * case-blind file system) */
            if (skipped(e->name) || !prefixed(e->name, mod->prefix) || !e->name[plen])
                continue;
            if (stop_asked(import)) {
                snprintf(why, why_size, "Cancelled.");
                goto done;
            }
            if (snprintf(path, sizeof(path), "%s/%s", at->staged, e->name + plen) >= (int)sizeof(path)) {
                snprintf(why, why_size, "A file name in the .zip is too long (%s).", e->name);
                goto done;
            }
            if (e->folder) {
                if (make_below(at->staged, e->name + plen, 1)) {
                    snprintf(why, why_size, "Could not make the folder %s: %s.", e->name, strerror(errno));
                    goto done;
                }
                continue;
            }
            /* "x": every file is new in the fresh folder, so one that is
             * there already is another of the .zip's names that this
             * storage folds into it (Android's shared storage ignores
             * letter case beyond ASCII, and accents' forms): refused, not
             * written over. */
            if (make_below(at->staged, e->name + plen, 0) || !(out = fopen(path, "wbx"))) {
                if (errno == EEXIST)
                    snprintf(why, why_size, "Two files in the .zip are one file on this storage, which does not "
                             "tell their letters apart: %s.", e->name);
                else
                    snprintf(why, why_size, "Could not write %s: %s.", e->name, strerror(errno));
                goto done;
            }
            written = unpack(import, e, out, NULL, why, why_size);
            if (fclose(out) && written) {
                snprintf(why, why_size, "Could not write %s: %s.", e->name, strerror(errno));
                written = 0;
            }
            if (!written)
                goto done;
        }
    }
    if (stop_asked(import)) {
        snprintf(why, why_size, "Cancelled."); /* the last file came with the cancel */
        goto done;
    }
    /* Everything is unpacked: each mod goes in place, the folder it
     * replaces moved aside first. */
    for (placed = 0; placed < import->mod_count; placed++) {
        ModsImportMod *mod = &import->mods[placed];
        Place *at = &places[placed];
        struct stat info;
        if (mod->replace[0] && !stat(mod->replace, &info)) {
            /* Under its own name, in "old/<the new mod's folder>": if the
             * process ends between this rename and the new mod's (or before
             * the folder is removed), Mods_ImportCleanup knows where the old
             * mod goes back, and whether the new one got in. */
            char old[1200], inner[1200];
            const char *name = mod->replace + strlen(mod->replace);
            while (name > mod->replace && name[-1] != '/' && name[-1] != '\\')
                name--;
            if (snprintf(old, sizeof(old), "%s/old", temporary) >= (int)sizeof(old) ||
                snprintf(inner, sizeof(inner), "%s/%s", old, mod->folder) >= (int)sizeof(inner) ||
                snprintf(at->aside, sizeof(at->aside), "%s/%s", inner, name) >= (int)sizeof(at->aside) ||
                (mkdir(old, 0777) && errno != EEXIST) || (mkdir(inner, 0777) && errno != EEXIST) ||
                rename(mod->replace, at->aside)) {
                snprintf(why, why_size, "Could not move the old %s aside: %s.", mod->folder, strerror(errno));
                goto undo;
            }
            at->moved = 1;
        }
        if (!stat(at->target, &info)) {
            snprintf(why, why_size, "A folder named %s is already in the mods folder.", mod->folder);
            goto undo;
        }
        if (rename(at->staged, at->target)) {
            snprintf(why, why_size, "Could not put %s in the mods folder: %s.", mod->folder, strerror(errno));
            goto undo;
        }
    }
    ok = 1;
    goto done;
undo:
    /* `placed` is the mod that failed: it may have moved its old folder.
     * A new mod placed before it goes back out (moved back, else removed:
     * it is this import's copy); one that cannot is left, and said. An old
     * folder that cannot go back is kept, never removed. */
    for (int m = placed; m >= 0; m--) {
        Place *at = &places[m];
        struct stat info;
        if (m < placed && rename(at->target, at->staged)) {
            Mods_ImportRemoveTree(at->target);
            if (!stat(at->target, &info)) {
                left = m;
                lefts++;
                if (at->moved && !strcmp(import->mods[m].replace, at->target)) {
                    stranded = 1; /* its place is the new mod's */
                    continue;
                }
            }
        }
        if (at->moved && rename(at->aside, import->mods[m].replace))
            stranded = 1;
    }
done:
    if (stranded || left >= 0) {
        /* .recovered-* is neither scanned (a dot) nor cleaned up. */
        char kept[1100], extra[1300] = "";
        snprintf(kept, sizeof(kept), "%s/.recovered-%s", mods, strrchr(temporary, '-') + 1);
        if (stranded)
            snprintf(extra, sizeof(extra), ", and an old mod could not be put back: it is in %s",
                     rename(temporary, kept) ? temporary : kept);
        if (left >= 0 && lefts > 1)
            snprintf(why, why_size, "Nothing was imported, but %s and %d more could not be taken out of the mods "
                     "folder again (delete those folders)%s.", import->mods[left].folder, lefts - 1, extra);
        else if (left >= 0)
            snprintf(why, why_size, "Nothing was imported, but %s could not be taken out of the mods folder again "
                     "(delete that folder)%s.", import->mods[left].folder, extra);
        else
            snprintf(why, why_size, "Nothing was imported%s.", extra);
    }
    if (!stranded)
        Mods_ImportRemoveTree(temporary);
    free(places);
    return ok;
}

int Mods_ImportRemoveInstalled(const char *mods, const char *directory, char *why, size_t why_size)
{
    char base[1024], inner[1400];
    size_t length = mods ? strlen(mods) : 0;
    const char *folder;
    struct stat info;
    if (why_size)
        why[0] = 0;
    if (!length || length >= sizeof(base) || !directory)
        goto outside;
    memcpy(base, mods, length + 1);
    while (length > 1 && (base[length - 1] == '/' || base[length - 1] == '\\'))
        base[--length] = 0;
    /* <mods>/<one name>: not the mods folder, not a folder below one of
     * its mods, not a "..", not a hidden folder (the import's own are). */
    if (!length || strncmp(directory, base, length) || (directory[length] != '/' && directory[length] != '\\'))
        goto outside;
    folder = directory + length + 1;
    while (*folder == '/' || *folder == '\\') /* "<mods>//<name>" from a MEMORIES_MODS_DIR ending in a separator */
        folder++;
    if (!*folder || *folder == '.' || strpbrk(folder, "/\\:"))
        goto outside;
    /* lstat: a link is not the mod's folder, and is never followed. */
#ifdef _WIN32
    if (stat(directory, &info))
#else
    if (lstat(directory, &info))
#endif
    {
        if (errno == ENOENT)
            return 0; /* gone already */
        goto outside;
    }
    if (!S_ISDIR(info.st_mode))
        goto outside;
    /* Renamed first, in one step, to a hidden folder the mod scan skips and
     * Mods_ImportCleanup sweeps (".import-*"): the mod is gone at once, and
     * what an interrupted or failed delete leaves is no half mod in the list
     * but a hidden folder the next start (or import) finishes off. */
    if (snprintf(inner, sizeof(inner), "%s/.import-removing-%s", base, folder) >= (int)sizeof(inner)) {
        snprintf(why, why_size, "The name of %s is too long to remove.", folder);
        return -1;
    }
    Mods_ImportRemoveTree(inner); /* what an earlier remove of that name left */
    if (rename(directory, inner)) {
        snprintf(why, why_size, "Could not delete %s: try Remove again.", folder);
        return -1;
    }
    Mods_ImportRemoveTree(inner);
    return 0;
outside:
    snprintf(why, why_size, "That folder is not one of the mods in the mods folder.");
    return -1;
}

/* --- Modpacks (import.h): one file out of the .zip, a .zip written, SHA-256 */

int Mods_ImportExtract(ModsImport *import, const char *name, const char *path, char *why, size_t why_size)
{
    const Entry *e = import ? find(import, name) : NULL;
    FILE *out;
    int ok;
    if (!e || e->folder)
        return 0;
    if (!(out = fopen(path, "wb"))) {
        snprintf(why, why_size, "Could not write %s: %s.", path, strerror(errno));
        return -1;
    }
    ok = unpack(import, e, out, NULL, why, why_size);
    if (fclose(out))
        ok = 0;
    if (!ok)
        remove(path);
    return ok ? 1 : -1;
}

typedef struct {
    char *name;
    unsigned long crc, packed, size, offset;
} ZipOutEntry;
struct ModsZipOut {
    FILE *file;
    char *path;
    ZipOutEntry *entries;
    int count, room, failed;
    long long *done;   /* Mods_ZipProgress */
    const int *cancel;
};
static void put_le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_le32(unsigned char *p, unsigned long v) { put_le16(p, (unsigned)(v & 0xffff)); put_le16(p + 2, (unsigned)(v >> 16)); }
ModsZipOut *Mods_ZipCreate(const char *path, char *why, size_t why_size)
{
    ModsZipOut *zip = calloc(1, sizeof(*zip));
    if (!zip || !(zip->path = malloc(strlen(path) + 1))) {
        free(zip);
        snprintf(why, why_size, "Out of memory.");
        return NULL;
    }
    strcpy(zip->path, path);
    if (!(zip->file = fopen(path, "wb"))) {
        snprintf(why, why_size, "Could not write %s: %s.", path, strerror(errno));
        free(zip->path);
        free(zip);
        return NULL;
    }
    return zip;
}
/* One entry, deflated, from a file (`source`) or from memory (`data`). */
void Mods_ZipAbandon(ModsZipOut *zip) { zip->failed = 1; }
void Mods_ZipProgress(ModsZipOut *zip, long long *done, const int *cancel)
{
    zip->done = done;
    zip->cancel = cancel;
}
static int zip_add(ModsZipOut *zip, const char *name, const char *source, const void *data, size_t size, char *why,
                   size_t why_size)
{
    unsigned char header[30], *in = NULL, *out = NULL;
    FILE *from = NULL;
    z_stream stream;
    long at;
    size_t n = strlen(name);
    unsigned long crc = crc32(0L, Z_NULL, 0), total = 0, packed = 0;
    int flush, ok = 0;
    ZipOutEntry *e;
    if (zip->failed || n > 0xffff || zip->count >= MODS_IMPORT_ENTRIES)
        return 0;
    if (zip->count == zip->room) {
        ZipOutEntry *grown = realloc(zip->entries, (size_t)(zip->room = zip->room ? zip->room * 2 : 64) * sizeof(*grown));
        if (!grown) { snprintf(why, why_size, "Out of memory."); return zip->failed = 1, 0; }
        zip->entries = grown;
    }
    if (source && !(from = fopen(source, "rb"))) {
        snprintf(why, why_size, "Could not read %s: %s.", source, strerror(errno));
        return zip->failed = 1, 0;
    }
    if ((at = ftell(zip->file)) < 0 || !(in = malloc(CHUNK)) || !(out = malloc(CHUNK))) {
        snprintf(why, why_size, "Could not write the .zip.");
        goto done;
    }
    memset(header, 0, sizeof(header));
    put_le32(header, 0x04034b50u);
    put_le16(header + 4, 20);
    put_le16(header + 6, 0x800); /* names in UTF-8 */
    put_le16(header + 8, 8);
    put_le16(header + 12, 0x21); /* 1980-01-01 */
    put_le16(header + 26, (unsigned)n);
    if (fwrite(header, 1, 30, zip->file) != 30 || fwrite(name, 1, n, zip->file) != n) {
        snprintf(why, why_size, "Could not write the .zip.");
        goto done;
    }
    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        snprintf(why, why_size, "Could not start packing the .zip.");
        goto done;
    }
    do {
        size_t got;
        if (from) {
            got = fread(in, 1, CHUNK, from);
            if (ferror(from)) { snprintf(why, why_size, "Could not read %s.", source); deflateEnd(&stream); goto done; }
            flush = feof(from) ? Z_FINISH : Z_NO_FLUSH;
            stream.next_in = in;
        } else {
            got = size - total < CHUNK ? size - total : CHUNK;
            stream.next_in = (unsigned char *)data + total;
            flush = total + got >= size ? Z_FINISH : Z_NO_FLUSH;
        }
        if (zip->cancel && __atomic_load_n(zip->cancel, __ATOMIC_RELAXED)) {
            snprintf(why, why_size, "Export stopped.");
            deflateEnd(&stream);
            goto done;
        }
        if (zip->done) __atomic_add_fetch(zip->done, (long long)got, __ATOMIC_RELAXED);
        crc = crc32(crc, stream.next_in, (uInt)got);
        total += (unsigned long)got;
        stream.avail_in = (uInt)got;
        do {
            size_t have;
            stream.next_out = out;
            stream.avail_out = CHUNK;
            deflate(&stream, flush);
            have = CHUNK - stream.avail_out;
            if (fwrite(out, 1, have, zip->file) != have) {
                snprintf(why, why_size, "Could not write the .zip.");
                deflateEnd(&stream);
                goto done;
            }
            packed += (unsigned long)have;
        } while (stream.avail_out == 0);
    } while (flush != Z_FINISH);
    deflateEnd(&stream);
    if (total > 0xfffffffful || packed > 0xfffffffful) { snprintf(why, why_size, "%s is too large.", name); goto done; }
    put_le32(header + 14, crc);
    put_le32(header + 18, packed);
    put_le32(header + 22, total);
    if (fseek(zip->file, at, SEEK_SET) || fwrite(header, 1, 30, zip->file) != 30 || fseek(zip->file, 0, SEEK_END)) {
        snprintf(why, why_size, "Could not write the .zip.");
        goto done;
    }
    e = &zip->entries[zip->count];
    if (!(e->name = malloc(n + 1))) { snprintf(why, why_size, "Out of memory."); goto done; }
    memcpy(e->name, name, n + 1);
    e->crc = crc;
    e->packed = packed;
    e->size = total;
    e->offset = (unsigned long)at;
    zip->count++;
    ok = 1;
done:
    if (from) fclose(from);
    free(in);
    free(out);
    if (!ok) zip->failed = 1;
    return ok;
}
int Mods_ZipAddFile(ModsZipOut *zip, const char *name, const char *source, char *why, size_t why_size)
{
    return zip_add(zip, name, source, NULL, 0, why, why_size);
}
int Mods_ZipAddData(ModsZipOut *zip, const char *name, const void *data, size_t size, char *why, size_t why_size)
{
    return zip_add(zip, name, NULL, data ? data : "", size, why, why_size);
}
int Mods_ZipFinish(ModsZipOut *zip, char *why, size_t why_size)
{
    unsigned char record[46], end[22];
    long start;
    unsigned long length = 0;
    int ok = !zip->failed;
    if (ok && (start = ftell(zip->file)) >= 0) {
        for (int i = 0; ok && i < zip->count; i++) {
            const ZipOutEntry *e = &zip->entries[i];
            size_t n = strlen(e->name);
            memset(record, 0, sizeof(record));
            put_le32(record, 0x02014b50u);
            put_le16(record + 4, 20);
            put_le16(record + 6, 20);
            put_le16(record + 8, 0x800);
            put_le16(record + 10, 8);
            put_le16(record + 14, 0x21);
            put_le32(record + 16, e->crc);
            put_le32(record + 20, e->packed);
            put_le32(record + 24, e->size);
            put_le16(record + 28, (unsigned)n);
            put_le32(record + 42, e->offset);
            ok = fwrite(record, 1, 46, zip->file) == 46 && fwrite(e->name, 1, n, zip->file) == n;
            length += 46 + (unsigned long)n;
        }
        memset(end, 0, sizeof(end));
        put_le32(end, 0x06054b50u);
        put_le16(end + 8, (unsigned)zip->count);
        put_le16(end + 10, (unsigned)zip->count);
        put_le32(end + 12, length);
        put_le32(end + 16, (unsigned long)start);
        ok = ok && fwrite(end, 1, 22, zip->file) == 22;
    } else
        ok = 0;
    if (fclose(zip->file))
        ok = 0;
    if (!ok) {
        if (!zip->failed) snprintf(why, why_size, "Could not write the .zip.");
        remove(zip->path);
    }
    for (int i = 0; i < zip->count; i++)
        free(zip->entries[i].name);
    free(zip->entries);
    free(zip->path);
    free(zip);
    return ok;
}
