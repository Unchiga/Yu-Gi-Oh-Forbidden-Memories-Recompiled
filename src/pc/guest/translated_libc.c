/* libc boundaries for experimental translated game units. This file must be
 * compiled normally: its real libc calls must never be remapped to itself. */
#include "translated_runtime.h"
#include "translated_libc.h"
#include <stdint.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void *at(const void *address, size_t offset)
{
    uintptr_t base = (uintptr_t)address;
    if (offset > UINTPTR_MAX - base ||
        (base <= UINT32_MAX && offset > UINT32_MAX - base)) abort();
    return (void *)(base + offset);
}
static unsigned char byte(const void *address, size_t offset)
{
    return *(const unsigned char *)GuestRuntime_ResolveData(at(address, offset), 1);
}
size_t GuestRuntime_strlen(const char *string)
{
    size_t length = 0;
    while (byte(string, length)) {
        if (length == SIZE_MAX) abort();
        length++;
    }
    return length;
}
long GuestRuntime_strtol(const char *string, char **end, int base)
{
    size_t length = GuestRuntime_strlen(string);
    if (length == SIZE_MAX) abort();
    const char *native = GuestRuntime_ResolveData((void *)string, length + 1);
    char *parsed_end;
    long value = strtol(native, &parsed_end, base);
    if (end) {
        char *guest_end = at(string, (size_t)(parsed_end - native));
        memcpy(GuestRuntime_ResolveData(end, sizeof(guest_end)), &guest_end, sizeof(guest_end));
    }
    return value;
}
/* FILE handles come from the native mod host's open_asset/open_data callbacks.
 * They are opaque host objects, never guest-addressed storage. */
