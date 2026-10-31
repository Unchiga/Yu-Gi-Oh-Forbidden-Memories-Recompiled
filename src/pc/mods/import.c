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
#define MANIFEST_MAX (4u << 20)         /* a mod.json read to name the mod */
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
    const int *cancel;  /* and nonzero (atomic) to stop between files */
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
} Sink;
static int emit(Sink *sink, const unsigned char *bytes, size_t n)
{
    if (n > sink->size - sink->written)
        return -1;
    sink->crc = crc32(sink->crc, bytes, (uInt)n);
    if (sink->out ? fwrite(bytes, 1, n, sink->out) != n : (memcpy(sink->memory + sink->written, bytes, n), 0))
        return 0;
    sink->written += (unsigned long)n;
    return 1;
}
static int unpack(ModsImport *import, const Entry *e, FILE *out, unsigned char *memory, char *why, size_t why_size)
{
    unsigned char header[30], *in = NULL, *buffer = NULL;
    unsigned long data, left = e->packed;
    Sink sink = {out, memory, 0, e->size, 0};
    z_stream stream;
    int ok = 0, started = 0, status = Z_OK, wrote;
    sink.crc = crc32(0L, Z_NULL, 0);
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
static int describe(ModsImport *import, ModsImportMod *mod, const Entry *manifest, char *why, size_t why_size)
{
    unsigned char *text = NULL;
    JsonDocument *document = NULL;
    const JsonValue *root = NULL;
    const char *id = NULL, *name, *slash;
    char folder[NAME_MAX_], error[128];
    size_t plen = strlen(mod->prefix);
    if (manifest->size <= MANIFEST_MAX && (text = malloc(manifest->size + 1))) {
        if (!unpack(import, manifest, NULL, text, why, why_size)) {
            free(text);
            return 0;
        }
        document = Json_Parse((const char *)text, error, sizeof(error));
        root = Json_Root(document);
    }
    free(text);
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
        unsigned made_by, flags, name_length, extra, comment, mode, type;
        char name[NAME_MAX_];
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
        if (!clean_name(p + 46, name_length, name, &e->folder)) {
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
            if (!describe(import, &import->mods[i], manifests[i], why, why_size))
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
    JsonDocument *document;
    FILE *file;
    if (snprintf(path, sizeof(path), "%s/mod.json", directory) >= (int)sizeof(path) || !(file = fopen(path, "rb")))
        return 0;
    fclose(file);
    document = Json_ParseFile(path, error, sizeof(error));
    text = Json_String(Json_Member(Json_Root(document), "id"), NULL);
    /* as the loader names it (mods.c read_manifest): the folder without one */
    copy(id, size, text && *text ? text : slash ? slash + 1 : directory);
    Json_Free(document);
    return 1;
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
        if (snprintf(path, sizeof(path), "%s/%s", mods, names[i]) < (int)sizeof(path))
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
        snprintf(at->aside, sizeof(at->aside), "%s/old-%d", temporary, m);
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
            if (import->cancel && __atomic_load_n(import->cancel, __ATOMIC_ACQUIRE)) {
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
            if (import->done)
                __atomic_add_fetch(import->done, e->size, __ATOMIC_RELEASE);
        }
    }
    if (import->cancel && __atomic_load_n(import->cancel, __ATOMIC_ACQUIRE)) {
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
            if (rename(mod->replace, at->aside)) {
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
