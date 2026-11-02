#ifndef MEMORIES_TESTS_ZIP_WRITER_H
#define MEMORIES_TESTS_ZIP_WRITER_H
/* Writes a .zip entry by entry, for the mod import's tests
 * (mod_import_test.c, mods_window_test.c): stored or deflated, with the
 * flags, attributes, CRCs and sizes a test wants, right or wrong. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    const char *name;
    const char *data;     /* NULL for a folder */
    int deflate;
    unsigned mode;        /* a Unix mode for the high half of the attributes; 0 none */
    unsigned flags;
    unsigned method;      /* 0: from `deflate` */
    int bad_crc;
    long lie_size;        /* nonzero: the size the directory gives instead */
} ZipItem;

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)(v >> 8 & 0xFF), f);
}
static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)(v >> 16 & 0xFFFF));
}

#define ZIP_ITEMS_MAX 512 /* entries in one .zip written here */

static void write_zip(const char *path, const ZipItem *items, int n)
{
    FILE *f = fopen(path, "wb");
    unsigned long offsets[ZIP_ITEMS_MAX], packed[ZIP_ITEMS_MAX], crcs[ZIP_ITEMS_MAX], sizes[ZIP_ITEMS_MAX], start, end;
    unsigned methods[ZIP_ITEMS_MAX];
    unsigned char *bodies[ZIP_ITEMS_MAX];
    if (n > ZIP_ITEMS_MAX) {
        fprintf(stderr, "write_zip: more than %d entries\n", ZIP_ITEMS_MAX);
        exit(2);
    }
    if (!f) {
        perror(path);
        exit(2);
    }
    for (int i = 0; i < n; i++) {
        const ZipItem *it = &items[i];
        size_t size = it->data ? strlen(it->data) : 0;
        unsigned char *body = malloc(size + 64 + size / 10);
        unsigned long length = (unsigned long)size;
        crcs[i] = crc32(0L, (const Bytef *)(it->data ? it->data : ""), (uInt)size) ^ (it->bad_crc ? 1u : 0u);
        sizes[i] = it->lie_size ? (unsigned long)it->lie_size : (unsigned long)size;
        methods[i] = it->method ? it->method : it->deflate ? 8 : 0;
        if (it->deflate) {
            z_stream s;
            memset(&s, 0, sizeof(s));
            deflateInit2(&s, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            s.next_in = (Bytef *)(it->data ? it->data : "");
            s.avail_in = (uInt)size;
            s.next_out = body;
            s.avail_out = (uInt)(size + 64 + size / 10);
            deflate(&s, Z_FINISH);
            length = s.total_out;
            deflateEnd(&s);
        } else if (size)
            memcpy(body, it->data, size);
        bodies[i] = body;
        packed[i] = length;
        offsets[i] = (unsigned long)ftell(f);
        put32(f, 0x04034b50u);
        put16(f, 20);
        put16(f, it->flags);
        put16(f, methods[i]);
        put16(f, 0);
        put16(f, 0);
        put32(f, crcs[i]);
        put32(f, packed[i]);
        put32(f, sizes[i]);
        put16(f, (unsigned)strlen(it->name));
        put16(f, 0);
        fwrite(it->name, 1, strlen(it->name), f);
        fwrite(body, 1, length, f);
    }
    start = (unsigned long)ftell(f);
    for (int i = 0; i < n; i++) {
        const ZipItem *it = &items[i];
        put32(f, 0x02014b50u);
        put16(f, it->mode ? 3u << 8 | 20 : 20);
        put16(f, 20);
        put16(f, it->flags);
        put16(f, methods[i]);
        put16(f, 0);
        put16(f, 0);
        put32(f, crcs[i]);
        put32(f, packed[i]);
        put32(f, sizes[i]);
        put16(f, (unsigned)strlen(it->name));
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put16(f, 0);
        put32(f, (unsigned long)it->mode << 16);
        put32(f, offsets[i]);
        fwrite(it->name, 1, strlen(it->name), f);
        free(bodies[i]);
    }
    end = (unsigned long)ftell(f);
    put32(f, 0x06054b50u);
    put16(f, 0);
    put16(f, 0);
    put16(f, (unsigned)n);
    put16(f, (unsigned)n);
    put32(f, end - start);
    put32(f, start);
    put16(f, 0);
    fclose(f);
}

#endif
