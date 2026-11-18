/* The letters text can be written in (glyphs.h, notes/translation.md). */
#include "pc/compat/font.h" /* a mod's font under a folder named with accents */
#include "glyphs.h"
#include "serif.h"
#include "pc/render/soft_gpu.h"
#include "pc/debug/log.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef _WIN32
#include "pc/platform/win32.h"
#elif defined(__APPLE__)
#include "pc/platform/macos_fonts.h"
#else
#include <fontconfig/fontconfig.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_TABLE 0x801D9000u
#define RETAIL_CODES 0x5C
#define ADDED_SJIS 0xF040u           /* Shift-JIS's user area: no retail glyph has one */
#define ADDED_MAX 672                /* small cells in one page: 32 x 21 of 8x12 */
/* The 8x8 font (the results' headings): its pictures at (704, 0), a glyph's
 * cell at u (index & 15) * 8 + 0x80, v (index >> 4) * 8 of that page, the
 * index being bits 20-27 of the glyph's word (func_80035E20). */
#define TINY_PAGE 0xB
#define TINY_COLON 0xBC              /* the index ':' stands in with: the 8x8 font's '·' */

/* The fonts' sizes. */
enum { FONT_SMALL, FONT_LARGE, FONT_TINY };

/* --- the retail glyphs ------------------------------------------------ */

/* The Shift-JIS the retail glyphs are, and the characters they write
 * (the letters and digits are worked out). */
static const struct { uint16_t sjis; uint32_t character; } punctuation[] = {
    {0x8149, '!'}, {0x8168, '"'}, {0x8194, '#'}, {0x8190, '$'}, {0x8193, '%'}, {0x8195, '&'}, {0x8166, '\''},
    {0x8169, '('}, {0x816A, ')'}, {0x8196, '*'}, {0x817B, '+'}, {0x8143, ','}, {0x817C, '-'}, {0x8144, '.'},
    {0x815E, '/'}, {0x8146, ':'}, {0x8147, ';'}, {0x8183, '<'}, {0x8181, '='}, {0x8184, '>'}, {0x8148, '?'},
    {0x8173, 0xAB}, {0x8174, 0xBB}, {0x8145, 0xB7}, {0x818A, 0x2640}, {0x8189, 0x2642}, {0x81BC, 0x2282},
    {0x81BD, 0x2283}, {0x83BF, 0x3B1}, {0x83C0, 0x3B2}, {0x83C1, 0x3B3}, {0x81A9, 0x2190}, {0x81A8, 0x2192}};
/* Other ways to type a retail glyph. */
static const struct { uint32_t typed, character; } aliases[] = {
    {0x2019, '\''}, {0x2018, '\''}, {0x201D, '"'}, {0x201C, '"'}, {0x2212, '-'}, {0x2013, '-'}, {0x2014, '-'},
    {0x300A, 0xAB}, {0x300B, 0xBB}, {0x30FB, 0xB7}, {0xA0, ' '}};

static uint32_t retail[RETAIL_CODES];   /* by code: the character, 0 for none */
static int retail_read;

static uint32_t sjis_character(unsigned sjis)
{
    size_t i;
    if (sjis >= 0x824F && sjis <= 0x8258) return '0' + (sjis - 0x824F);
    if (sjis >= 0x8260 && sjis <= 0x8279) return 'A' + (sjis - 0x8260);
    if (sjis >= 0x8281 && sjis <= 0x829A) return 'a' + (sjis - 0x8281);
    for (i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        if (punctuation[i].sjis == sjis) return punctuation[i].character;
    }
    return 0;
}

static unsigned character_sjis(uint32_t character)
{
    size_t i;
    if (character >= '0' && character <= '9') return 0x824F + (character - '0');
    if (character >= 'A' && character <= 'Z') return 0x8260 + (character - 'A');
    if (character >= 'a' && character <= 'z') return 0x8281 + (character - 'a');
    for (i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        if (punctuation[i].character == character) return punctuation[i].sjis;
    }
    return 0;
}

static const uint32_t *table(void)
{
    return (const uint32_t *)(uintptr_t)GLYPH_TABLE;
}

static void read_retail(void)
{
    int code;
    if (retail_read) return;
    retail_read = 1;
    retail[0] = ' ';
    for (code = 1; code < RETAIL_CODES; code++) retail[code] = sjis_character(table()[code] & 0xFFFF);
}

static int retail_code(uint32_t character)
{
    int code;
    size_t i;
    read_retail();
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (aliases[i].typed == character) character = aliases[i].character;
    }
    for (code = 0; code < RETAIL_CODES; code++) {
        if (retail[code] == character) return code;
    }
    return -1;
}

/* --- what an added glyph is made of ----------------------------------- */

enum {
    MARK_NONE, MARK_GRAVE, MARK_ACUTE, MARK_CIRCUMFLEX, MARK_TILDE, MARK_MACRON, MARK_BREVE, MARK_DOT,
    MARK_DIAERESIS, MARK_RING, MARK_DOUBLE_ACUTE, MARK_CARON, MARK_CEDILLA, MARK_OGONEK, MARK_HOOK,
    MARK_DOT_BELOW, MARK_HORN, MARK_COUNT,
    /* Not marks of Unicode's, but drawn the same way. */
    MARK_FLIP = MARK_COUNT, MARK_DOTLESS, MARK_SLASH, MARK_BAR
};

/* A letter with two marks (Vietnamese's) has its shape mark (^, the breve
 * or the horn) as `mark` and its tone as `tone`. */
static const struct { uint32_t character; char base; unsigned char mark, tone; } accents[] = {
#include "accents.inc"
    {0x00BF, '?', MARK_FLIP, MARK_NONE}, {0x00A1, '!', MARK_FLIP, MARK_NONE}, {0x0131, 'i', MARK_DOTLESS, MARK_NONE},
    {0x00F8, 'o', MARK_SLASH, MARK_NONE}, {0x00D8, 'O', MARK_SLASH, MARK_NONE}, {0x0142, 'l', MARK_SLASH, MARK_NONE},
    {0x0141, 'L', MARK_SLASH, MARK_NONE}, {0x0111, 'd', MARK_BAR, MARK_NONE}, {0x0110, 'D', MARK_BAR, MARK_NONE},
    {0x0127, 'h', MARK_BAR, MARK_NONE}, {0x0126, 'H', MARK_BAR, MARK_NONE},
};

