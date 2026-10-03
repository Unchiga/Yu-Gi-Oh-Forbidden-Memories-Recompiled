/* The control channel (control.h). */
#include "pc/compat/fs.h"
#include "control.h"
#include "control_net.h"
#include "control_protocol.h"
#include "recorder.h"
#include "pc/guest/image.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/platform/title_jump.h"
#include "pc/platform/menu.h"
#include "pc/platform/settings.h"
#include "pc/sdk/display.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCHPAD 0x1F800000u
#define MODE_BYTE 0x8009B26Cu /* D_8009B26C, main_mode_state.h: the active mode and its flags */
#define WAIT_MS 10            /* how long a blocked wait sleeps before it pumps the window again */

static enum { UNREAD, OFF, ON } channel;
static int attached;      /* a client is connected */
static int running;       /* a `step` is being run */
static unsigned target;   /* the VBlank count it runs to */
static int owed;          /* the "ok" of a `step` or a `load`, sent at the next stop */
static int send_failed;
static volatile uint16_t pads[2];
static volatile int pad2_set;
static ControlLines lines;
static char line[CONTROL_LINE_MAX + 1];
static uint8_t data[CONTROL_DATA_MAX];
static char reply[2 * CONTROL_DATA_MAX + 64];

static int send_idle(void)
{
    Platform_ControlIdle();
    Platform_PumpEvents();
    return !Platform_ShouldQuit();
}

uint16_t Control_Pad(int port) { return pads[port & 1]; }
int Control_PadConnected(int port) { return port == 1 && pad2_set; }

/* One reply line, in one piece (`text` is `reply` or a short literal). */
static void send_line(const char *text)
{
    size_t length = strlen(text);
    if (text != reply) memcpy(reply, text, length);
    reply[length] = '\n';
    if (ControlNet_Send(reply, length + 1, send_idle)) {
        send_failed = 1;
        ControlNet_Drop(); /* next_line discards pending commands and detaches */
    }
}

static void send_error(const char *message)
{
    snprintf(reply, sizeof(reply), "err %s", message);
    send_line(reply);
}

/* "ok frame F vblank V": where the game has stopped. */
static void send_stopped(void)
{
    snprintf(reply, sizeof(reply), "ok frame %u vblank %u", Memories_PresentedFrames(), Platform_VBlankCount());
    send_line(reply);
}

static void attach(void)
{
    attached = 1;
    running = owed = 0;
    Platform_ControlAttach(1); /* the watchdog is off from now on, not before a client exists */
    fprintf(stderr, "memories-pc: control: a client is attached at frame %u\n", Memories_PresentedFrames());
}

static void detach(const char *why)
{
    ControlNet_Drop();
    attached = running = owed = 0;
    send_failed = pad2_set = 0;
    lines.used = lines.dropping = 0;
    pads[0] = pads[1] = 0; /* nothing held by a client that has gone */
    Platform_ControlAttach(0);
    fprintf(stderr, "memories-pc: control: %s at frame %u\n", why, Memories_PresentedFrames());
}

/* The window's notice while the game waits for its first client: "Play\n * now" ends the wait. */
static int play_now;
static void wait_answered(int button, int *quit)
{
    (void)button;
    (void)quit;
    play_now = 1;
}

/* The first client, for MEMORIES_CONTROL_WAIT seconds (10 unset, a negative
 * number for as long as it takes, 0 not at all): a run is the client's from
 * its first frame. Past the time the game goes on by itself, as when a
 * client has left, and a client may attach later. In a window the wait says
 * so in a notice, which (or closing the window) ends it: the window is
 * given a still of the display area to draw it over (Memories_ShowStill),
 * and the clock stands paused meanwhile, so that the backend's pumps
 * repaint the menu and the notice with no game frame. */
