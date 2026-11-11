#include "pc/platform/text_edit.h"
#include <assert.h>
#include <stdio.h>
static int glyph_width(const char *s) { return *s ? 10 : 0; }
int main(void)
{
    TextEdit e = {0, 0, -1};
    char text[128] = "Hello world";
    TextEdit_Move(&e, 5, 0);
    assert(TextEdit_Insert(&e, text, sizeof(text), "\r\n\xf0\x9f\x98\x80\r"));
    assert(!strcmp(text, "Hello\n\xf0\x9f\x98\x80\n world"));
    assert(e.cursor == 11 && e.anchor == 11);
    TextEdit_Move(&e, 6, 0);
    TextEdit_Move(&e, TextEdit_Next(text, e.cursor), 1);
    assert(e.cursor == 10 && TextEdit_Previous(text, e.cursor) == 6);
    assert(TextEdit_Insert(&e, text, sizeof(text), "star"));
    assert(!strcmp(text, "Hello\nstar\n world"));
    /* Backwards selection and empty replacement (cut/delete). */
    e.cursor = 6; e.anchor = 10;
    assert(TextEdit_Insert(&e, text, sizeof(text), ""));
    assert(!strcmp(text, "Hello\n\n world"));
    char before[128]; strcpy(before, text);
    assert(!TextEdit_Insert(&e, text, strlen(text) + 1, "more"));
    assert(!strcmp(text, before) && e.cursor == 6);
    /* Hit testing and caret positioning agree across soft/hard wraps. */
    strcpy(text, "ab\xf0\x9f\x98\x80" "d\n\nef");
    for (size_t i = 0;; i = TextEdit_Next(text, i)) {
        int x, y;
        TextEdit_Position(text, i, 20, 22, glyph_width, &x, &y);
        assert(TextEdit_Hit(text, 20, 22, glyph_width, x, y) == i);
        if (!text[i]) break;
    }
    assert(TextEdit_Hit(text, 20, 22, glyph_width, 0, -20) == 0);
    assert(TextEdit_Hit(text, 20, 22, glyph_width, 500, 500) == strlen(text));
    puts("UTF-8 insertion, paste normalization, selection, deletion, capacity and wrapped hit testing: passed");
    return 0;
}