static FILE *native_file(FILE *file)
{
    if (!GuestRuntime_IsHostAddress((uintptr_t)file)) abort();
    return file;
}
char *GuestRuntime_fgets(char *buffer, int size, FILE *file)
{
    char *native = size > 0 ? GuestRuntime_ResolveData(buffer, (size_t)size) : NULL;
    return fgets(native, size, native_file(file)) ? buffer : NULL;
}
size_t GuestRuntime_fwrite(const void *buffer, size_t size, size_t count, FILE *file)
{
    if (size && count > SIZE_MAX / size) abort();
    size_t bytes = size * count;
    const void *native = bytes ? GuestRuntime_ResolveData((void *)buffer, bytes) : buffer;
    return fwrite(native, size, count, native_file(file));
}
int GuestRuntime_fseek(FILE *file, long offset, int origin)
{
    return fseek(native_file(file), offset, origin);
}
int GuestRuntime_fclose(FILE *file) { return fclose(native_file(file)); }
int GuestRuntime_isalnum(int value) { return isalnum(value); }
int GuestRuntime_isdigit(int value) { return isdigit(value); }
int GuestRuntime_isspace(int value) { return isspace(value); }
int GuestRuntime_tolower(int value) { return tolower(value); }
void *GuestRuntime_memcpy(void *destination, const void *source, size_t length)
{
    if (length) memcpy(GuestRuntime_ResolveData(destination, length), GuestRuntime_ResolveData((void *)source, length), length);
    return destination;
}
void *GuestRuntime_memmove(void *destination, const void *source, size_t length)
{
    if (length) memmove(GuestRuntime_ResolveData(destination, length), GuestRuntime_ResolveData((void *)source, length), length);
    return destination;
}
void *GuestRuntime_memset(void *destination, int value, size_t length)
{
    if (length) memset(GuestRuntime_ResolveData(destination, length), value, length);
    return destination;
}
void GuestRuntime_bzero(void *destination, size_t length)
{
    GuestRuntime_memset(destination, 0, length);
}
int GuestRuntime_memcmp(const void *a, const void *b, size_t length)
{
    return length ? memcmp(GuestRuntime_ResolveData((void *)a, length), GuestRuntime_ResolveData((void *)b, length), length) : 0;
}
int GuestRuntime_strncmp(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char x = byte(a, i), y = byte(b, i);
        if (x != y) return (int)x - (int)y;
        if (!x) break;
    }
    return 0;
}
int GuestRuntime_strcmp(const char *a, const char *b)
{
    return GuestRuntime_strncmp(a, b, SIZE_MAX);
}
char *GuestRuntime_strcpy(char *destination, const char *source)
{
    size_t length = GuestRuntime_strlen(source);
    if (length == SIZE_MAX) abort();
    GuestRuntime_memcpy(destination, source, length + 1);
    return destination;
}
char *GuestRuntime_strncpy(char *destination, const char *source, size_t length)
{
    size_t copied = 0;
    unsigned char *out;
    if (!length) return destination;
    out = GuestRuntime_ResolveData(destination, length);
    while (copied < length) {
        unsigned char c = byte(source, copied);
        out[copied++] = c;
        if (!c) { memset(out + copied, 0, length - copied); break; }
    }
    return destination;
}
char *GuestRuntime_strcat(char *destination, const char *source)
{
    size_t prefix = GuestRuntime_strlen(destination), suffix = GuestRuntime_strlen(source);
    if (suffix == SIZE_MAX || prefix > SIZE_MAX - suffix - 1) abort();
    GuestRuntime_ResolveData(destination, prefix + suffix + 1);
    GuestRuntime_memcpy(at(destination, prefix), source, suffix + 1);
    return destination;
}
struct SortContext {
    uintptr_t guest_base, host_base;
    size_t length, width;
    int (*compare)(const void *, const void *);
    void *temporary[2];
};
static _Thread_local struct SortContext sort_context;
static const void *sort_argument(const void *element, unsigned slot)
{
    uintptr_t address = (uintptr_t)element;
    if (address >= sort_context.host_base &&
        address - sort_context.host_base <= sort_context.length - sort_context.width)
        return (const void *)(sort_context.guest_base + address - sort_context.host_base);
    /* Darwin's qsort may compare a temporary copy rather than an array row.
     * Subtracting the array base from that address invents an invalid guest
     * pointer. Give each copied operand registered storage instead, including
     * for comparators that cast their arguments back to 32-bit addresses. */
    if (!sort_context.temporary[slot]) {
        sort_context.temporary[slot] = malloc(sort_context.width);
        if (!sort_context.temporary[slot]) abort();
        GuestRuntime_RegisterAutomatic(sort_context.temporary[slot], sort_context.width);
    }
    memcpy(sort_context.temporary[slot], element, sort_context.width);
    return (const void *)(uintptr_t)GuestRuntime_EncodePointer(sort_context.temporary[slot]);
}
static int compare_elements(const void *a, const void *b)
{
    const void *x = sort_argument(a, 0);
    const void *y = a == b ? x : sort_argument(b, 1);
    return sort_context.compare(x, y);
}
void GuestRuntime_qsort(void *base, size_t count, size_t width, int (*compare)(const void *, const void *))
{
    struct SortContext previous = sort_context;
    void *host;
    if (!count) return;
    if (!width || count > SIZE_MAX / width) abort();
    host = GuestRuntime_ResolveData(base, count * width);
    sort_context = (struct SortContext){.guest_base = (uintptr_t)base, .host_base = (uintptr_t)host,
        .length = count * width, .width = width,
        .compare = (int (*)(const void *, const void *))GuestRuntime_ResolveFunction((void *)compare)};
    qsort(host, count, width, compare_elements);
    for (unsigned i = 0; i < 2; i++) if (sort_context.temporary[i]) {
        if (GuestRuntime_UnregisterData(sort_context.temporary[i])) abort();
        free(sort_context.temporary[i]);
    }
    sort_context = previous;
}