static void wait_first(unsigned port)
{
    static const char *const buttons[] = {"Play now"};
    const char *setting = getenv("MEMORIES_CONTROL_WAIT");
    long seconds = setting && *setting ? strtol(setting, NULL, 10) : 10;
    long waited = 0;
    int rate = Platform_ClockRate(), confirm = Settings_Get(SET_CONFIRM_QUIT);
    char text[200];
    if (!seconds) return;
    if (seconds > 0) {
        snprintf(text, sizeof(text), "Waiting %ld s for a control client on 127.0.0.1:%u (MEMORIES_CONTROL is set).",
                 seconds, port);
    } else {
        snprintf(text, sizeof(text), "Waiting for a control client on 127.0.0.1:%u (MEMORIES_CONTROL is set).", port);
    }
    fprintf(stderr, "memories-pc: control: %s\n", text);
    play_now = 0;
    Menu_ShowNotice("Control channel", text, buttons, 1, 0, wait_answered);
    Platform_SetClockRate(0);
    Platform_ControlHold(1);
    /* Nothing is played yet: closing the window (or Esc) quits at once. */
    Settings_Set(SET_CONFIRM_QUIT, 0);
    for (;;) {
        if (ControlNet_Accept(WAIT_MS)) {
            attach();
            break;
        }
        /* Before its first frame the window has no picture to draw the
         * notice over: it gets the display area as it stands (black at
         * boot), twice a second (once was too early, before the window was
         * up), and the paused clock's pumps repaint the menu and notice. */
        if (waited % 500 == 0) Memories_ShowStill();
        waited += WAIT_MS;
        Platform_ControlIdle();
        Platform_PumpEvents();
        if (Platform_ShouldQuit()) break;
        if (play_now || (seconds > 0 && waited >= seconds * 1000)) {
            fprintf(stderr, "memories-pc: control: no client %s; the game goes on, and a client may attach "
                            "later\n", play_now ? "(Play now)" : "in time");
            break;
        }
    }
    if (!play_now && Menu_NoticeShown()) Menu_CloseNotice();
    Settings_Set(SET_CONFIRM_QUIT, confirm);
    Platform_SetClockRate(rate);
    Platform_ControlHold(0);
}

static void start(void)
{
    const char *port = getenv("MEMORIES_CONTROL");
    unsigned bound;
    char *end;
    unsigned long number;
    channel = OFF;
    if (!port || !*port) return;
    number = strtoul(port, &end, 10);
    if (*end || number > 65535) {
        fprintf(stderr, "memories-pc: control: MEMORIES_CONTROL=%s is not a port number\n", port);
        return;
    }
    if (ControlNet_Listen((unsigned)number, &bound)) return;
    channel = ON;
    fprintf(stderr, "memories-pc: control: listening on 127.0.0.1:%u\n", bound);
    wait_first(bound);
}

/* The guest bytes [address, address + length) in this process, or NULL. */
static uint8_t *guest(uint32_t address, uint32_t length, int *ram)
{
    int scratchpad;
    uint32_t offset;
    if (ControlProtocol_GuestRange(address, length, &scratchpad, &offset)) return NULL;
    *ram = !scratchpad;
    return (uint8_t *)(uintptr_t)((scratchpad ? SCRATCHPAD : MEMORIES_GUEST_RAM) + offset);
}

/* The presented picture: Memories_DumpFrame's PPM, as PNG unless the path
 * asks for PPM (its own .ppm.partial first, beside the PNG). */
