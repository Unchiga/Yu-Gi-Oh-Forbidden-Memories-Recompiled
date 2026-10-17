/* The control channel's text protocol (control_protocol.h). */
#include "control_protocol.h"
#include <stdio.h>
#include <string.h>

#define RAM_SIZE 0x200000u
#define SCRATCHPAD_BASE 0x1F800000u
#define SCRATCHPAD_SIZE 0x400u

static int fail(char *error, size_t size, const char *message)
{
    if (size) snprintf(error, size, "%s", message);
    return -1;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The next word of the line, NUL-terminated in place; NULL at the end. */
static char *word(char **cursor)
{
    char *start = *cursor, *end;
    while (*start == ' ' || *start == '\t') start++;
    if (!*start) {
        *cursor = start;
        return NULL;
    }
    for (end = start; *end && *end != ' ' && *end != '\t'; end++) {}
    if (*end) *end++ = '\0';
    *cursor = end;
    return start;
}

/* The rest of the line, without the blanks around it. */
static char *rest(char **cursor)
{
    char *start = *cursor, *end;
    while (*start == ' ' || *start == '\t') start++;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    *cursor = end;
    return start;
}

/* A whole 32-bit number: hex (`hex` set, or a 0x prefix) or decimal. */
static int number(const char *text, int hex, uint32_t *out)
{
    uint64_t value = 0;
    int base = hex ? 16 : 10;
    if (!text || !*text) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
        if (!*text) return -1;
    }
    for (; *text; text++) {
        int digit = hex_digit(*text);
        if (digit < 0 || digit >= base) return -1;
        value = value * (unsigned)base + (unsigned)digit;
        if (value > 0xFFFFFFFFu) return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

/* peek's and poke's address: hex, or @NAME[+OFF], a game variable by its name
 * (OFF hex); the game finds where NAME is in this build. */
static int address_argument(char *text, ControlCommand *command)
{
    char *plus;
    if (!text || text[0] != '@') return number(text, 1, &command->address);
    command->symbol = text + 1;
    plus = strchr(command->symbol, '+');
    if (plus) {
        *plus = '\0';
        if (number(plus + 1, 1, &command->offset)) return -1;
    }
    return *command->symbol ? 0 : -1;
}

static int at_end(char **cursor)
{
    return word(cursor) == NULL;
}

int ControlProtocol_Parse(char *line, ControlCommand *command, uint8_t *data, char *error, size_t error_size)
{
    char *cursor = line, *name, *argument;
    size_t length = strlen(line);
    uint32_t value;
    if (length && line[length - 1] == '\r') line[length - 1] = '\0';
    memset(command, 0, sizeof(*command));
    name = word(&cursor);
    if (!name) return fail(error, error_size, "empty command");
    if (!strcmp(name, "step")) {
        command->kind = CONTROL_STEP;
        if (number(word(&cursor), 0, &command->count) || !at_end(&cursor))
            return fail(error, error_size, "usage: step N");
    } else if (!strcmp(name, "pad")) {
        command->kind = CONTROL_PAD;
        if (number(word(&cursor), 0, &value) || (value != 1 && value != 2))
            return fail(error, error_size, "usage: pad P BITS (P is 1 or 2)");
        command->pad = (int)value - 1;
        if (number(word(&cursor), 1, &value) || value > 0xFFFFu || !at_end(&cursor))
            return fail(error, error_size, "usage: pad P BITS (BITS is hex, up to ffff)");
        command->bits = (uint16_t)value;
    } else if (!strcmp(name, "shot") || !strcmp(name, "save") || !strcmp(name, "load")) {
        command->kind = name[0] == 's' ? (name[1] == 'h' ? CONTROL_SHOT : CONTROL_SAVE) : CONTROL_LOAD;
        command->path = rest(&cursor);
        if (!*command->path) {
            snprintf(error, error_size, "usage: %s PATH", name);
            return -1;
        }
    } else if (!strcmp(name, "hash") || !strcmp(name, "info") || !strcmp(name, "quit")) {
        command->kind = name[0] == 'h' ? CONTROL_HASH : name[0] == 'i' ? CONTROL_INFO : CONTROL_QUIT;
        if (!at_end(&cursor)) {
            snprintf(error, error_size, "usage: %s", name);
            return -1;
        }
    } else if (!strcmp(name, "jump")) {
        command->kind = CONTROL_JUMP;
        command->path = word(&cursor);
        argument = word(&cursor);
        if (!command->path) return fail(error, error_size, "usage: jump TARGET [OPPONENT [DECK]]");
        if (argument) {
            if (number(argument, 0, &value) || value > 255)
                return fail(error, error_size, "usage: jump TARGET [OPPONENT [DECK]] (OPPONENT decimal)");
            command->opponent = (int)value;
            command->deck = word(&cursor);
            if (!at_end(&cursor)) return fail(error, error_size, "usage: jump TARGET [OPPONENT [DECK]]");
        }
    } else if (!strcmp(name, "peek")) {
        command->kind = CONTROL_PEEK;
        if (address_argument(word(&cursor), command) || number(word(&cursor), 0, &command->length) ||
            !at_end(&cursor))
            return fail(error, error_size, "usage: peek ADDR LEN (ADDR hex or @NAME[+OFF], LEN decimal)");
        if (command->length > CONTROL_DATA_MAX) return fail(error, error_size, "peek: at most 16384 bytes at once");
    } else if (!strcmp(name, "poke")) {
        size_t digits, i;
        command->kind = CONTROL_POKE;
        if (address_argument(word(&cursor), command))
            return fail(error, error_size, "usage: poke ADDR HEX (ADDR hex or @NAME[+OFF])");
        argument = word(&cursor);
        if (!argument || !at_end(&cursor)) return fail(error, error_size, "usage: poke ADDR HEX");
        digits = strlen(argument);
        if (digits % 2) return fail(error, error_size, "poke: HEX needs two digits a byte");
        if (digits / 2 > CONTROL_DATA_MAX) return fail(error, error_size, "poke: at most 16384 bytes at once");
        for (i = 0; i < digits; i += 2) {
            int high = hex_digit(argument[i]), low = hex_digit(argument[i + 1]);
            if (high < 0 || low < 0) return fail(error, error_size, "poke: HEX is not hex");
            data[i / 2] = (uint8_t)(high << 4 | low);
        }
        command->length = (uint32_t)(digits / 2);
    } else {
        snprintf(error, error_size, "unknown command '%.32s'", name);
        return -1;
    }
    return 0;
}

int ControlProtocol_GuestRange(uint32_t address, uint32_t length, int *scratchpad, uint32_t *offset)
{
    uint64_t physical, end;
    if (address < 0x20000000u) physical = address;                /* physical (KUSEG) */
    else if (address >= 0x80000000u && address < 0xC0000000u) physical = address & 0x1FFFFFFFu; /* KSEG0, KSEG1 */
    else return -1;
    end = physical + length;
    if (end <= RAM_SIZE) {
        *scratchpad = 0;
        *offset = (uint32_t)physical;
        return 0;
    }
    if (physical >= SCRATCHPAD_BASE && end <= SCRATCHPAD_BASE + SCRATCHPAD_SIZE) {
        *scratchpad = 1;
        *offset = (uint32_t)(physical - SCRATCHPAD_BASE);
        return 0;
    }
    return -1;
}

void ControlProtocol_Hex(const uint8_t *bytes, size_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < size; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * size] = '\0';
}

char *ControlLines_Room(ControlLines *lines, size_t *room)
{
    *room = sizeof(lines->data) - lines->used;
    return lines->data + lines->used;
}

void ControlLines_Added(ControlLines *lines, size_t count)
{
    lines->used += count;
}

int ControlLines_Next(ControlLines *lines, char *out)
{
    for (;;) {
        char *newline = memchr(lines->data, '\n', lines->used);
        size_t length;
        if (!newline) {
            if (lines->used < sizeof(lines->data)) return 0;
            /* Full and no end in sight: the line is too long. */
            lines->used = 0;
            if (lines->dropping) return 0;
            lines->dropping = 1;
            return -1;
        }
        length = (size_t)(newline - lines->data);
        if (lines->dropping) {
            lines->dropping = 0;
            memmove(lines->data, newline + 1, lines->used - length - 1);
            lines->used -= length + 1;
            continue; /* the end of the line already reported */
        }
        if (length > CONTROL_LINE_MAX) {
            memmove(lines->data, newline + 1, lines->used - length - 1);
            lines->used -= length + 1;
            return -1;
        }
        memcpy(out, lines->data, length);
        out[length] = '\0';
        memmove(lines->data, newline + 1, lines->used - length - 1);
        lines->used -= length + 1;
        return 1;
    }
}