/* Format one conversion at a time so %s and %n also cross the guest boundary.
 * Unsupported positional/wide conversions abort rather than dereferencing
 * an untranslated pointer inside host printf. */
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>
struct Text { char *data; size_t length, capacity; };
static void append(struct Text *text, const char *data, size_t length)
{
    if (length > SIZE_MAX - text->length - 1) abort();
    size_t needed = text->length + length + 1;
    if (needed > text->capacity) {
        size_t capacity = needed < 256 ? 256 : needed;
        char *next = realloc(text->data, capacity);
        if (!next) abort();
        text->data = next; text->capacity = capacity;
    }
    memcpy(text->data + text->length, data, length);
    text->length += length; text->data[text->length] = 0;
}
static struct Text format_text(const char *guest_format, va_list arguments)
{
    size_t length = GuestRuntime_strlen(guest_format);
    const char *format = GuestRuntime_ResolveData((void *)guest_format, length + 1);
    struct Text result = {0};
    append(&result, "", 0);
    for (const char *p = format; *p;) {
        if (*p != '%') { append(&result, p++, 1); continue; }
        char spec[128]; size_t used = 0;
        spec[used++] = *p++;
        if (*p == '%') { append(&result, p++, 1); continue; }
        while (*p && strchr("-+ #0", *p)) { if (used >= 100) abort(); spec[used++] = *p++; }
        if (*p == '*') {
            int width = va_arg(arguments, int); p++;
            int n = snprintf(spec + used, sizeof(spec) - used, "%d", width);
            if (n < 0 || (size_t)n >= sizeof(spec) - used) abort();
            used += (size_t)n;
        } else while (*p >= '0' && *p <= '9') { if (used >= 100) abort(); spec[used++] = *p++; }
        int precision = -1;
        if (*p == '.') {
            p++; precision = 0;
            if (*p == '*') { precision = va_arg(arguments, int); p++; }
            else while (*p >= '0' && *p <= '9') {
                if (precision > 100000000) abort();
                precision = precision * 10 + (*p++ - '0');
            }
            if (precision >= 0) {
                int n = snprintf(spec + used, sizeof(spec) - used, ".%d", precision);
                if (n < 0 || (size_t)n >= sizeof(spec) - used) abort();
                used += (size_t)n;
            }
        }
        enum { NORMAL, LONG, LONGLONG, SIZE, SHORT, CHAR } kind = NORMAL;
        if (*p == 'l') { p++; kind = LONG; if (*p == 'l') { p++; kind = LONGLONG; } }
        else if (*p == 'z' || *p == 't' || *p == 'j') { p++; kind = SIZE; }
        else if (*p == 'h') { p++; kind = SHORT; if (*p == 'h') { p++; kind = CHAR; } }
        char conversion = *p++;
        if (!conversion) abort();
        char *string = NULL;
        int signed_value = conversion == 'd' || conversion == 'i';
        int integer = signed_value || strchr("ouxX", conversion) != NULL;
        if (integer) { spec[used++] = 'j'; }
        else if (kind != NORMAL && conversion != 'n') abort();
        spec[used++] = conversion; spec[used] = 0;
        int count;
        intmax_t signed_arg = 0; uintmax_t unsigned_arg = 0;
        double floating = 0; void *pointer = NULL; int character = 0;
        if (integer) {
            if (signed_value) {
                if (kind == LONG) signed_arg = va_arg(arguments, long);
                else if (kind == LONGLONG) signed_arg = va_arg(arguments, long long);
                else if (kind == SIZE) signed_arg = va_arg(arguments, ptrdiff_t);
                else { signed_arg = va_arg(arguments, int); if (kind == SHORT) signed_arg = (short)signed_arg; if (kind == CHAR) signed_arg = (signed char)signed_arg; }
                count = snprintf(NULL, 0, spec, signed_arg);
            } else {
                if (kind == LONG) unsigned_arg = va_arg(arguments, unsigned long);
                else if (kind == LONGLONG) unsigned_arg = va_arg(arguments, unsigned long long);
                else if (kind == SIZE) unsigned_arg = va_arg(arguments, size_t);
                else { unsigned_arg = va_arg(arguments, unsigned); if (kind == SHORT) unsigned_arg = (unsigned short)unsigned_arg; if (kind == CHAR) unsigned_arg = (unsigned char)unsigned_arg; }
                count = snprintf(NULL, 0, spec, unsigned_arg);
            }
        } else if (conversion == 's') {
            const char *guest = va_arg(arguments, const char *);
            if (!guest) string = "(null)";
            else {
                size_t n = 0;
                while ((precision < 0 || n < (size_t)precision) && byte(guest, n)) n++;
                string = malloc(n + 1); if (!string) abort();
                if (n) memcpy(string, GuestRuntime_ResolveData((void *)guest, n), n);
                string[n] = 0;
            }
            count = snprintf(NULL, 0, spec, string);
            pointer = (void *)guest; /* tracks ownership of the temporary */
        } else if (conversion == 'p') { pointer = va_arg(arguments, void *); count = snprintf(NULL, 0, spec, pointer); }
        else if (conversion == 'c') { character = va_arg(arguments, int); count = snprintf(NULL, 0, spec, character); }
        else if (strchr("fFeEgGaA", conversion)) { floating = va_arg(arguments, double); count = snprintf(NULL, 0, spec, floating); }
        else if (conversion == 'n') {
            void *target = va_arg(arguments, void *);
            size_t n = kind == LONG ? sizeof(long) : kind == LONGLONG ? sizeof(long long) : kind == SIZE ? sizeof(size_t) : kind == SHORT ? sizeof(short) : kind == CHAR ? 1 : sizeof(int);
            void *host = GuestRuntime_ResolveData(target, n);
            if (kind == LONG) *(long *)host = (long)result.length;
            else if (kind == LONGLONG) *(long long *)host = (long long)result.length;
            else if (kind == SIZE) *(size_t *)host = result.length;
            else if (kind == SHORT) *(short *)host = (short)result.length;
            else if (kind == CHAR) *(signed char *)host = (signed char)result.length;
            else *(int *)host = (int)result.length;
            continue;
        } else abort();
        if (count < 0) abort();
        char *chunk = malloc((size_t)count + 1); if (!chunk) abort();
        if (integer && signed_value) snprintf(chunk, (size_t)count + 1, spec, signed_arg);
        else if (integer) snprintf(chunk, (size_t)count + 1, spec, unsigned_arg);
        else if (conversion == 's') snprintf(chunk, (size_t)count + 1, spec, string);
        else if (conversion == 'p') snprintf(chunk, (size_t)count + 1, spec, pointer);
        else if (conversion == 'c') snprintf(chunk, (size_t)count + 1, spec, character);
        else snprintf(chunk, (size_t)count + 1, spec, floating);
        append(&result, chunk, (size_t)count); free(chunk);
        if (conversion == 's' && pointer) free(string);
    }
    return result;
}
int GuestRuntime_printf(const char *format, ...)
{
    va_list args; va_start(args, format);
    struct Text text = format_text(format, args); va_end(args);
    if (text.length > INT32_MAX) abort();
    int result = fwrite(text.data, 1, text.length, stdout) == text.length ? (int)text.length : -1;
    free(text.data); return result;
}
int GuestRuntime_sprintf(char *destination, const char *format, ...)
{
    va_list args; va_start(args, format);
    struct Text text = format_text(format, args); va_end(args);
    if (text.length > INT32_MAX) abort();
    GuestRuntime_memcpy(destination, text.data, text.length + 1);
    int result = (int)text.length; free(text.data); return result;
}

