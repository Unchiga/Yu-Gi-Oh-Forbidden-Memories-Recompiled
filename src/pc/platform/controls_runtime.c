#define _POSIX_C_SOURCE 200809L
#include "controls_runtime.h"
#include "controls_config.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static ControlsConfig active;
static ControllerDevice devices[CONTROLS_DEVICES];
static int initialized, assigned[2] = {-1, -1};
static volatile sig_atomic_t blocked, held, gate = 1;
static unsigned char keys[CTRL_KEY_COUNT];
/* Main thread only: keys pressed since the last update. A press released
 * before the update (a tap shorter than a frame, or a key event injected
 * with its release in the same millisecond) still counts as held for that
 * one update, so the game sees it at least once. */
static unsigned char tapped[CTRL_KEY_COUNT];
static ControlsEvaluator evaluators[2];
static volatile uint16_t keyboard_bits, pad_bits[2];
static volatile int connected[2];
static char load_error[256];
/* Main thread only: the controllers' pad presses, which a notice reads
 * while the game cannot. */
static uint16_t raw_pads, pad_presses;
/* Main thread only: host rows held last update (and so while input runs),
 * pressed since taken, tapped between two updates, and when a repeating one
 * fires next. */
static uint32_t host_down, host_live, host_presses, host_taps;
static uint64_t host_repeat_at[CTRL_HOST_COUNT];
#ifdef __SWITCH__
static int turbo_on;
#endif
#define REPEAT_DELAY_US 400000 /* like a keyboard's own repeat */
#define REPEAT_EVERY_US 60000
uint64_t ControlsRuntime_Now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (unsigned)ts.tv_nsec / 1000;
}
void ControlsRuntime_Init(void)
{
    if (initialized)
        return;
    initialized = 1;
    if (ControlsConfig_Load(&active, load_error, sizeof(load_error)) < 0)
        fprintf(stderr, "memories-pc: %s\n", load_error);
}
const ControlsConfig *ControlsRuntime_Config(void)
{
    ControlsRuntime_Init();
    return &active;
}
const char *ControlsRuntime_Error(void) { return load_error; }
ControllerDevice *ControlsRuntime_Device(int i)
{
    return i >= 0 && i < CONTROLS_DEVICES ? &devices[i] : NULL;
}
static int explicit_device(const ControlsConfig *cfg, int p)
{
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (devices[i].connected && !strcmp(devices[i].identity, cfg->port[p].identity))
            return i;
    return -1;
}
static void resolve(const ControlsConfig *cfg, int result[2])
{
    for (int p = 0; p < 2; p++)
        result[p] = cfg->port[p].mode == 2 ? explicit_device(cfg, p) : -1;
    for (int p = 0; p < 2; p++)
        if (cfg->port[p].mode == 1) {
            int old = assigned[p];
            if (old >= 0 && devices[old].connected && result[1 - p] != old)
                result[p] = old;
            else
                for (int i = 0; i < CONTROLS_DEVICES; i++)
                    if (devices[i].connected && i != result[1 - p]) {
                        result[p] = i;
                        break;
                    }
        }
}
int ControlsRuntime_Assigned(const ControlsConfig *cfg, int p)
{
    int out[2];
    resolve(cfg, out);
    return p >= 0 && p < 2 ? out[p] : -1;
}
void ControlsRuntime_Reconcile(void)
{
    int out[2];
    ControlsRuntime_Init();
    resolve(&active, out);
    for (int p = 0; p < 2; p++)
        if (out[p] != assigned[p]) {
            assigned[p] = out[p];
            memset(&evaluators[p], 0, sizeof(evaluators[p]));
            gate = 1;
        }
}
static ControlsDeviceProfile *profile(ControlsConfig *cfg, int p, int create)
{
    int device;
    if (p < 0 || p >= 2)
        return NULL; /* cfg->port has two entries */
    device = ControlsRuntime_Assigned(cfg, p);
    const char *id = cfg->port[p].mode == 2 ? cfg->port[p].identity
                     : device >= 0          ? devices[device].identity
                                            : "";
    if (!*id)
        return NULL;
    for (int i = 0; i < cfg->profile_count; i++)
        if (!strcmp(id, cfg->profiles[i].identity))
            return &cfg->profiles[i];
    if (!create || cfg->profile_count == CTRL_PROFILE_MAX)
        return NULL;
    ControlsDeviceProfile *pr = &cfg->profiles[cfg->profile_count++];
    memset(pr, 0, sizeof(*pr));
    snprintf(pr->identity, sizeof(pr->identity), "%s", id);
    pr->bindings = cfg->ctrl[p];
    pr->icon = cfg->port[p].icon;
    return pr;
}
ControlsProfile *ControlsRuntime_Profile(ControlsConfig *cfg, int p, int create)
{
    ControlsDeviceProfile *pr = profile(cfg, p, create);
    return pr ? &pr->bindings : &cfg->ctrl[p];
}
CtrlIconStyle *ControlsRuntime_Style(ControlsConfig *cfg, int p, int create)
{
    ControlsDeviceProfile *pr = profile(cfg, p, create);
    return pr ? &pr->icon : &cfg->port[p].icon;
}
int ControlsRuntime_Apply(const ControlsConfig *cfg, char *error, unsigned size)
{
    if (!ControlsConfig_Save(cfg, error, size))
        return 0;
    active = *cfg;
    load_error[0] = 0;
    ControlsRuntime_Gate();
    ControlsRuntime_Reconcile();
    return 1;
}
void ControlsRuntime_Key(int key, int down)
{
    if (key <= 0 || key >= CTRL_KEY_COUNT)
        return;
    /* A tap whose release comes before the next update still counts. */
    for (int h = 0; h < CTRL_HOST_COUNT; h++)
        if (down && !keys[key] && active.kb.host[h][0].kind == CTRL_SRC_KEY && active.kb.host[h][0].code == key)
            host_taps |= 1u << h;
    if (down && !keys[key] && key == CTRL_KEY_ESCAPE)
        host_taps |= 1u << CTRL_HOST_EXIT;
    if (down)
        tapped[key] = 1;
    keys[key] = down != 0;
}
void ControlsRuntime_ResetKeys(void)
{
    memset(keys, 0, sizeof(keys));
    memset(tapped, 0, sizeof(tapped));
    /* A tap of a key let go here (Esc or Back closing a Controls panel) is
     * no host action either: else the next update, open again with every
     * key reset, fires it (Exit game's prompt behind the closed panel). */
    host_taps = 0;
    ControlsRuntime_Gate();
}
int ControlsRuntime_Keys(ControlSource *out)
{
    int n = 0;
    for (int k = 1; k < CTRL_KEY_COUNT; k++)
        if (keys[k])
            out[n++] = (ControlSource){CTRL_SRC_KEY, (uint16_t)k, 0};
    return n;
}
void ControlsRuntime_Gate(void)
{
    sigset_t all, previous;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &previous);
    gate = 1;
    keyboard_bits = pad_bits[0] = pad_bits[1] = 0;
    memset(evaluators, 0, sizeof(evaluators));
    sigprocmask(SIG_SETMASK, &previous, NULL);
}
void ControlsRuntime_Block(int b)
{
    blocked = b;
    ControlsRuntime_Gate();
}
void ControlsRuntime_Hold(int h)
{
    if (!h == !held)
        return;
    held = h != 0;
    /* A press from before the notice came up does not answer it. */
    if (held)
        pad_presses = 0;
    ControlsRuntime_Gate();
}
uint16_t ControlsRuntime_TakePadPresses(void)
{
    uint16_t out = pad_presses;
    pad_presses = 0;
    return out;
}
int ControlsRuntime_Blocked(void) { return blocked || held || gate; }
uint32_t ControlsRuntime_TakeHost(void)
{
    uint32_t out = host_presses;
    host_presses = 0;
    return out;
}
uint32_t ControlsRuntime_HostHeld(void) { return host_live; }
int ControlsRuntime_KeyDown(int key) { return key > 0 && key < CTRL_KEY_COUNT && keys[key]; }