/* The marks, drawn for the two sizes: '#' is the mark, and the retail
 * glyphs' dark outline goes round it. Rows top to bottom. */
static const char *const small_marks[MARK_COUNT][3] = {
    {0}, {"#..", ".#."}, {"..#", ".#."}, {".#.", "#.#"}, {".#.#", "#.#."}, {"###"}, {"#..#", ".##."},
    {"#"}, {"#.#"}, {".#.", "#.#", ".#."}, {".#.#", "#.#."}, {"#.#", ".#."}, {".#", "#."}, {"#.", ".#"},
    {"##", ".#"}, {"#"}, {0}};
static const char *const large_marks[MARK_COUNT][3] = {
    {0}, {"##...", ".##..", "..##."}, {"...##", "..##.", ".##.."}, {".##.", "#..#"}, {".##..#", "#..##."},
    {"#####"}, {"#..#", ".##."}, {"##", "##"}, {"##.##", "##.##"}, {".##.", "#..#", ".##."},
    {"..#..#", ".#..#.", "#..#.."}, {"#..#", ".##."}, {"..##", "...#", ".##."}, {"##..", ".##."},
    {"###.", "...#", "..#."}, {"##", "##"}, {0}};

static const struct {
    uint32_t character;
    const char *small[12], *large[16];
} letters[] = {
#include "letters.inc"
};

typedef struct {
    uint32_t character;
    int base;              /* the retail glyph it stands for where it is not drawn */
    char letter;           /* composed: the retail letter */
    unsigned char mark, tone;
    unsigned char made;    /* its pictures are in the bank */
    unsigned char made_tiny; /* and its 8x8 one */
    short letter_shape;    /* one of `letters`, or -1 */
} Added;

static Added added[ADDED_MAX];
static int added_count, tiny_colon_made;
static int european;   /* Glyphs_SetEuropean */

uint32_t Glyphs_Character(int code)
{
    read_retail();
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count)
        return added[code - GLYPHS_EXTENDED_FIRST].character;
    return code >= 0 && code < RETAIL_CODES ? retail[code] : 0;
}

/* --- fonts ---------------------------------------------------------------- */

static FT_Library library;
static FT_Face faces[8];
static int face_count, system_tried;

static void open_face(const char *path)
{
    if (!path || face_count >= (int)(sizeof(faces) / sizeof(faces[0]))) return;
    if (!library && FT_Init_FreeType(&library)) return;
    if (FT_New_Face(library, path, 0, &faces[face_count]) == 0) {
        LOG(LOG_MODS, "glyphs: font %s", path);
        face_count++;
    }
}

void Glyphs_AddFont(const char *path)
{
    open_face(path);
}

static void open_system_face(void)
{
    if (system_tried) return;
    system_tried = 1;
#ifdef _WIN32
    open_face(Win32_FontPath(0));
#elif defined(__APPLE__)
    open_face(MacOS_FontPath(MEMORIES_FONT_BOLD));
#else
    {
        FcPattern *pattern, *match;
        FcResult result;
        FcChar8 *file = NULL;
        if (!FcInit()) return;
        pattern = FcNameParse((const FcChar8 *)"sans-serif:bold");
        FcConfigSubstitute(NULL, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        match = FcFontMatch(NULL, pattern, &result);
        if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) open_face((const char *)file);
        if (match) FcPatternDestroy(match);
        FcPatternDestroy(pattern);
    }
#endif
}

static FT_Face face_for(uint32_t character)
{
    int i;
    open_system_face();
    for (i = 0; i < face_count; i++) {
        if (FT_Get_Char_Index(faces[i], character)) return faces[i];
    }
    return NULL;
}

/* --- codes ---------------------------------------------------------------- */

int Glyphs_Code(uint32_t character)
{
    int code = retail_code(character), i;
    size_t a;
    Added glyph;
    if (code >= 0) return code;
    for (i = 0; i < added_count; i++) {
        if (added[i].character == character) return GLYPHS_EXTENDED_FIRST + i;
    }
    if (added_count >= ADDED_MAX) return -1;
    memset(&glyph, 0, sizeof(glyph));
    glyph.character = character;
    glyph.letter_shape = -1;
    for (a = 0; a < sizeof(letters) / sizeof(letters[0]); a++) {
        if (letters[a].character == character) glyph.letter_shape = (short)a;
    }
    for (a = 0; a < sizeof(accents) / sizeof(accents[0]); a++) {
        if (accents[a].character == character && retail_code((unsigned char)accents[a].base) >= 0) {
            glyph.letter = accents[a].base;
            glyph.mark = accents[a].mark;
            glyph.tone = accents[a].tone;
            glyph.base = retail_code((unsigned char)accents[a].base);
            break;
        }
    }
    if (!glyph.letter && glyph.letter_shape < 0 && !face_for(character)) return -1;
    added[added_count] = glyph;
    return GLYPHS_EXTENDED_FIRST + added_count++;
}

uint32_t Glyphs_Word(int code)
{
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        const Added *glyph = &added[code - GLYPHS_EXTENDED_FIRST];
        /* Its own character; the small font's index and the mouth's kind
         * are its retail letter's. */
        uint32_t base = glyph->letter ? table()[glyph->base] : 0;
        return (ADDED_SJIS + (uint32_t)(code - GLYPHS_EXTENDED_FIRST)) | (base & 0x0FF70000u);
    }
    return code >= 0 && code < RETAIL_CODES ? table()[code] : 0;
}

uint32_t Glyphs_SortCharacter(int code)
{
    uint32_t character;
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        const Added *glyph = &added[code - GLYPHS_EXTENDED_FIRST];
        character = glyph->letter ? (unsigned char)glyph->letter : glyph->character;
    } else {
        character = Glyphs_Character(code);
    }
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
}

int Glyphs_Base(int code)
{
    if (code >= GLYPHS_EXTENDED_FIRST && code < GLYPHS_EXTENDED_FIRST + added_count) {
        return added[code - GLYPHS_EXTENDED_FIRST].base;
    }
    return code;
}

