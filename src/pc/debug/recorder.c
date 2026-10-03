/* The input recorder and player (recorder.h). */
#include "pc/compat/fs.h"
#include "recorder.h"
#include "control.h"
#include "control_protocol.h"
#include "pc/debug/monitor.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t index;
    uint16_t bits[2], fixed[2];
    uint8_t pad2;
} PadEvent;

#define PENDING_MAX 1024 /* changes between two safe points; a frame has a few VBlanks */

static enum { WAITING, RUNNING } phase;
static int recording, playing, started;
static unsigned start_vblank;        /* the VBlank count the indices count from */
static FILE *out;
static char out_path[600];
static unsigned states_every, last_state;
/* Recording: changes noted at the VBlank, written at the next safe point. */
static PadEvent pending[PENDING_MAX], last_written;
static volatile unsigned pending_count, dropped;
static int have_last;
/* Playing: the file's changes, in order. */
static PadEvent *script;
static size_t script_count, script_at;
static unsigned play_end;
static int play_has_end;
static unsigned play_end_frame;
static int clock_written;
/* The safe point Recorder_Point last passed, where a client's commands are taken. */
static unsigned point_index, point_frame;
/* Playing: the C lines, in order. */
typedef struct {
    unsigned index, frame;
    char *line;
} Command;
static Command *commands;
static size_t command_count, command_at;
static volatile uint16_t played[2]; /* the bits given at the last VBlank, played or recorded */

static void begin(void)
{
    started = 1;
    phase = RUNNING;
    start_vblank = Platform_VBlankCount();
}

static void add_command(unsigned index, unsigned frame, const char *text)
{
    static size_t room;
    size_t length = strcspn(text, "\r\n");
    char *copy = malloc(length + 1);
    if (!copy) return;
    if (command_count == room) {
        Command *grown;
        room = room ? room * 2 : 64;
        grown = realloc(commands, room * sizeof(*commands));
        if (!grown) {
            free(copy);
            return;
        }
        commands = grown;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    commands[command_count].index = index;
    commands[command_count].frame = frame;
    commands[command_count++].line = copy;
}

static void read_script(const char *path)
{
    FILE *file = fopen(path, "r");
    /* A C line holds a poke of up to CONTROL_DATA_MAX bytes in hex. */
    static char line[CONTROL_LINE_MAX + 64];
    size_t room = 0;
    if (!file) {
        fprintf(stderr, "memories-pc: replay: cannot read %s; the pads stay idle\n", path);
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        unsigned index, b1, f1, b2, f2, c2, frame;
        int at = 0;
        if (sscanf(line, "C %u %u %n", &index, &frame, &at) == 2 && at) {
            add_command(index, frame, line + at);
        } else if (sscanf(line, "I %u %x %x %x %x %u", &index, &b1, &f1, &b2, &f2, &c2) == 6) {
            if (script_count == room) {
                PadEvent *grown;
                room = room ? room * 2 : 1024;
                grown = realloc(script, room * sizeof(*script));
                if (!grown) break;
                script = grown;
            }
            script[script_count].index = index;
            script[script_count].bits[0] = (uint16_t)b1;
            script[script_count].fixed[0] = (uint16_t)f1;
            script[script_count].bits[1] = (uint16_t)b2;
            script[script_count].fixed[1] = (uint16_t)f2;
            script[script_count].pad2 = (uint8_t)(c2 != 0);
            script_count++;
        } else if (sscanf(line, "E %u %u", &index, &frame) == 2) {
            play_end = index;
            play_end_frame = frame;
            play_has_end = 1;
        }
    }
    fclose(file);
    fprintf(stderr, "memories-pc: replay: %lu pad changes and %lu client commands from %s%s\n",
            (unsigned long)script_count, (unsigned long)command_count, path, play_has_end ? "" : " (no end: it runs on)");
}

/* What the run was made with: the facts every crash report starts with
 * (monitor.h) that decide what a replay sees, as "F key: value" lines. */
static void write_facts(void)
{
    static const char *const keys[] = {"build: ", "os: ", "settings: ", "mods: "};
    static char facts[4096];
    char *line, *next;
    size_t i;
    Monitor_Facts(facts, sizeof(facts));
    for (line = facts; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!strncmp(line, keys[i], strlen(keys[i]))) fprintf(out, "F %s\n", line);
        }
    }
}

static void finish(void)
{
    if (!out) return;
    fprintf(out, "E %u %u\n", started ? Platform_VBlankCount() - start_vblank : 0, Memories_PresentedFrames());
    fclose(out);
    out = NULL;
}

void Recorder_Init(void)
{
    const char *record = getenv("MEMORIES_RECORD"), *play = getenv("MEMORIES_PLAY");
    const char *state = getenv("MEMORIES_LOAD_STATE"), *every = getenv("MEMORIES_RECORD_STATES");
    if (play && *play) {
        playing = 1;
        read_script(play);
    }
    if (record && *record) {
        snprintf(out_path, sizeof(out_path), "%s", record);
        out = fopen(record, "w");
        if (!out) {
            fprintf(stderr, "memories-pc: record: cannot write %s\n", record);
        } else {
            recording = 1;
            fprintf(out, "yfm-recording 1\nbuild %08x\n", (unsigned)Memories_StateBuildId());
            if (state && *state) fprintf(out, "start state %s\n", state);
            else fprintf(out, "start boot\n");
            write_facts();
            fflush(out);
            atexit(finish);
        }
        states_every = every ? (unsigned)strtoul(every, NULL, 10) : 0;
    }
    if (!recording && !playing) return;
    /* From boot the indices are the VBlank count; from a state they start
     * once the state is in (Recorder_Point). */
    if (state && *state) phase = WAITING;
    else begin();
}