static void check_object(size_t required, size_t available)
{
    if (required > available) abort();
}
void *GuestRuntime___memcpy_chk(void *out, const void *in, size_t length, size_t available)
{
    check_object(length, available); return GuestRuntime_memcpy(out, in, length);
}
void *GuestRuntime___memmove_chk(void *out, const void *in, size_t length, size_t available)
{
    check_object(length, available); return GuestRuntime_memmove(out, in, length);
}
void *GuestRuntime___memset_chk(void *out, int value, size_t length, size_t available)
{
    check_object(length, available); return GuestRuntime_memset(out, value, length);
}
char *GuestRuntime___strcpy_chk(char *out, const char *in, size_t available)
{
    size_t length = GuestRuntime_strlen(in);
    if (length == SIZE_MAX) abort();
    check_object(length + 1, available); return GuestRuntime_strcpy(out, in);
}
char *GuestRuntime___strncpy_chk(char *out, const char *in, size_t length, size_t available)
{
    check_object(length, available); return GuestRuntime_strncpy(out, in, length);
}
char *GuestRuntime___strcat_chk(char *out, const char *in, size_t available)
{
    size_t prefix = GuestRuntime_strlen(out), suffix = GuestRuntime_strlen(in);
    if (suffix == SIZE_MAX || prefix > SIZE_MAX - suffix - 1) abort();
    check_object(prefix + suffix + 1, available); return GuestRuntime_strcat(out, in);
}
static int output_text(char *destination, size_t limit, size_t available, int bounded,
                       const char *format, va_list arguments)
{
    if (bounded) check_object(limit, available);
    struct Text text = format_text(format, arguments);
    if (text.length > INT32_MAX) abort();
    size_t length = text.length;
    if (!bounded) {
        check_object(length + 1, available);
        GuestRuntime_memcpy(destination, text.data, length + 1);
    } else if (limit) {
        size_t copied = length < limit - 1 ? length : limit - 1;
        char *host = GuestRuntime_ResolveData(destination, copied + 1);
        memcpy(host, text.data, copied); host[copied] = 0;
    }
    free(text.data); return (int)length;
}
int GuestRuntime_vsnprintf(char *out, size_t limit, const char *format, va_list arguments)
{
    return output_text(out, limit, SIZE_MAX, 1, format, arguments);
}
int GuestRuntime_snprintf(char *out, size_t limit, const char *format, ...)
{
    va_list arguments; va_start(arguments, format);
    int result = GuestRuntime_vsnprintf(out, limit, format, arguments);
    va_end(arguments); return result;
}
int GuestRuntime___vsnprintf_chk(char *out, size_t limit, int flags, size_t available,
                                const char *format, va_list arguments)
{
    if (flags) abort(); /* stronger fortify policies are not implemented */
    return output_text(out, limit, available, 1, format, arguments);
}
int GuestRuntime___snprintf_chk(char *out, size_t limit, int flags, size_t available, const char *format, ...)
{
    va_list arguments; va_start(arguments, format);
    int result = GuestRuntime___vsnprintf_chk(out, limit, flags, available, format, arguments);
    va_end(arguments); return result;
}
int GuestRuntime___sprintf_chk(char *out, int flags, size_t available, const char *format, ...)
{
    if (flags) abort(); /* do not silently weaken nonzero fortify policy */
    va_list arguments; va_start(arguments, format);
    int result = output_text(out, 0, available, 0, format, arguments);
    va_end(arguments); return result;
}
char *GuestRuntime_strchr(const char *string, int value)
{
    for (size_t i = 0;; i++) {
        unsigned char c = byte(string, i);
        if (c == (unsigned char)value) return at(string, i);
        if (!c) return NULL;
    }
}
char *GuestRuntime_strrchr(const char *string, int value)
{
    char *last = NULL;
    for (size_t i = 0;; i++) {
        unsigned char c = byte(string, i);
        if (c == (unsigned char)value) last = at(string, i);
        if (!c) return last;
    }
}
size_t GuestRuntime_strnlen(const char *string, size_t limit)
{
    size_t i = 0;
    while (i < limit && byte(string, i)) i++;
    return i;
}
static int in_set(unsigned char value, const char *set)
{
    for (size_t i = 0;; i++) {
        unsigned char c = byte(set, i);
        if (!c) return 0;
        if (c == value) return 1;
    }
}
size_t GuestRuntime_strcspn(const char *string, const char *reject)
{
    size_t i = 0;
    for (;;) { unsigned char c = byte(string, i); if (!c || in_set(c, reject)) return i; i++; }
}
size_t GuestRuntime_strspn(const char *string, const char *accept)
{
    size_t i = 0;
    for (;;) { unsigned char c = byte(string, i); if (!c || !in_set(c, accept)) return i; i++; }
}
char *GuestRuntime_strstr(const char *string, const char *needle)
{
    size_t length = GuestRuntime_strlen(needle);
    if (!length) return (char *)string;
    for (size_t i = 0; byte(string, i); i++)
        if (GuestRuntime_strncmp(at(string, i), needle, length) == 0) return at(string, i);
    return NULL;
}
void *GuestRuntime_memchr(const void *memory, int value, size_t length)
{
    if (!length) return NULL;
    const unsigned char *host = GuestRuntime_ResolveData((void *)memory, length);
    const unsigned char *found = memchr(host, value, length);
    return found ? at(memory, (size_t)(found - host)) : NULL;
}