/* --- pictures ------------------------------------------------------------- */

typedef struct {
    int width, height;
    unsigned char pixels[16][16];   /* 4-bit indices into the text palettes */
} Cell;

/* Where the retail font has a glyph in its page, as func_80035E20 finds it:
 * letters and digits by their Shift-JIS, punctuation by its list. */
static int retail_cell(unsigned sjis, int large, int *u, int *v)
{
    static const unsigned short listed[] = {0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169, 0x816A,
                                            0x8196, 0x817B, 0x8143, 0x817C, 0x8144, 0x815E, 0x8146, 0x8147, 0x8183,
                                            0x8181, 0x8184, 0x8148, 0x8140};
    int i;
    if (sjis - 0x824Fu < 0x4C && !(sjis - 0x8259u < 7 || sjis - 0x827Au < 7)) {
        *u = (int)(sjis & 0xF) * (large ? 16 : 8);
        *v = large ? (int)((sjis - 0x8240) >> 4) * 16 + 0x48 : (int)((sjis - 0x8240) >> 4) * 12;
        return 1;
    }
    for (i = 0; i < (int)(sizeof(listed) / sizeof(listed[0])); i++) {
        if (listed[i] != sjis) continue;
        if (i < 15) {
            *u = i * (large ? 16 : 8);
            *v = large ? 0x48 : 0;
        } else {
            *u = large ? i * 16 - 0x60 : i * 8 - 0x30;
            *v = large ? 0x58 : 0xC;
        }
        return 1;
    }
    return 0;
}

static int page_x(int page) { return (page & 0xF) * 64; }
static int page_y(int page) { return ((page >> 4) & 1) * 256; }

/* A retail glyph's index in the 8x8 font, 0 for none. */
static int tiny_index(int code)
{
    return code >= 0 && code < RETAIL_CODES ? (int)((table()[code] >> 20) & 0xFF) : 0;
}

static void read_cell(int font_page, char letter, int size, Cell *cell)
{
    const uint16_t *vram = SoftGpu_VramRaw();
    int u, v, x, y, large = size == FONT_LARGE;
    memset(cell, 0, sizeof(*cell));
    cell->width = large ? 16 : 8;
    cell->height = large ? 16 : size == FONT_TINY ? 8 : 12;
    if (size == FONT_TINY) {
        /* A letter, or with `letter` 0 the index in `font_page`. */
        int index = letter ? tiny_index(retail_code((unsigned char)letter)) : font_page;
        if (!index) return;
        font_page = TINY_PAGE;
        u = (index & 0xF) * 8 + 0x80;
        v = (index >> 4) * 8;
    } else if (!retail_cell(character_sjis((unsigned char)letter), large, &u, &v)) {
        return;
    }
    SoftGpu_Settle(page_x(font_page) + u / 4, page_y(font_page) + v, (u % 4 + cell->width + 3) / 4, cell->height);
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int tu = u + x;
            uint16_t word = vram[(page_y(font_page) + v + y) * SOFT_GPU_WIDTH + page_x(font_page) + tu / 4];
            cell->pixels[y][x] = (unsigned char)((word >> ((tu & 3) * 4)) & 0xF);
        }
    }
}

static int ink(const Cell *cell, int *top, int *bottom, int *left, int *right)
{
    int x, y, any = 0;
    *top = cell->height; *bottom = -1; *left = cell->width; *right = -1;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            if (!cell->pixels[y][x]) continue;
            any = 1;
            if (y < *top) *top = y;
            if (y > *bottom) *bottom = y;
            if (x < *left) *left = x;
            if (x > *right) *right = x;
        }
    }
    return any;
}

/* The letter's own color: the most common of its bright indices. */
static int bright(const Cell *cell)
{
    int count[16] = {0}, x, y, best = 14;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) count[cell->pixels[y][x]]++;
    }
    for (x = 8; x < 16; x++) {
        if (count[x] > count[best] || (count[best] == 0 && count[x])) best = x;
    }
    return best;
}

/* Rows top..bottom of the glyph fitted into rows to..bottom, first and last
 * rows kept: room above for a mark, on the same baseline. */
static void squash(Cell *cell, int top, int bottom, int to)
{
    Cell source = *cell;
    int y;
    for (y = 0; y < cell->height; y++) {
        if (y < to || y > bottom) {
            if (y < to) memset(cell->pixels[y], 0, sizeof(cell->pixels[y]));
            continue;
        }
        memcpy(cell->pixels[y], source.pixels[top + ((y - to) * (bottom - top) + (bottom - to) / 2) / (bottom - to)],
               sizeof(cell->pixels[y]));
    }
}

/* What a pixel is to the shape: nothing, outline or ink. */
static int kind(const Cell *cell, int x, int y, int outline)
{
    int index = cell->pixels[y][x];
    return !index ? 0 : index == outline ? 1 : 2;
}

static int rows_differ(const Cell *cell, int a, int b, int outline)
{
    int x, count = 0;
    for (x = 0; x < cell->width; x++) count += kind(cell, x, a, outline) != kind(cell, x, b, outline);
    return count;
}

/* `count` rows taken out of the glyph (top..bottom, its outline included)
 * for a mark to go above it, on the same baseline: each time the inner row
 * most like a neighbour, nearer the middle on a tie, so a thick stroke
 * thins and a thin one stays; its first and last rows of ink stay. What is
 * above moves down. */
static void drop_rows(Cell *cell, int top, int bottom, int count, int outline)
{
    while (count-- > 0) {
        int y, best = -1, best_score = 0;
        for (y = top + 2; y <= bottom - 2; y++) {
            int above = rows_differ(cell, y, y - 1, outline), below = rows_differ(cell, y, y + 1, outline);
            int middle = 2 * y - top - bottom;
            int score = (above < below ? above : below) * 4 + (middle < 0 ? -middle : middle);
            if (best < 0 || score < best_score) {
                best = y;
                best_score = score;
            }
        }
        if (best < 0) return;
        for (y = best; y > 0; y--) memcpy(cell->pixels[y], cell->pixels[y - 1], sizeof(cell->pixels[y]));
        memset(cell->pixels[0], 0, sizeof(cell->pixels[0]));
        top++;
    }
}