void Recorder_Pads(uint16_t bits[2], uint16_t fixed[2], int *pad2_connected)
{
    unsigned index;
    if (!recording && !playing) return;
    if (phase != RUNNING) {
        if (playing) bits[0] = bits[1] = fixed[0] = fixed[1] = 0, *pad2_connected = 0;
        return;
    }
    index = Platform_VBlankCount() - start_vblank;
    if (playing) {
        while (script_at < script_count && script[script_at].index <= index) script_at++;
        if (script_at) {
            const PadEvent *event = &script[script_at - 1];
            bits[0] = event->bits[0];
            bits[1] = event->bits[1];
            fixed[0] = event->fixed[0];
            fixed[1] = event->fixed[1];
            *pad2_connected = event->pad2;
        } else {
            bits[0] = bits[1] = fixed[0] = fixed[1] = 0;
            *pad2_connected = 0;
        }
    }
    /* A mod's host->pad reads these, recording or playing, so both see the
     * same bits (Recorder_HostPad). */
    played[0] = bits[0];
    played[1] = bits[1];
    if (recording) {
        PadEvent event;
        event.index = index;
        event.bits[0] = bits[0];
        event.bits[1] = bits[1];
        event.fixed[0] = fixed[0];
        event.fixed[1] = fixed[1];
        event.pad2 = (uint8_t)(*pad2_connected != 0);
        if (pending_count < PENDING_MAX) pending[pending_count++] = event;
        else dropped++;
    }
}

void Recorder_Command(const char *line)
{
    if (!recording || !out || phase != RUNNING) return;
    fprintf(out, "C %u %u %s\n", point_index, point_frame, line);
    fflush(out);
}

uint16_t Recorder_HostPad(int port, uint16_t live)
{
    return playing || (recording && phase == RUNNING) ? played[port & 1] : live;
}

void Recorder_Point(unsigned frame)
{
    unsigned index, i;
    if (!recording && !playing) return;
    if (phase == WAITING) {
        if (!Memories_StateStartupDone()) return;
        begin();
    }
    index = Platform_VBlankCount() - start_vblank;
    if (recording && out) {
        /* The VBlank's buffer, taken whole: with MEMORIES_CLOCK=interrupt
         * the timer may append to it at any moment. */
        static PadEvent taken[PENDING_MAX];
        unsigned count;
        sigset_t set, previous;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &previous);
        count = pending_count;
        memcpy(taken, pending, count * sizeof(*taken));
        pending_count = 0;
        sigprocmask(SIG_SETMASK, &previous, NULL);
        if (!clock_written) {
            /* Only the virtual clock replays: a run on the real-time clock
             * met each VBlank at its own moment (notes/agent-control.md). */
            clock_written = 1;
            fprintf(out, "F clock: %s\n", Platform_VirtualClock() ? "virtual" : "real");
            if (!Platform_VirtualClock()) {
                fprintf(stderr, "memories-pc: record: the real-time clock runs: this recording will not play back "
                                "(MEMORIES_DETERMINISTIC=1 makes one that does)\n");
            }
        }
        for (i = 0; i < count; i++) {
            const PadEvent *event = &taken[i];
            if (have_last && !memcmp(event->bits, last_written.bits, sizeof(event->bits)) &&
                !memcmp(event->fixed, last_written.fixed, sizeof(event->fixed)) && event->pad2 == last_written.pad2) {
                continue;
            }
            fprintf(out, "I %u %04x %04x %04x %04x %u\n", event->index, event->bits[0], event->fixed[0],
                    event->bits[1], event->fixed[1], event->pad2);
            last_written = *event;
            have_last = 1;
        }
        if (dropped) {
            fprintf(stderr, "memories-pc: record: %u VBlanks of input were dropped (too many between two frames)\n",
                    dropped);
            dropped = 0;
        }
        fprintf(out, "H %u %u %016llx\n", index, frame, Memories_VramHash());
        if (states_every && index - last_state >= states_every) {
            char path[700];
            snprintf(path, sizeof(path), "%s.%u.state", out_path, index);
            if (!Memories_StateSaveHere(path)) {
                fprintf(out, "S %u %u %s\n", index, frame, path);
                last_state = index;
            }
        }
        fflush(out);
    }
    point_index = index;
    point_frame = frame;
    /* The client's commands taken at this point, or at one passed (a play
     * that took another path, which the check reports by its frames). */
    while (playing && command_at < command_count &&
           (commands[command_at].index < index ||
            (commands[command_at].index == index && commands[command_at].frame <= frame))) {
        Command *command = &commands[command_at++];
        if (command->index != index || command->frame != frame) {
            fprintf(stderr, "memories-pc: replay: the command of VBlank %u frame %u comes at VBlank %u frame %u\n",
                    command->index, command->frame, index, frame);
        }
        if (recording && out) {
            fprintf(out, "C %u %u %s\n", command->index, command->frame, command->line);
            fflush(out);
        }
        if (Control_Apply(command->line)) {
            fprintf(stderr, "memories-pc: replay: the game refused the command of VBlank %u: %.80s\n",
                    command->index, command->line);
        }
    }
    /* The end: its VBlank and its frame (a game closed between two VBlanks
     * presents twice in the last one). */
    if (playing && play_has_end && (index > play_end || (index == play_end && frame >= play_end_frame))) {
        fprintf(stderr, "memories-pc: replay: the recording ends at VBlank %u; done\n", play_end);
        playing = 0;
        Platform_RequestQuit();
    }
}
