/* The control channel's parser and framing (src/pc/debug/control_protocol.h). */
#include "pc/debug/control_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1); \
        } \
    } while (0)

static uint8_t data[CONTROL_DATA_MAX];
static char error[128];

static int parse(const char *text, ControlCommand *command)
{
    static char line[CONTROL_LINE_MAX + 2];
    snprintf(line, sizeof(line), "%s", text);
    return ControlProtocol_Parse(line, command, data, error, sizeof(error));
}

static void commands(void)
{
    ControlCommand c;
    static char big[2 * CONTROL_DATA_MAX + 64];
    size_t i;
    CHECK(parse("step 6", &c) == 0 && c.kind == CONTROL_STEP && c.count == 6);
    CHECK(parse("  step   0x10  \r", &c) == 0 && c.count == 16);
    CHECK(parse("step", &c) == -1 && strstr(error, "usage: step"));
    CHECK(parse("step 6 7", &c) == -1);
    CHECK(parse("step -1", &c) == -1);
    CHECK(parse("step 4294967296", &c) == -1);
    CHECK(parse("pad 1 4000", &c) == 0 && c.kind == CONTROL_PAD && c.pad == 0 && c.bits == 0x4000);
    CHECK(parse("pad 2 0xffff", &c) == 0 && c.pad == 1 && c.bits == 0xffff);
    CHECK(parse("pad 3 0", &c) == -1 && parse("pad 0 0", &c) == -1);
    CHECK(parse("pad 1 10000", &c) == -1 && parse("pad 1 xyz", &c) == -1 && parse("pad 1", &c) == -1);
    CHECK(parse("shot D:/some dir/a b.png ", &c) == 0 && c.kind == CONTROL_SHOT && !strcmp(c.path, "D:/some dir/a b.png"));
    CHECK(parse("save /tmp/x.state", &c) == 0 && c.kind == CONTROL_SAVE && !strcmp(c.path, "/tmp/x.state"));
    CHECK(parse("load x.state\r", &c) == 0 && c.kind == CONTROL_LOAD && !strcmp(c.path, "x.state"));
    CHECK(parse("load   ", &c) == -1 && strstr(error, "usage: load"));
    CHECK(parse("hash", &c) == 0 && c.kind == CONTROL_HASH);
    CHECK(parse("info", &c) == 0 && c.kind == CONTROL_INFO);
    CHECK(parse("quit", &c) == 0 && c.kind == CONTROL_QUIT);
    CHECK(parse("info now", &c) == -1);
    CHECK(parse("peek 801D0200 80", &c) == 0 && c.kind == CONTROL_PEEK && c.address == 0x801D0200 && c.length == 80);
    CHECK(parse("peek 0x8009b26c 0x1", &c) == 0 && c.address == 0x8009B26C && c.length == 1);
    CHECK(parse("peek 80000000 16385", &c) == -1 && parse("peek 80000000", &c) == -1);
    CHECK(parse("peek 1800000000 1", &c) == -1);
    CHECK(parse("poke 801D07E0 e8030000", &c) == 0 && c.kind == CONTROL_POKE && c.address == 0x801D07E0 &&
          c.length == 4 && data[0] == 0xe8 && data[1] == 0x03 && data[2] == 0 && data[3] == 0);
    CHECK(parse("peek @gGraphics_sViewportX 2", &c) == 0 && c.kind == CONTROL_PEEK && c.length == 2 &&
          !strcmp(c.symbol, "gGraphics_sViewportX") && c.offset == 0);
    CHECK(parse("peek @D_8009B066+1a 4", &c) == 0 && !strcmp(c.symbol, "D_8009B066") && c.offset == 0x1a);
    CHECK(parse("peek @name#2+0x10 1", &c) == 0 && !strcmp(c.symbol, "name#2") && c.offset == 0x10);
    CHECK(parse("peek 801D0200 80", &c) == 0 && c.symbol == NULL && c.offset == 0);
    CHECK(parse("poke @gGraphics_sViewportX+0 2800", &c) == 0 && c.kind == CONTROL_POKE && c.length == 2 &&
          !strcmp(c.symbol, "gGraphics_sViewportX") && data[0] == 0x28 && data[1] == 0);
    CHECK(parse("peek @ 1", &c) == -1 && parse("peek @+4 1", &c) == -1 && parse("peek @name+zz 1", &c) == -1);
    CHECK(parse("poke @name+ 00", &c) == -1 && parse("poke @name", &c) == -1 && parse("peek @name", &c) == -1);
    CHECK(parse("poke 801D07E0 E8F", &c) == -1 && parse("poke 801D07E0 zz", &c) == -1);
    CHECK(parse("poke 801D07E0", &c) == -1 && parse("poke 801D07E0 00 00", &c) == -1);
    strcpy(big, "poke 80000000 ");
    i = strlen(big);
    memset(big + i, 'a', 2 * CONTROL_DATA_MAX);
    big[i + 2 * CONTROL_DATA_MAX] = '\0';
    CHECK(parse(big, &c) == 0 && c.length == CONTROL_DATA_MAX && data[CONTROL_DATA_MAX - 1] == 0xaa);
    strcat(big, "aa");
    CHECK(parse(big, &c) == -1);
    CHECK(parse("", &c) == -1 && parse("   ", &c) == -1);
    CHECK(parse("goto 3", &c) == -1 && strstr(error, "unknown command 'goto'"));
    CHECK(parse("jump library", &c) == 0 && c.kind == CONTROL_JUMP && !strcmp(c.path, "library") && !c.deck);
    CHECK(parse("jump duel 3 723-762", &c) == 0 && c.opponent == 3 && !strcmp(c.deck, "723-762"));
    CHECK(parse("jump duel 3", &c) == 0 && c.opponent == 3 && !c.deck);
    CHECK(parse("jump", &c) == -1 && parse("jump duel x", &c) == -1 && parse("jump duel 256", &c) == -1);
    CHECK(parse("jump duel 3 1-40 extra", &c) == -1);
}