/* The 8x8 font's colors, which are not the others' (its outline is 15):
 * the outline is the index most pixels next to nothing have, the letter the
 * most common of the rest. */
static void tiny_colors(const Cell *cell, int *fill, int *outline)
{
    int edge[16] = {0}, inner[16] = {0}, x, y, i;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int index = cell->pixels[y][x];
            if (!index) continue;
            if (x == 0 || y == 0 || x == cell->width - 1 || y == cell->height - 1 || !cell->pixels[y][x - 1] ||
                !cell->pixels[y][x + 1] || !cell->pixels[y - 1][x] || !cell->pixels[y + 1][x])
                edge[index]++;
            else
                inner[index]++;
        }
    }
    *outline = 15;
    for (i = 1; i < 16; i++) {
        if (edge[i] > edge[*outline]) *outline = i;
    }
    *fill = 0;
    for (i = 1; i < 16; i++) {
        if (i != *outline && (!*fill || inner[i] > inner[*fill])) *fill = i;
    }
}

/* Mark pixels in the fill color, and the outline round them. */
static void stamp(Cell *cell, const int (*points)[2], int count, int fill, int outline)
{
    int i, dx, dy;
    for (i = 0; i < count; i++) {
        int x = points[i][0], y = points[i][1];
        if (x >= 0 && x < cell->width && y >= 0 && y < cell->height) cell->pixels[y][x] = (unsigned char)fill;
    }
    for (i = 0; i < count; i++) {
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                int x = points[i][0] + dx, y = points[i][1] + dy;
                if (x >= 0 && x < cell->width && y >= 0 && y < cell->height && !cell->pixels[y][x])
                    cell->pixels[y][x] = (unsigned char)outline;
            }
        }
    }
}

/* Marks beside another, for the tone the small fonts have no rows to stack
 * over ^ or the breve (Vietnamese's): two rows, three columns at most, so
 * that the two and a column between fit the cell. */
static const char *const side_marks[MARK_COUNT][3] = {
    {0}, {"#.", ".#"}, {".#", "#."}, {".#.", "#.#"}, {".##", "##."}, {0}, {"#.#", ".#."},
    {0}, {0}, {0}, {0}, {0}, {0}, {0}, {"##", ".#"}, {0}, {0}};

static int mark_width(const char *const *rows)
{
    int y, width = 0;
    for (y = 0; y < 3 && rows[y]; y++) {
        if ((int)strlen(rows[y]) > width) width = (int)strlen(rows[y]);
    }
    return width;
}

/* Whether a mark put at row `at`, column 0, would touch (or run into) what
 * `grid` has from that row up. */
static int touches(char grid[8][16], int at, const char *const *rows)
{
    int y, x, dy, dx;
    for (y = 0; y < 3 && rows[y]; y++) {
        for (x = 0; rows[y][x]; x++) {
            if (rows[y][x] != '#') continue;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int gy = at + y + dy, gx = x + dx;
                    if (gy >= 0 && gy < 8 && gx >= 0 && gx < 16 && grid[gy][gx] == '#') return 1;
                }
            }
        }
    }
    return 0;
}

/* A mark's rows into `grid` from row `at`, column `column`; returns the row
 * after it. */
static int mark_rows(char grid[8][16], int at, int column, const char *const *rows)
{
    int y, x;
    for (y = 0; y < 3 && rows[y] && at + y < 8; y++) {
        for (x = 0; x < column + (int)strlen(rows[y]) && x < 15; x++) {
            if (!grid[at + y][x]) grid[at + y][x] = '.';
        }
        for (x = 0; rows[y][x] && column + x < 15; x++) {
            if (rows[y][x] == '#') grid[at + y][column + x] = '#';
        }
    }
    return at + y;
}

/* With a European language, whether a letter made of `letter` and these
 * marks gets the PAL's serifs (serif.h): an i or l, dotless or with marks
 * over it; one with a mark below or through it (whose stem is not the
 * lowest ink) stays as the US letter is. */
static int serifed(char letter, int mark, int tone)
{
    int i, marks[2];
    if (!european || (letter != 'i' && letter != 'l')) return 0;
    marks[0] = mark;
    marks[1] = tone;
    for (i = 0; i < 2; i++) {
        switch (marks[i]) {
        case MARK_CEDILLA: case MARK_OGONEK: case MARK_DOT_BELOW: case MARK_HORN: case MARK_FLIP: case MARK_SLASH:
        case MARK_BAR:
            return 0;
        default:
            break;
        }
    }
    return 1;
}

static void add_serifs(Cell *cell, int large)
{
    Serif_Add(&cell->pixels[0][0], 16, cell->width, cell->height, SERIF_REACH(large), 1);
}

