#ifndef MEMORIES_PC_LISTING_H
#define MEMORIES_PC_LISTING_H
/* A text listing (notes/translation.md, tools/pc/text_listing.py) compiled
 * into the bytes the game's text boxes read.
 *
 * The strings are laid out one after another in one buffer. Their jumps
 * cannot be the retail 16-bit offsets into a 64 KB bank, since the buffer is
 * neither where the bank was nor bound to its size: a jump's operand is an
 * index into the unit's own targets instead, which Text_Retarget follows
 * for any stream inside the buffer (translation.c). A target the listing
 * does not define is the retail address it names, so a partial translation
 * still reaches the game's own text, and the player's name, which the game
 * writes into its bank, is reached where it is. */
#include <stddef.h>
#include <stdint.h>

enum { TEXT_BANK_DIALOG, TEXT_BANK_DESCRIPTIONS, TEXT_BANK_NAMES, TEXT_BANK_COUNT };

typedef struct {
    uint16_t id;
    uint32_t offset;       /* into data */
} TextString;

typedef struct {
    uint16_t bank;
    uint16_t name;         /* LXXXX: XXXX */
    uint32_t offset;       /* into data */
} TextLabel;

#define TEXT_NO_CALL 0xFFFFFFFFu

typedef struct {
    unsigned char *data;
    size_t size;
    unsigned char **targets;   /* by operand */
    int target_count;
    /* Where each target is jumped from, by operand: the offset into data of
     * the {call} or {jump} that has it, else TEXT_NO_CALL (a choice's, a
     * flag's). With the offsets of the unit's control codes (F6-FF, which a
     * letter's second byte can look like), ascending, it lets the port
     * find what comes before a call without reading letters as codes. */
    uint32_t *target_calls;
    uint32_t *controls;
    int control_count;
    TextString *strings;
    int string_count;
    TextLabel *labels;         /* for the listings compiled after this one */
    int label_count;
    int not_utf8_lines, first_not_utf8_line;   /* left out, for the mod's status */
    /* The span of data the result screens' strings (0x40-0x45) take, up to
     * the next string; 0, 0 when the unit has none. Set by translation.c. */
    size_t results_from, results_to;
} TextUnit;

/* The glyph code for a character (-1 for none), and what to say about a
 * line of the listing. */
typedef int (*TextGlyphEncoder)(uint32_t character);
typedef void (*TextReport)(void *context, int line, const char *message);

/* A label the listing jumps to but does not define is looked for in the
 * `earlier` units (the files and mods read before it; the latest that
 * defines it wins), and failing that is the retail text at that offset of
 * its bank: `bases` are the retail banks' addresses (the dialogue,
 * descriptions and names banks). NULL if nothing could be compiled;
 * problems with single lines are reported and skipped. */
TextUnit *TextListing_Compile(const char *text, size_t length, const uint32_t bases[TEXT_BANK_COUNT],
                              TextUnit *const *earlier, int earlier_count,
                              TextGlyphEncoder encode, TextReport report, void *context);

/* The most letters one page of a text box has room for: its channel's slice
 * of the game's glyph entries (255, 160, 160 and 45 of them; with a PAL
 * language 280, 220, 220 and 80, pc/text/entry_layout.h), less the one
 * that ends the list. Which channel a string is shown on is the game's
 * choice, not the string's: the dialogue box and a few menus have the 255,
 * most menus 160 (and a retail menu string has 224 letters), so the
 * compiler can only warn past the largest. The game leaves out what does
 * not fit (DuelEffect_AppendEntry). Those of the layout this launch has
 * (TextEntries_PageLetters: 254 and 159, or 279 and 219). */
#define TEXT_PAGE_LETTERS (TextEntries_PageLetters(0))
#define TEXT_MENU_LETTERS (TextEntries_PageLetters(1))
void TextListing_Free(TextUnit *unit);

/* The bank a string id belongs in, or -1 for an id no bank has. */
int TextListing_Bank(unsigned id);

#endif