static int shot(const char *path)
{
    char ppm[1100];
    size_t length = strlen(path);
    FILE *file;
    int width, height, ok = 0;
    if (length >= 4 && (!strcmp(path + length - 4, ".ppm") || !strcmp(path + length - 4, ".PPM"))) {
        remove(path);
        Memories_DumpFrame(path, 0);
        file = fopen(path, "rb");
        if (file) fclose(file);
        return file ? 0 : -1;
    }
    if ((size_t)snprintf(ppm, sizeof(ppm), "%s.ppm.partial", path) >= sizeof(ppm)) return -1;
    Memories_DumpFrame(ppm, 0);
    file = fopen(ppm, "rb");
    if (!file) return -1;
    if (fscanf(file, "P6 %d %d 255", &width, &height) == 2 && fgetc(file) != EOF && width > 0 && height > 0 &&
        width <= 16384 && height <= 16384) {
        size_t size = (size_t)width * (size_t)height * 3;
        unsigned char *pixels = malloc(size);
        if (pixels && fread(pixels, 1, size, file) == size) {
            png_image image;
            FILE *out = fopen(path, "wb");
            memset(&image, 0, sizeof(image));
            image.version = PNG_IMAGE_VERSION;
            image.width = (png_uint_32)width;
            image.height = (png_uint_32)height;
            image.format = PNG_FORMAT_RGB;
            ok = out && png_image_write_to_stdio(&image, out, 0, pixels, 0, NULL);
            if (out && fclose(out)) ok = 0;
        }
        free(pixels);
    }
    fclose(file);
    remove(ppm);
    return ok ? 0 : -1;
}

/* The poke's bytes into guest memory; -1 outside it. */
static int poke(const ControlCommand *command)
{
    uint8_t *at;
    int ram;
    if (!(at = guest(command->address, command->length, &ram))) return -1;
    memcpy(at, data, command->length);
    if (ram && command->length) Memories_GuestWritten(at, command->length); /* a module's identifier, perhaps */
    return 0;
}

/* The jump asked for, checked as Debug > Jump to checks it: -1 with why. */
static int jump(const ControlCommand *command, char *error, size_t size)
{
    int target = TitleJump_TargetByName(command->path);
    if (target < 0) {
        snprintf(error, size, "no such screen (title, debug, duel, free_duel, build_deck, library, password, map, "
                              "credits, options)");
        return -1;
    }
    return TitleJump_RequestTo(target, command->opponent, command->deck, error, size) ? -1 : 0;
}

int Control_Apply(char *text)
{
    ControlCommand command;
    char error[160];
    if (ControlProtocol_Parse(text, &command, data, error, sizeof(error))) return -1;
    if (command.kind == CONTROL_POKE) return poke(&command);
    if (command.kind == CONTROL_JUMP) return jump(&command, error, sizeof(error));
    return -1;
}