static void compose(const Added *glyph, int font_page, int size, Cell *cell)
{
    int top, bottom, left, right, fill, outline = 1, count = 0, x, y, large = size == FONT_LARGE;
    int points[64][2];
    /* The PAL's serifs, which the 8x8 font's letters do not get (the PAL
     * has only digits in its 8x8 font). */
    int serifs = size != FONT_TINY && serifed(glyph->letter, glyph->mark, glyph->tone);
    read_cell(font_page, glyph->letter, size, cell);
    if (!ink(cell, &top, &bottom, &left, &right)) return;
    if (size == FONT_TINY) tiny_colors(cell, &fill, &outline);
    else fill = bright(cell);
    if ((glyph->letter == 'i' || glyph->letter == 'j') && glyph->mark != MARK_CEDILLA && glyph->mark != MARK_OGONEK &&
        glyph->mark != MARK_DOT_BELOW) {
        /* The dot goes: the letter keeps what is below the x-height. */
        Cell n;
        int n_top, n_bottom, n_left, n_right;
        read_cell(font_page, 'n', size, &n);
        if (ink(&n, &n_top, &n_bottom, &n_left, &n_right)) {
            for (y = 0; y < n_top; y++) memset(cell->pixels[y], 0, sizeof(cell->pixels[y]));
            ink(cell, &top, &bottom, &left, &right);
        }
    }
    if (serifs) {
        /* What is left above the stem's top outline goes too: the small
         * font's i has its dot's lower row where the n starts. The mark
         * then stands clear of the stem, a row between, as the PAL's do. */
        SerifStem stem;
        if (Serif_Stem(&cell->pixels[0][0], 16, cell->width, cell->height, &stem)) {
            for (y = 0; y < stem.top - 1; y++) memset(cell->pixels[y], 0, sizeof(cell->pixels[y]));
            ink(cell, &top, &bottom, &left, &right);
        }
    }
    if (glyph->mark == MARK_DOTLESS) {
        if (serifs) add_serifs(cell, large);
        return;
    }
    if (glyph->mark == MARK_FLIP) {
        /* Turned upside down within its ink, as an inverted ? and ! are. */
        Cell source = *cell;
        for (y = top; y <= bottom; y++) {
            for (x = left; x <= right; x++) cell->pixels[y][x] = source.pixels[top + bottom - y][left + right - x];
        }
        return;
    }
    if (glyph->mark == MARK_SLASH || glyph->mark == MARK_BAR) {
        int height = bottom - top;
        if (glyph->mark == MARK_BAR) {
            y = top + height / 4 + 1;
            for (x = left > 0 ? left - 1 : 0; x <= right && count < 64; x++) {
                points[count][0] = x; points[count][1] = y; count++;
            }
        } else {
            int from = glyph->letter == 'l' || glyph->letter == 'L' ? top + height / 3 : top + 1;
            int to = glyph->letter == 'l' || glyph->letter == 'L' ? top + (2 * height) / 3 : bottom - 1;
            for (y = from; y <= to && count < 64; y++) {
                points[count][0] = right - 1 - (y - from) * (right - left - 2) / (to - from > 0 ? to - from : 1);
                points[count][1] = y;
                count++;
            }
        }
        stamp(cell, (const int (*)[2])points, count, fill, outline);
        return;
    }
    {
        /* The marks: above (one, or Vietnamese's two), below, and the
         * horn, which is part of the letter's right side. */
        int above[2], above_count = 0, below = MARK_NONE, horn = 0, i, x0, y0, height = 0, width = 0;
        int marks[2], letter_top, letter_right;
        char grid[8][16];
        marks[0] = glyph->mark;
        marks[1] = glyph->tone;
        for (i = 0; i < 2; i++) {
            if (marks[i] == MARK_NONE) continue;
            if (marks[i] == MARK_HORN) horn = 1;
            else if (marks[i] == MARK_CEDILLA || marks[i] == MARK_OGONEK || marks[i] == MARK_DOT_BELOW) below = marks[i];
            else above[above_count++] = marks[i];
        }
        memset(grid, 0, sizeof(grid));
        if (above_count == 2 && large) {
            /* Stacked: the tone over the shape mark and to its right, as
             * Vietnamese type sets it, its last row beside the mark's
             * first, as far right as it takes not to touch it; so the
             * letter gives up one row more than for one mark, not four. */
            const char *const *shape = large_marks[above[0]], *const *tone = large_marks[above[1]];
            int tone_high = 0, column;
            while (tone_high < 3 && tone[tone_high]) tone_high++;
            for (column = 0; column < 8; column++) {
                memset(grid, 0, sizeof(grid));
                mark_rows(grid, 0, column, tone);
                if (!touches(grid, tone_high - 1, shape)) break;
            }
            height = mark_rows(grid, tone_high - 1, 0, shape);
        } else if (above_count == 2) {
            /* The small fonts have no rows to stack them in: the tone goes
             * beside the shape mark, on its right, as the compact Vietnamese
             * fonts have it. The 8x8 font leaves the tone out if the two
             * do not fit (the letter keeps its ^ or breve, and is never
             * left out). */
            const char *const *shape = side_marks[above[0]], *const *tone = side_marks[above[1]];
            height = mark_rows(grid, 0, 0, shape);
            if (mark_width(shape) + 1 + mark_width(tone) <= cell->width - (size == FONT_TINY)) {
                int tone_height = mark_rows(grid, 0, mark_width(shape) + 1, tone);
                if (tone_height > height) height = tone_height;
            }
        } else if (above_count == 1) {
            height = mark_rows(grid, 0, 0, large ? large_marks[above[0]] : small_marks[above[0]]);
        }
        for (y = 0; y < height; y++) {
            if ((int)strlen(grid[y]) > width) width = (int)strlen(grid[y]);
        }
        y0 = 0;
        if (!height) {
        } else if (large && top < height + 1) {
            squash(cell, top, bottom, height + 1);
            ink(cell, &top, &bottom, &left, &right);
        } else if (!large && top < height) {
            /* A capital in the small fonts, its outline on the cell's top
             * row: it gives up a row of its body (two for a three-row
             * mark), and the mark's last row takes its top outline's place,
             * on the letter, so the capital stays taller than the small
             * letters (notes/translation.md). */
            int target = height > 2 ? height - 1 : 1;
            if (top < target) {
                drop_rows(cell, top, bottom, target - top, outline);
                ink(cell, &top, &bottom, &left, &right);
            }
        } else if (!large && top == height) {
            /* The letter's top outline is the mark's below it. */
        } else {
            y0 = top - 1 - height;
        }
        x0 = (left + right + 1) / 2 - width / 2;
        /* The PAL's acute over a narrow letter is a column left of where
         * it goes over the rest, on the stem (its grave is where this one
         * is): the small font's two pixels, which otherwise lean right. */
        if (serifs && !large && above_count == 1 && above[0] == MARK_ACUTE) x0--;
        /* Two: the shape mark over the letter's middle, the tone beside. */
        if (above_count == 2) x0 = (left + right + 1) / 2 - mark_width(large ? large_marks[above[0]] : side_marks[above[0]]) / 2;
        if (above_count == 2 && x0 + width > cell->width) x0 = cell->width - width;
        if (above_count == 2 && x0 < 0) x0 = 0;
        letter_top = top;
        letter_right = right;
        for (y = 0; y < height; y++) {
            for (x = 0; grid[y][x] && count < 64; x++) {
                if (grid[y][x] != '#') continue;
                points[count][0] = x0 + x; points[count][1] = y0 + y; count++;
            }
        }
        if (below == MARK_DOT_BELOW) {
            /* Under the letter, a row of its outline between; where there
             * is no room, on the cell's last row, and under a tail (a y's)
             * beside it, under the right arm, just below the line. */
            int dot = large ? 2 : 1, cx = (left + right + 1) / 2 - dot / 2;
            y0 = bottom + 1;
            if (y0 + dot > cell->height) {
                Cell n;
                int n_top, n_bottom, n_left, n_right;
                read_cell(font_page, 'n', size, &n);
                y0 = cell->height - dot;
                if (ink(&n, &n_top, &n_bottom, &n_left, &n_right) && n_bottom < bottom && n_bottom + dot <= cell->height) {
                    y0 = n_bottom;
                    cx = right - dot;
                }
            }
            for (y = 0; y < dot; y++) {
                for (x = 0; x < dot && count < 64; x++) {
                    points[count][0] = cx + x; points[count][1] = y0 + y; count++;
                }
            }
            /* Where it sits right under the letter's ink (the small fonts'
             * last row is the letters' foot), that pixel is outline: the
             * dot stays a dot, not a longer stem. */
            for (x = cx; x < cx + dot && y0 > 0; x++) {
                if (x >= 0 && x < cell->width && cell->pixels[y0 - 1][x] && cell->pixels[y0 - 1][x] != outline)
                    cell->pixels[y0 - 1][x] = (unsigned char)outline;
            }
        } else if (below) {
            static const char *const tiny_below[3] = {"#"};
            const char *const *rows = size == FONT_TINY ? tiny_below   /* the 8x8 font has one row below: a dot */
                : large ? large_marks[below] : small_marks[below];
            int rows_high = 0, rows_wide = mark_width(rows);
            while (rows_high < 3 && rows[rows_high]) rows_high++;
            y0 = bottom < cell->height - rows_high ? bottom : cell->height - rows_high;
            x0 = below == MARK_OGONEK ? right - rows_wide + 1 : (left + right + 1) / 2 - rows_wide / 2;
            for (y = 0; y < rows_high; y++) {
                for (x = 0; rows[y][x] && count < 64; x++) {
                    if (rows[y][x] != '#') continue;
                    points[count][0] = x0 + x; points[count][1] = y0 + y; count++;
                }
            }
        }
        if (horn) {
            /* Out and up from the letter's top right, where its own outline
             * was: two pixels in the small fonts, three in the large. */
            for (i = 0; i < (large ? 3 : 2) && count < 64; i++) {
                points[count][0] = letter_right - 1 + (i + 1) / 2;
                points[count][1] = letter_top + 1 - i;
                count++;
            }
        }
        stamp(cell, (const int (*)[2])points, count, fill, outline);
        /* Last, so that the mark goes where it goes over the US letter:
         * the serif on the left would move it. */
        if (serifs) add_serifs(cell, large);
    }
}