static void ranges(void)
{
    int scratchpad = -1;
    uint32_t offset = 0;
    CHECK(ControlProtocol_GuestRange(0x801D0200, 0x50, &scratchpad, &offset) == 0 && !scratchpad && offset == 0x1D0200);
    CHECK(ControlProtocol_GuestRange(0xA01D0200, 4, &scratchpad, &offset) == 0 && !scratchpad && offset == 0x1D0200);
    CHECK(ControlProtocol_GuestRange(0x001D0200, 4, &scratchpad, &offset) == 0 && offset == 0x1D0200);
    CHECK(ControlProtocol_GuestRange(0x801FFFFC, 4, &scratchpad, &offset) == 0 && offset == 0x1FFFFC);
    CHECK(ControlProtocol_GuestRange(0x801FFFFD, 4, &scratchpad, &offset) == -1); /* crosses the end */
    CHECK(ControlProtocol_GuestRange(0x80200000, 1, &scratchpad, &offset) == -1);  /* a mirror the host lacks */
    CHECK(ControlProtocol_GuestRange(0x1F800010, 16, &scratchpad, &offset) == 0 && scratchpad == 1 && offset == 0x10);
    CHECK(ControlProtocol_GuestRange(0x9F8003FC, 4, &scratchpad, &offset) == 0 && scratchpad == 1 && offset == 0x3FC);
    CHECK(ControlProtocol_GuestRange(0x1F8003FC, 8, &scratchpad, &offset) == -1);
    CHECK(ControlProtocol_GuestRange(0xC0000000, 1, &scratchpad, &offset) == -1);
    CHECK(ControlProtocol_GuestRange(0x40000000, 1, &scratchpad, &offset) == -1);
    CHECK(ControlProtocol_GuestRange(0xBFFFFFFF, 0xFFFFFFFF, &scratchpad, &offset) == -1); /* no wrap */
}

static void hex(void)
{
    const uint8_t bytes[] = {0x00, 0x7f, 0xa5, 0xff};
    char out[9];
    ControlProtocol_Hex(bytes, sizeof(bytes), out);
    CHECK(!strcmp(out, "007fa5ff"));
    ControlProtocol_Hex(bytes, 0, out);
    CHECK(!strcmp(out, ""));
}

static void feed(ControlLines *lines, const char *text)
{
    size_t room, length = strlen(text);
    char *at = ControlLines_Room(lines, &room);
    CHECK(length <= room);
    memcpy(at, text, length);
    ControlLines_Added(lines, length);
}

static void framing(void)
{
    static ControlLines lines;
    static char out[CONTROL_LINE_MAX + 1];
    size_t room, i;
    feed(&lines, "step 1\ninf");
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, "step 1"));
    CHECK(ControlLines_Next(&lines, out) == 0);
    feed(&lines, "o\r\n\nhash\n");
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, "info\r"));
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, ""));
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, "hash"));
    CHECK(ControlLines_Next(&lines, out) == 0 && lines.used == 0);
    /* A line too long is dropped, once, and the next one still comes through. */
    for (i = 0; i < 3; i++) {
        char *at = ControlLines_Room(&lines, &room);
        memset(at, 'x', room);
        ControlLines_Added(&lines, room);
        CHECK(ControlLines_Next(&lines, out) == (i == 0 ? -1 : 0));
    }
    feed(&lines, "xxx\nquit\n");
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, "quit"));
    CHECK(ControlLines_Next(&lines, out) == 0);
    /* One that fits the buffer but not the limit, with its newline in. */
    {
        char *at = ControlLines_Room(&lines, &room);
        memset(at, 'y', CONTROL_LINE_MAX + 1);
        at[CONTROL_LINE_MAX + 1] = '\n';
        ControlLines_Added(&lines, CONTROL_LINE_MAX + 2);
    }
    CHECK(ControlLines_Next(&lines, out) == -1 && lines.used == 0);
    feed(&lines, "info\n");
    CHECK(ControlLines_Next(&lines, out) == 1 && !strcmp(out, "info"));
}

int main(void)
{
    commands();
    ranges();
    hex();
    framing();
    puts("control protocol: all passed");
    return 0;
}
