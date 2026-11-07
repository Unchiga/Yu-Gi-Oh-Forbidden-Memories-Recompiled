/* The hash tables behind Cards_Named find what going down the names in order
 * found: the same id for every spelling, the lower of two that match. */
#include "pc/cards/name_index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT 722
#define WIDE 64
static char names[COUNT + 1][WIDE];

static int linear(const char *text)
{
    int id;
    for (id = 1; id <= COUNT; id++)
        if (Names_SameWords(text, names[id])) return id;
    for (id = 1; id <= COUNT; id++)
        if (Names_SameLetters(text, names[id])) return id;
    return 0;
}

static unsigned state = 0x2545F491u;
static unsigned next_random(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* Few letters, so that many names differ only in case or punctuation. */
static void random_name(char *out)
{
    static const char pieces[] = "abAB-  '.1";
    int length = (int)(next_random() % 9);
    for (int i = 0; i < length; i++) out[i] = pieces[next_random() % (sizeof(pieces) - 1)];
    out[length] = 0;
}

/* The same name, spelled another way. */
static void respell(const char *from, char *out)
{
    int used = 0;
    for (; *from && used < WIDE - 3; from++) {
        unsigned what = next_random() % 8;
        if (what == 0) continue;
        if (what == 1) out[used++] = '-';
        out[used++] = what < 4 ? (char)toupper((unsigned char)*from) : *from;
    }
    out[used] = 0;
}

int main(void)
{
    static NameIndex index;
    char text[WIDE];
    int id, found = 0;
    for (id = 1; id <= COUNT; id++) random_name(names[id]);
    /* Names with a bytes above ASCII, which the letters comparison skips. */
    strcpy(names[10], "Caf\xc3\xa9");
    strcpy(names[11], "Caf");
    for (id = 1; id <= COUNT; id++) NameIndex_Add(&index, id, names[id]);
    for (id = 1; id <= COUNT; id++) {
        assert(NameIndex_Find(&index, names[id], names[0], WIDE) == linear(names[id]));
    }
    for (int round = 0; round < 200000; round++) {
        if (round % 2) respell(names[1 + next_random() % COUNT], text);
        else random_name(text);
        id = linear(text);
        found += id != 0;
        assert(NameIndex_Find(&index, text, names[0], WIDE) == id);
    }
    assert(found > 50000);
    assert(NameIndex_Find(&index, "Caf\xc3\xa9", names[0], WIDE) == 10);
    assert(NameIndex_Find(&index, "CAF", names[0], WIDE) == 11);
    assert(!NameIndex_Find(&index, "no such card anywhere", names[0], WIDE));
    puts("Name index: the same ids as going down the names");
    return 0;
}