/* A character of the built-in letters or from a font, with the retail
 * glyphs' look: their shading by row inside, their dark outline round it. */
static void render(const Added *glyph, int font_page, int large, Cell *cell)
{
    static const char capitals[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int shade[16], y, x, i, baseline = large ? 13 : 10, room = large ? 14 : 6;
    unsigned char mask[16][16];
    FT_Face face = glyph->letter_shape >= 0 ? NULL : face_for(glyph->character);
    memset(cell, 0, sizeof(*cell));
    cell->width = large ? 16 : 8;
    cell->height = large ? 16 : 12;
    /* The shading: per row, the retail capitals' most common inner index. */
    {
        int count[16][16];
        memset(count, 0, sizeof(count));
        for (i = 0; capitals[i]; i++) {
            Cell letter;
            read_cell(font_page, capitals[i], large, &letter);
            for (y = 0; y < letter.height; y++) {
                for (x = 0; x < letter.width; x++) {
                    if (letter.pixels[y][x] >= 2) count[y][letter.pixels[y][x]]++;
                }
            }
        }
        for (y = 0; y < cell->height; y++) {
            int best = 0;
            for (x = 2; x < 16; x++) {
                if (count[y][x] > count[y][best]) best = x;
            }
            shade[y] = best ? best : (y ? shade[y - 1] : 12);
        }
    }
    memset(mask, 0, sizeof(mask));
    if (glyph->letter_shape >= 0) {
        const char *const *rows = large ? letters[glyph->letter_shape].large : letters[glyph->letter_shape].small;
        for (y = 0; y < cell->height; y++) {
            for (x = 0; x < cell->width && rows[y][x]; x++) mask[y][x] = rows[y][x] == '#';
        }
    } else if (!face || FT_Set_Pixel_Sizes(face, 0, large ? 14 : 10) ||
               FT_Load_Char(face, glyph->character, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP)) {
        return;
    } else {
        FT_Bitmap *bitmap = &face->glyph->bitmap;
        int width = (int)bitmap->width, rows = (int)bitmap->rows;
        int top = baseline - face->glyph->bitmap_top;
        int squeezed = width > room ? room : width;
        int left = 1 + (room - squeezed) / 2;
        for (y = 0; y < rows; y++) {
            int cy = top + y;
            if (cy < 1 || cy > cell->height - 2) continue;
            for (x = 0; x < squeezed; x++) {
                int from = width > room ? x * width / room : x;
                unsigned char level = bitmap->buffer[y * bitmap->pitch + from];
                if (level >= 96) mask[cy][left + x] = 1;
            }
        }
    }
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int dx, dy, edge = 0;
            if (mask[y][x]) {
                cell->pixels[y][x] = (unsigned char)shade[y];
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < cell->width && ny >= 0 && ny < cell->height && mask[ny][nx]) edge = 1;
                }
            }
            if (edge) cell->pixels[y][x] = 1;
        }
    }
}

/* Where added glyph `n` is in the bank: small ones on page 0, large ones on
 * pages 1 on, 256 to a page, 8x8 ones on page 4 (with ':' after them). */