/* One command. 1 when the game is to run (step, quit), 0 to read the next. */
static int handle(char *text)
{
    ControlCommand command;
    char error[160];
    /* A poke or a jump changes what the game does next, and the pads alone
     * would not bring it back: a recording keeps the line (recorder.h). The
     * parser cuts the line up, so it is copied first. */
    static char kept[CONTROL_LINE_MAX + 1];
    uint8_t *at;
    int ram;
    snprintf(kept, sizeof(kept), "%s", text);
    if (ControlProtocol_Parse(text, &command, data, error, sizeof(error))) {
        send_error(error);
        return 0;
    }
    switch (command.kind) {
    case CONTROL_STEP:
        if (!command.count) {
            send_stopped();
            return 0;
        }
        target = Platform_VBlankCount() + command.count;
        running = owed = 1;
        return 1;
    case CONTROL_PAD:
        pads[command.pad] = command.bits;
        if (command.pad == 1) pad2_set = 1;
        send_line("ok");
        return 0;
    case CONTROL_SHOT:
        if (shot(command.path)) {
            snprintf(error, sizeof(error), "could not write %.120s", command.path);
            send_error(error);
        } else {
            send_line("ok");
        }
        return 0;
    case CONTROL_HASH:
        snprintf(reply, sizeof(reply), "ok %016llx", Memories_VramHash());
        send_line(reply);
        return 0;
    case CONTROL_PEEK:
        if (!(at = guest(command.address, command.length, &ram))) {
            send_error("outside guest RAM and the scratchpad");
            return 0;
        }
        memcpy(reply, "ok ", 3);
        ControlProtocol_Hex(at, command.length, reply + 3);
        send_line(reply);
        return 0;
    case CONTROL_POKE:
        if (poke(&command)) {
            send_error("outside guest RAM and the scratchpad");
            return 0;
        }
        Recorder_Command(kept);
        send_line("ok");
        return 0;
    case CONTROL_SAVE:
        switch (Memories_StateSaveHere(command.path)) {
        case 0: send_line("ok"); break;
        case -2: send_error("not at a state point (this VSync was called from native code); step and try again"); break;
        default: send_error("the state was not saved (the reason is in the log)"); break;
        }
        return 0;
    case CONTROL_LOAD:
        /* Does not return when the state loads: the game resumes in the
         * state's VSync caller and the "ok" goes out at the next point. */
        owed = 1;
        Platform_ControlHold(0);
        if (Memories_StateLoadHere(command.path, error, sizeof(error)) == -2) {
            snprintf(error, sizeof(error), "not at a state point (this VSync was called from native code); step and try again");
        }
        Platform_ControlHold(1);
        owed = 0;
        send_error(error);
        return 0;
    case CONTROL_INFO:
        snprintf(reply, sizeof(reply), "ok frame %u vblank %u mode %u build %08x jumps %u clock %s",
                 Memories_PresentedFrames(), Platform_VBlankCount(), (unsigned)*guest(MODE_BYTE, 1, &ram),
                 (unsigned)Memories_StateBuildId(), TitleJump_Count(), Platform_VirtualClock() ? "virtual" : "real");
        send_line(reply);
        return 0;
    case CONTROL_JUMP:
        /* Taken now, done by the game at its next point between two
         * screens' frames: the client waits for the mode it wants. */
        if (jump(&command, error, sizeof(error))) {
            send_error(error);
        } else {
            Recorder_Command(kept);
            send_line("ok");
        }
        return 0;
    case CONTROL_QUIT:
        send_line("ok");
        detach("the client asked to quit");
        Platform_RequestQuit(); /* VSync's own way out, right after this */
        return 1;
    }
    return 0;
}

/* The next line from the client: 1 a line, 0 when the game is closing, -1
 * when the client has gone. Waits, keeping the window alive. */
static int next_line(void)
{
    for (;;) {
        size_t room;
        char *at;
        long count;
        int got;
        if (send_failed) return -1;
        got = ControlLines_Next(&lines, line);
        if (got > 0) return 1;
        if (got < 0) send_error("line too long");
        at = ControlLines_Room(&lines, &room);
        count = ControlNet_Receive(at, room, WAIT_MS);
        if (count < 0) return -1;
        if (count > 0) {
            ControlLines_Added(&lines, (size_t)count);
            continue;
        }
        Platform_PumpEvents();
        ControlNet_RefuseOthers("err busy: another client is attached\n");
        if (Platform_ShouldQuit()) return 0;
    }
}

void Control_Point(void)
{
    if (channel != ON) {
        if (channel == OFF) return;
        start();
        if (channel != ON) return;
    }
    /* A client that has gone mid-step: its pads are let go now, and the
     * game runs on by itself; seen before anyone new is turned away, so a
     * client reconnecting at once is taken, not told it is busy. */
    if (attached && running && (int)(Platform_VBlankCount() - target) < 0 && ControlNet_Gone()) {
        detach("the client left during a step; the game runs on");
    }
    if (!attached) {
        if (!ControlNet_Accept(0)) return;
        attach();
    }
    ControlNet_RefuseOthers("err busy: another client is attached\n");
    if (running && (int)(Platform_VBlankCount() - target) < 0) return;
    running = 0;
    if (owed) {
        owed = 0;
        send_stopped();
    }
    Platform_ControlHold(1);
    for (;;) {
        int got = next_line();
        if (got < 0) {
            detach("the client left; the game runs on");
            break;
        }
        if (!got || handle(line)) break;
    }
    Platform_ControlHold(0);
}