uint16_t ControlsRuntime_Keyboard(void) { return keyboard_bits; }
uint16_t ControlsRuntime_Pad(int p) { return p >= 0 && p < 2 ? pad_bits[p] : 0; }
int ControlsRuntime_Connected(int p) { return p >= 0 && p < 2 ? connected[p] : 0; }
int ControlsRuntime_Sources(const ControllerDevice *d, ControlSource *out, int capacity, int neutral)
{
    int n = 0;
    float a = d->threshold > 0 ? d->threshold : 1.0f / 3;
    if (neutral)
        a *= 0.8f;
#define ADD(kind_, code_, sign_)                                                                             \
    do {                                                                                                     \
        if (n < capacity)                                                                                    \
            out[n++] = (ControlSource){kind_, (uint16_t)(code_), sign_};                                     \
    } while (0)
    if (!d->connected)
        return 0;
    for (int b = 1; b < CTRL_BTN_COUNT; b++)
        if (d->snapshot.buttons_down & (1u << (b - 1)))
            ADD(CTRL_SRC_BUTTON, b, 0);
    for (int h = 1; h <= 8; h *= 2)
        if (d->snapshot.hat_down & h)
            ADD(CTRL_SRC_HAT, h, 0);
    for (int i = 1; i < CTRL_AXIS_COUNT; i++) {
        if (d->snapshot.axis[i] > a)
            ADD(CTRL_SRC_AXIS, i, 1);
        if (d->snapshot.axis[i] < -a)
            ADD(CTRL_SRC_AXIS, i, -1);
    }
    for (int i = 1; i < CTRL_TRIGGER_COUNT; i++)
        if (d->snapshot.trigger[i] > (neutral ? 0.8f / 3 : 1.0f / 3))
            ADD(CTRL_SRC_TRIGGER, i, 0);
#undef ADD
    return n;
}
void ControlsRuntime_Update(void)
{
    ControlSource down[CTRL_KEY_COUNT], sources[64];
    uint64_t kb, pads[2] = {0}, now = ControlsRuntime_Now();
    uint32_t host;
    int conn[2], neutral, stopped;
    sigset_t all, previous;
    ControlsRuntime_Reconcile();
    int n = ControlsRuntime_Keys(down);
    for (int k = 1; k < CTRL_KEY_COUNT; k++)
        if (tapped[k] && !keys[k])
            down[n++] = (ControlSource){CTRL_SRC_KEY, (uint16_t)k, 0};
    memset(tapped, 0, sizeof(tapped));
    neutral = n == 0;
    kb = Controls_EvalKeyboardRows(&active.kb, down, n);
    /* Esc in a window is always Exit game, bound or not (the backends keep
     * it for fullscreen and menus first), so clearing or moving Exit's key
     * never loses the way out; a key bound to Exit adds to it. */
    for (int i = 0; i < n; i++)
        if (down[i].code == CTRL_KEY_ESCAPE)
            kb |= (uint64_t)1 << (CTRL_DEST_COUNT + CTRL_HOST_EXIT);
    for (int p = 0; p < 2; p++) {
        int i = assigned[p];
        conn[p] = i >= 0;
        if (i < 0)
            continue;
        if (ControlsRuntime_Sources(&devices[i], sources, 64, 1))
            neutral = 0;
        evaluators[p].activation = devices[i].threshold;
        pads[p] = Controls_EvalControllerRows(ControlsRuntime_Profile(&active, p, 0), &devices[i].snapshot,
                                              &evaluators[p]);
    }
    if (gate && neutral && !blocked && !held)
        gate = 0;
    stopped = blocked || held || gate;
    pad_presses |= (uint16_t)(pads[0] | pads[1]) & ~raw_pads;
    raw_pads = (uint16_t)(pads[0] | pads[1]);
    /* A host action fires once per press, never for a press begun while
     * input was stopped (the key that closed a window, a held button). */
    host = (uint32_t)((kb | pads[0] | pads[1]) >> CTRL_DEST_COUNT);
    if (!stopped) {
        uint32_t fresh = (host & ~host_down) | host_taps;
        host_presses |= fresh;
#ifdef __SWITCH__
        if (fresh >> CTRL_HOST_TURBO & 1) turbo_on = !turbo_on;
#endif
        for (int h = 0; h < CTRL_HOST_COUNT; h++) {
            if (Controls_HostActions[h].mode != CTRL_HOST_REPEAT || !(host >> h & 1))
                continue;
            if (fresh >> h & 1)
                host_repeat_at[h] = now + REPEAT_DELAY_US;
            else if (now >= host_repeat_at[h]) {
                host_presses |= 1u << h;
                host_repeat_at[h] = now + REPEAT_EVERY_US;
            }
        }
    }
    host_down = host;
    host_live = stopped ? 0 : host;
#ifdef __SWITCH__
    /* Turbo is a switch there (L3, controls.c): a press turns it on, the
     * next off, and it stays through a menu. */
    host_live = (host_live & ~(1u << CTRL_HOST_TURBO)) | (uint32_t)turbo_on << CTRL_HOST_TURBO;
#endif
    host_taps = 0;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &previous);
    keyboard_bits = stopped ? 0 : (uint16_t)kb;
    for (int p = 0; p < 2; p++) {
        pad_bits[p] = stopped ? 0 : (uint16_t)pads[p];
        connected[p] = conn[p];
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}