static void place(int n, int size, int *page, int *u, int *v)
{
    if (size == FONT_TINY) {
        *page = 4;
        *u = (n % 32) * 8;
        *v = (n / 32) * 8;
    } else if (size == FONT_LARGE) {
        *page = 1 + n / 256;
        *u = (n % 16) * 16;
        *v = ((n % 256) / 16) * 16;
    } else {
        *page = 0;
        *u = (n % 32) * 8;
        *v = (n / 32) * 12;
    }
}

static void store_at(uint16_t *bank, int page, int u, int v, const Cell *cell)
{
    int x, y;
    for (y = 0; y < cell->height; y++) {
        for (x = 0; x < cell->width; x++) {
            int tu = u + x;
            uint16_t *word = &bank[(v + y) * SOFT_GPU_WIDTH + page_x(page) + tu / 4];
            int shift = (tu & 3) * 4;
            *word = (uint16_t)((*word & ~(0xF << shift)) | (cell->pixels[y][x] << shift));
        }
    }
}

static void store(uint16_t *bank, int n, int size, const Cell *cell)
{
    int page, u, v;
    place(n, size, &page, &u, &v);
    store_at(bank, page, u, v, cell);
}

/* The text palettes, where the glyphs' primitives read them: the bank is
 * VRAM-shaped and a primitive sampling it takes its palette from it too. */
static void copy_palettes(uint16_t *bank)
{
    const uint16_t *vram = SoftGpu_VramRaw();
    int y;
    SoftGpu_Settle(640, 0xE8, 16, 0xFF - 0xE8 + 1);
    SoftGpu_Settle(544, 0xFF, 16, 1);
    SoftGpu_Settle(656, 0xFA, 256, 1);
    for (y = 0xE8; y <= 0xFF; y++) {
        memcpy(&bank[y * SOFT_GPU_WIDTH + 640], &vram[y * SOFT_GPU_WIDTH + 640], 16 * sizeof(uint16_t));
    }
    memcpy(&bank[0xFF * SOFT_GPU_WIDTH + 544], &vram[0xFF * SOFT_GPU_WIDTH + 544], 16 * sizeof(uint16_t));
    /* The 8x8 font's, at 0x290 + 16 per color on row 0xFA. */
    memcpy(&bank[0xFA * SOFT_GPU_WIDTH + 656], &vram[0xFA * SOFT_GPU_WIDTH + 656], 256 * sizeof(uint16_t));
}

/* MEMORIES_GLYPH_SHEET=file.ppm: the pictures of every letter the text
 * added (a translation's), made as the game makes them, written once the
 * first is drawn: a column each, the 8x12, 16x16 and 8x8 ones down it; the
 * outline black, the letter in greys, the cell blue. For a translator to
 * see the letters whole, and for the tests (notes/translation.md). */
static void write_sheet(int font_page)
{
    static int written;
    const char *path = getenv("MEMORIES_GLYPH_SHEET");
    FILE *file;
    int n, size, x, y, width = added_count * 17 + 1, height = 12 + 16 + 8 + 4;
    unsigned char *rgb;
    if (written || !path || !*path || !added_count) return;
    written = 1;
    rgb = calloc((size_t)width * height, 3);
    if (!rgb) return;
    for (n = 0; n < added_count; n++) {
        int at_y = 1;
        for (size = 0; size < 3; size++) {
            Cell cell;
            int fill, outline = 1;
            if (size == FONT_TINY && !added[n].letter) continue;
            if (!added[n].letter) render(&added[n], font_page, size, &cell);
            else compose(&added[n], size == FONT_TINY ? TINY_PAGE : font_page, size, &cell);
            if (size == FONT_TINY) tiny_colors(&cell, &fill, &outline);
            for (y = 0; y < cell.height; y++) {
                for (x = 0; x < cell.width; x++) {
                    unsigned char *at = &rgb[((size_t)(at_y + y) * width + 1 + n * 17 + x) * 3];
                    int index = cell.pixels[y][x];
                    at[0] = at[1] = (unsigned char)(!index ? 40 : index == outline ? 0 : 96 + index * 10);
                    at[2] = !index ? 110 : at[0];
                }
            }
            at_y += cell.height + 1;
        }
    }
    file = fopen(path, "wb");
    if (file) {
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        fwrite(rgb, 3, (size_t)width * height, file);
        fclose(file);
        /* Which letter each column is: their code points, one a line. */
        {
            char list[1024];
            snprintf(list, sizeof(list), "%s.txt", path);
            if ((file = fopen(list, "w")) != NULL) {
                for (n = 0; n < added_count; n++) fprintf(file, "%04X\n", (unsigned)added[n].character);
                fclose(file);
            }
        }
        LOG(LOG_MODS, "glyphs: %d letters drawn into %s", added_count, path);
    }
    free(rgb);
}

/* --- the European narrow letters (serif.h) ------------------------------- */

/* The retail i and l with the PAL's serifs, on a page of the bank of their
 * own (place() leaves it alone): i then l across, the small ones at v 0,
 * the large at v 16. Made again when the retail letter they are made from
 * is another (a font loaded over the page). */
#define SERIF_PAGE 5
static const char serif_letters[2] = {'i', 'l'};
static uint32_t serif_sum[2][2];
static unsigned char serif_made[2][2];

void Glyphs_SetEuropean(int on)
{
    european = on != 0;
}

char Glyphs_SerifLetter(uint32_t character)
{
    size_t a;
    if (!european) return 0;
    if (character == 'i' || character == 'l') return (char)character;
    for (a = 0; a < sizeof(accents) / sizeof(accents[0]); a++) {
        if (accents[a].character == character)
            return serifed(accents[a].base, accents[a].mark, accents[a].tone) ? accents[a].base : 0;
    }
    return 0;
}

static void serif_place(int which, int large, int *u, int *v)
{
    *u = which * (large ? 16 : 8);
    *v = large ? 16 : 0;
}

