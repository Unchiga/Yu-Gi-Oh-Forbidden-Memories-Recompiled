#ifndef MEMORIES_PC_TEXT_EDIT_H
#define MEMORIES_PC_TEXT_EDIT_H
/* UTF-8 editor model. Byte offsets always point between characters. */
#include <stddef.h>
#include <string.h>
#include <limits.h>
typedef struct { size_t cursor, anchor; int preferred_x; } TextEdit;
static inline size_t TextEdit_Next(const char *s, size_t at)
{
    if (s[at]) { at++; while (((unsigned char)s[at] & 0xc0) == 0x80) at++; }
    return at;
}
static inline size_t TextEdit_Previous(const char *s, size_t at)
{
    if (at) { --at; while (at && ((unsigned char)s[at] & 0xc0) == 0x80) --at; }
    return at;
}
static inline void TextEdit_Move(TextEdit *e, size_t at, int selecting)
{
    e->cursor = at;
    if (!selecting) e->anchor = at;
    e->preferred_x = -1;
}
/* Atomic replacement: reject oversized pastes, normalize CRLF/CR to LF. */
static inline int TextEdit_Insert(TextEdit *e, char *s, size_t capacity, const char *insert)
{
    size_t lo = e->cursor < e->anchor ? e->cursor : e->anchor;
    size_t hi = e->cursor > e->anchor ? e->cursor : e->anchor;
    size_t n = strlen(s), bytes = 0;
    for (const char *p = insert; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        bytes++;
    }
    if (bytes >= capacity || n - (hi - lo) >= capacity - bytes) return 0;
    memmove(s + lo + bytes, s + hi, n - hi + 1);
    size_t at = lo;
    for (const char *p = insert; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        s[at++] = *p == '\r' ? '\n' : *p;
    }
    TextEdit_Move(e, at, 0);
    return 1;
}
typedef int (*TextEditWidth)(const char *);
static inline void TextEdit_Position(const char *s, size_t at, int width, int line,
                                     TextEditWidth measure, int *px, int *py)
{
    int x = 0, y = 0;
    for (size_t i = 0;;) {
        char glyph[8];
        size_t next = TextEdit_Next(s, i), n = next - i;
        if (n > 4) n = 4;
        memcpy(glyph, s + i, n); glyph[n] = 0;
        int advance = s[i] == '\n' ? 0 : measure(glyph);
        if (x && x + advance > width) { x = 0; y += line; }
        if (i >= at || !s[i]) { *px = x; *py = y; return; }
        if (s[i] == '\n') { x = 0; y += line; }
        else x += advance;
        i = next;
    }
}
static inline size_t TextEdit_Hit(const char *s, int width, int line, TextEditWidth measure, int px, int py)
{
    size_t best = 0;
    int best_y = INT_MAX, best_x = INT_MAX;
    if (py < 0) return 0;
    py = py / line * line;
    int x = 0, y = 0;
    for (size_t i = 0;;) {
        char glyph[8];
        size_t next = TextEdit_Next(s, i), n = next - i;
        if (n > 4) n = 4;
        memcpy(glyph, s + i, n); glyph[n] = 0;
        int advance = s[i] == '\n' ? 0 : measure(glyph);
        if (x && x + advance > width) { x = 0; y += line; }
        int dy = y > py ? y - py : py - y, dx = x > px ? x - px : px - x;
        if (dy < best_y || (dy == best_y && dx < best_x)) { best = i; best_y = dy; best_x = dx; }
        if (!s[i]) return best;
        if (s[i] == '\n') { x = 0; y += line; }
        else x += advance;
        i = next;
    }
}
#endif