static int serif_cell(uint32_t sjis, int large, int font_page, int *tpage, int *u, int *v)
{
    uint16_t *bank;
    Cell cell;
    uint32_t sum = 2166136261u;
    int which = character_sjis('i') == sjis ? 0 : character_sjis('l') == sjis ? 1 : -1, x, y;
    if (which < 0 || !(bank = SoftGpu_Bank(GLYPHS_BANK))) return 0;
    read_cell(font_page, serif_letters[which], large ? FONT_LARGE : FONT_SMALL, &cell);
    for (y = 0; y < cell.height; y++) {
        for (x = 0; x < cell.width; x++) sum = (sum ^ cell.pixels[y][x]) * 16777619u;
    }
    serif_place(which, large, u, v);
    if (!serif_made[which][large] || serif_sum[which][large] != sum) {
        add_serifs(&cell, large);
        store_at(bank, SERIF_PAGE, *u, *v, &cell);
        serif_made[which][large] = 1;
        serif_sum[which][large] = sum;
    }
    copy_palettes(bank);
    *tpage = SERIF_PAGE | (GLYPHS_BANK << 11);
    return 1;
}

int Glyphs_Cell(uint32_t sjis, int large, int font_page, int *tpage, int *u, int *v)
{
    uint16_t *bank;
    Added *glyph;
    int n = (int)sjis - (int)ADDED_SJIS, page;
    if (n < 0 || n >= added_count) return european ? serif_cell(sjis, large != 0, font_page, tpage, u, v) : 0;
    write_sheet(font_page);
    bank = SoftGpu_Bank(GLYPHS_BANK);
    if (!bank) return 0;
    glyph = &added[n];
    if (!glyph->made) {
        int size;
        for (size = 0; size < 2; size++) {
            Cell cell;
            if (glyph->letter) compose(glyph, font_page, size, &cell);
            else render(glyph, font_page, size, &cell);
            store(bank, n, size, &cell);
        }
        glyph->made = 1;
    }
    copy_palettes(bank);
    place(n, large, &page, u, v);
    *tpage = page | (GLYPHS_BANK << 11);
    return 1;
}

/* ':' in the 8x8 font, which has none: two of its '·', in the letters'
 * colors (the '·' has its own). */
static void tiny_colon(Cell *cell)
{
    Cell dot, letter;
    int top, bottom, left, right, x, y, dot_fill, dot_outline, fill, outline;
    read_cell(TINY_COLON, 0, FONT_TINY, &dot);
    read_cell(0, 'A', FONT_TINY, &letter);
    memset(cell, 0, sizeof(*cell));
    cell->width = 8;
    cell->height = 8;
    if (!ink(&dot, &top, &bottom, &left, &right) || bottom - top > 2 || !ink(&letter, &y, &y, &y, &y)) return;
    tiny_colors(&dot, &dot_fill, &dot_outline);
    tiny_colors(&letter, &fill, &outline);
    for (y = top; y <= bottom; y++) {
        for (x = 0; x < 8; x++) {
            int index = dot.pixels[y][x];
            index = !index ? 0 : index == dot_outline ? outline : fill;
            cell->pixels[1 + y - top][x] = cell->pixels[4 + y - top][x] = (unsigned char)index;
        }
    }
}

int Glyphs_TinyIndex(uint32_t word)
{
    int index = (int)((word >> 20) & 0xFF);
    return !index && (word & 0xFFFF) == 0x8146 ? TINY_COLON : index;
}

int Glyphs_TinyCell(uint32_t sjis, int *tpage, int *u, int *v)
{
    uint16_t *bank;
    int n = (int)sjis - (int)ADDED_SJIS, page;
    if (sjis == 0x8146) {
        n = ADDED_MAX;
    } else if (n < 0 || n >= added_count || !added[n].letter) {
        return 0;
    }
    bank = SoftGpu_Bank(GLYPHS_BANK);
    if (!bank) return 0;
    if (n == ADDED_MAX ? !tiny_colon_made : !added[n].made_tiny) {
        Cell cell;
        if (n == ADDED_MAX) {
            tiny_colon(&cell);
            tiny_colon_made = 1;
        } else {
            compose(&added[n], TINY_PAGE, FONT_TINY, &cell);
            added[n].made_tiny = 1;
        }
        store(bank, n, FONT_TINY, &cell);
    }
    copy_palettes(bank);
    place(n, FONT_TINY, &page, u, v);
    *tpage = page | (GLYPHS_BANK << 11);
    return 1;
}

/* --- for HD text (hd_text.h) ---------------------------------------------- */

uint32_t Glyphs_CellCharacter(int in_bank, int page, int large, int u, int v)
{
    uint32_t character;
    int n, cu, cv;
    if (in_bank && page == SERIF_PAGE) {
        /* serif_place(), backwards. */
        n = u / (large ? 16 : 8);
        return european && n < 2 && u % (large ? 16 : 8) == 0 && v == (large ? 16 : 0) ? (uint32_t)serif_letters[n] : 0;
    }
    if (in_bank) {
        /* place(), backwards. */
        if (large) {
            if (page < 1 || u % 16 || v % 16) return 0;
            n = (page - 1) * 256 + (v / 16) * 16 + u / 16;
        } else {
            if (page != 0 || u % 8 || v % 12) return 0;
            n = (v / 12) * 32 + u / 8;
        }
        return n < added_count && added[n].made ? added[n].character : 0;
    }
    /* The retail letters, digits and punctuation a font sets as they are. */
    for (character = '!'; character <= '~'; character++) {
        if (retail_cell(character_sjis(character), large, &cu, &cv) && cu == u && cv == v) return character;
    }
    /* The large font's arrows, where ASCII has { and | (0x827B and 0x827C,
     * after z): the name entry's left and right. */
    if (large && v == 0x78 && (u == 0xB0 || u == 0xC0)) return u == 0xB0 ? 0x2190 : 0x2192;
    return 0;
}

const char *Glyphs_CellWord(int in_bank, int page, int large, int u, int v, int *first_u, int *cells)
{
    (void)page;
    /* The large font's END, drawn across the two cells where ASCII has ~
     * and DEL (0x827E and 0x827F): the name entry's. */
    if (in_bank || !large || v != 0x78 || (u != 0xE0 && u != 0xF0)) return NULL;
    *first_u = 0xE0;
    *cells = 2;
    return "END";
}

int Glyphs_RetailCell(uint32_t character, int large, int *u, int *v)
{
    return retail_cell(character_sjis(character), large, u, v);
}

void *Glyphs_Face(uint32_t character)
{
    return face_for(character);
}
