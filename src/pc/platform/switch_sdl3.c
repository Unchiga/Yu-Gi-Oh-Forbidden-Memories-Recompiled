/* The SDL3 calls of src/pc/compat/switch/SDL3/sdl3_shim.h, over devkitPro's
 * SDL2 for the Switch. This unit sees SDL2's own names (it does not include
 * the shim's <SDL3/SDL.h>). */
#ifdef __SWITCH__
#include "pc/compat/switch/SDL3/sdl3_shim.h"
#include <SDL2/SDL_opengles2.h>
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- init and time ------------------------------------------------------- */

static void hints(void)
{
    /* SDL3 names gamepad buttons by place (SOUTH is the bottom one); SDL2's
     * Xbox names are places too once it stops swapping Nintendo's A and B
     * by their labels. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    /* The mouse a touch makes is the shim's (touch_mouse), as SDL3 makes it. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
}

bool Shim_Init(Uint32 flags)
{
    hints();
    return SDL_Init(flags) == 0;
}

bool Shim_InitSubSystem(Uint32 flags)
{
    hints();
    return SDL_InitSubSystem(flags) == 0;
}

int Shim_GetVersion(void)
{
    SDL_version version;
    SDL_GetVersion(&version);
    return version.major * 1000000 + version.minor * 1000 + version.patch;
}

Uint64 Shim_GetTicksNS(void)
{
    const Uint64 count = SDL_GetPerformanceCounter(), rate = SDL_GetPerformanceFrequency();
    return count / rate * 1000000000ull + count % rate * 1000000000ull / rate;
}

/* --- events -------------------------------------------------------------- */

#define QUEUE 64
static SDL_Window *touch_window; /* the game's: the touch screen is its */
static Shim_Event queue[QUEUE]; /* pushed by the port, given out before SDL2's */
static unsigned queue_head, queue_count;
static char text[sizeof(((SDL_TextInputEvent *)0)->text)];

static void header(Shim_Event *out, Uint32 type, Uint32 timestamp, Uint32 window)
{
    memset(out, 0, sizeof(*out));
    out->common.type = type;
    out->common.timestamp = (Uint64)timestamp * 1000000ull;
    out->common.windowID = window;
}

static Uint32 window_type(Uint8 event)
{
    switch (event) {
    case SDL_WINDOWEVENT_EXPOSED: return SHIM_EVENT_WINDOW_EXPOSED;
    case SDL_WINDOWEVENT_MOVED: return SHIM_EVENT_WINDOW_MOVED;
    case SDL_WINDOWEVENT_RESIZED: return SHIM_EVENT_WINDOW_RESIZED;
    case SDL_WINDOWEVENT_SIZE_CHANGED: return SHIM_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    case SDL_WINDOWEVENT_ENTER: return SHIM_EVENT_WINDOW_MOUSE_ENTER;
    case SDL_WINDOWEVENT_LEAVE: return SHIM_EVENT_WINDOW_MOUSE_LEAVE;
    case SDL_WINDOWEVENT_FOCUS_GAINED: return SHIM_EVENT_WINDOW_FOCUS_GAINED;
    case SDL_WINDOWEVENT_FOCUS_LOST: return SHIM_EVENT_WINDOW_FOCUS_LOST;
    case SDL_WINDOWEVENT_CLOSE: return SHIM_EVENT_WINDOW_CLOSE_REQUESTED;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED: return SHIM_EVENT_WINDOW_DISPLAY_CHANGED;
    default: return 0;
    }
}

/* SDL2's event as SDL3's; 0 for one the port has no use for. */
static int translate(const SDL_Event *in, Shim_Event *out)
{
    switch (in->type) {
    case SDL_QUIT: header(out, SHIM_EVENT_QUIT, in->quit.timestamp, 0); return 1;
    case SDL_APP_WILLENTERBACKGROUND: header(out, SHIM_EVENT_WILL_ENTER_BACKGROUND, in->common.timestamp, 0); return 1;
    case SDL_APP_DIDENTERBACKGROUND: header(out, SHIM_EVENT_DID_ENTER_BACKGROUND, in->common.timestamp, 0); return 1;
    case SDL_APP_WILLENTERFOREGROUND: header(out, SHIM_EVENT_WILL_ENTER_FOREGROUND, in->common.timestamp, 0); return 1;
    case SDL_APP_DIDENTERFOREGROUND: header(out, SHIM_EVENT_DID_ENTER_FOREGROUND, in->common.timestamp, 0); return 1;
    case SDL_RENDER_DEVICE_RESET: header(out, SHIM_EVENT_RENDER_DEVICE_RESET, in->common.timestamp, 0); return 1;
    case SDL_KEYMAPCHANGED: header(out, SHIM_EVENT_KEYMAP_CHANGED, in->common.timestamp, 0); return 1;
    case SDL_WINDOWEVENT: {
        const Uint32 type = window_type(in->window.event);
        if (!type) return 0;
        header(out, type, in->window.timestamp, in->window.windowID);
        out->window.data1 = in->window.data1;
        out->window.data2 = in->window.data2;
        return 1;
    }
    case SDL_KEYDOWN: case SDL_KEYUP:
        header(out, in->type == SDL_KEYDOWN ? SHIM_EVENT_KEY_DOWN : SHIM_EVENT_KEY_UP, in->key.timestamp,
               in->key.windowID);
        out->key.scancode = in->key.keysym.scancode;
        out->key.key = in->key.keysym.sym;
        out->key.mod = in->key.keysym.mod;
        out->key.down = in->key.state == SDL_PRESSED;
        out->key.repeat = in->key.repeat != 0;
        return 1;
    case SDL_TEXTINPUT:
        header(out, SHIM_EVENT_TEXT_INPUT, in->text.timestamp, in->text.windowID);
        memcpy(text, in->text.text, sizeof(text));
        text[sizeof(text) - 1] = '\0';
        out->text.text = text;
        return 1;
    case SDL_MOUSEMOTION:
        header(out, SHIM_EVENT_MOUSE_MOTION, in->motion.timestamp, in->motion.windowID);
        out->motion.which = in->motion.which;
        out->motion.state = in->motion.state;
        out->motion.x = (float)in->motion.x;
        out->motion.y = (float)in->motion.y;
        out->motion.xrel = (float)in->motion.xrel;
        out->motion.yrel = (float)in->motion.yrel;
        return 1;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        header(out, in->type == SDL_MOUSEBUTTONDOWN ? SHIM_EVENT_MOUSE_BUTTON_DOWN : SHIM_EVENT_MOUSE_BUTTON_UP,
               in->button.timestamp, in->button.windowID);
        out->button.which = in->button.which;
        out->button.button = in->button.button;
        out->button.down = in->button.state == SDL_PRESSED;
        out->button.clicks = in->button.clicks;
        out->button.x = (float)in->button.x;
        out->button.y = (float)in->button.y;
        return 1;
    case SDL_MOUSEWHEEL:
        header(out, SHIM_EVENT_MOUSE_WHEEL, in->wheel.timestamp, in->wheel.windowID);
        out->wheel.which = in->wheel.which;
        out->wheel.x = in->wheel.preciseX;
        out->wheel.y = in->wheel.preciseY;
        out->wheel.direction = in->wheel.direction;
        out->wheel.mouse_x = (float)in->wheel.mouseX;
        out->wheel.mouse_y = (float)in->wheel.mouseY;
        return 1;
    case SDL_CONTROLLERDEVICEADDED: /* SDL2 gives the device index here, SDL3 the instance */
        header(out, SHIM_EVENT_GAMEPAD_ADDED, in->cdevice.timestamp, 0);
        out->gdevice.which = SDL_JoystickGetDeviceInstanceID(in->cdevice.which);
        return 1;
    case SDL_CONTROLLERDEVICEREMOVED:
        header(out, SHIM_EVENT_GAMEPAD_REMOVED, in->cdevice.timestamp, 0);
        out->gdevice.which = in->cdevice.which;
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
        header(out, SHIM_EVENT_GAMEPAD_BUTTON_DOWN, in->cbutton.timestamp, 0);
        out->gbutton.which = in->cbutton.which;
        out->gbutton.button = in->cbutton.button;
        out->gbutton.down = 1;
        return 1;
    case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
        header(out, in->type == SDL_FINGERDOWN ? SHIM_EVENT_FINGER_DOWN
                    : in->type == SDL_FINGERUP ? SHIM_EVENT_FINGER_UP : SHIM_EVENT_FINGER_MOTION,
               in->tfinger.timestamp, in->tfinger.windowID);
        out->tfinger.touchID = (Uint64)in->tfinger.touchId;
        out->tfinger.fingerID = (Uint64)in->tfinger.fingerId;
        out->tfinger.x = in->tfinger.x;
        out->tfinger.y = in->tfinger.y;
        out->tfinger.dx = in->tfinger.dx;
        out->tfinger.dy = in->tfinger.dy;
        out->tfinger.pressure = in->tfinger.pressure;
        return 1;
    default:
        return 0;
    }
}

/* SDL3's mouse from a touch (SDL_TOUCH_MOUSEID), which the menu and the
 * panels take taps from as on Android: the first finger down moves it and
 * holds its left button until it lifts; the others are fingers only. Queued
 * after the finger's own event. */
static void touch_mouse(const SDL_TouchFingerEvent *finger)
{
    static SDL_FingerID held = -1;
    Shim_Event mouse;
    int w = 0, h = 0;
    float x, y;
    if (!touch_window) return;
    if (finger->type == SDL_FINGERDOWN && held == -1) held = finger->fingerId;
    if (finger->fingerId != held) return;
    SDL_GetWindowSize(touch_window, &w, &h);
    x = finger->x * (float)w;
    y = finger->y * (float)h;
    header(&mouse, SHIM_EVENT_MOUSE_MOTION, finger->timestamp, SDL_GetWindowID(touch_window));
    mouse.motion.which = SDL_TOUCH_MOUSEID;
    mouse.motion.state = finger->type == SDL_FINGERUP ? 0 : SDL_BUTTON_LMASK;
    mouse.motion.x = x;
    mouse.motion.y = y;
    if (finger->type == SDL_FINGERDOWN) mouse.motion.state = 0; /* moved there, then pressed */
    Shim_PushEvent(&mouse);
    if (finger->type == SDL_FINGERMOTION) return;
    header(&mouse, finger->type == SDL_FINGERDOWN ? SHIM_EVENT_MOUSE_BUTTON_DOWN : SHIM_EVENT_MOUSE_BUTTON_UP,
           finger->timestamp, SDL_GetWindowID(touch_window));
    mouse.button.which = SDL_TOUCH_MOUSEID;
    mouse.button.button = SDL_BUTTON_LEFT;
    mouse.button.down = finger->type == SDL_FINGERDOWN;
    mouse.button.clicks = 1;
    mouse.button.x = x;
    mouse.button.y = y;
    Shim_PushEvent(&mouse);
    if (finger->type == SDL_FINGERUP) held = -1;
}

bool Shim_PollEvent(Shim_Event *event)
{
    SDL_Event in;
    if (queue_count) {
        if (event) *event = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE;
        queue_count--;
        return 1;
    }
    while (SDL_PollEvent(&in)) {
        Shim_Event out;
        if (!translate(&in, &out)) continue;
        if (in.type == SDL_FINGERDOWN || in.type == SDL_FINGERUP || in.type == SDL_FINGERMOTION)
            touch_mouse(&in.tfinger);
        if (event) *event = out;
        return 1;
    }
    return 0;
}

bool Shim_PushEvent(Shim_Event *event)
{
    if (queue_count == QUEUE) {
        SDL_SetError("the event queue is full");
        return 0;
    }
    queue[(queue_head + queue_count++) % QUEUE] = *event;
    return 1;
}

static Shim_EventFilter watch;
static void *watch_data;

static int SDLCALL watched(void *userdata, SDL_Event *in)
{
    Shim_Event out;
    (void)userdata;
    if (watch && translate(in, &out)) watch(watch_data, &out);
    return 1;
}

bool Shim_AddEventWatch(Shim_EventFilter filter, void *userdata)
{
    if (watch) return SDL_SetError("the shim keeps one event watch") == 0;
    watch = filter;
    watch_data = userdata;
    SDL_AddEventWatch(watched, NULL);
    return 1;
}

SDL_Window *Shim_GetWindowFromEvent(const Shim_Event *event)
{
    return event && event->common.windowID ? SDL_GetWindowFromID(event->common.windowID) : NULL;
}

/* --- windows ------------------------------------------------------------- */

/* The Switch has one screen, 1280x720 in the hand and 1920x1080 docked: the
 * window is always all of it, whatever size is asked. */
SDL_Window *Shim_CreateWindow(const char *title, int w, int h, Uint64 flags)
{
    SDL_DisplayMode mode;
    SDL_Window *window;
    if (!SDL_GetDesktopDisplayMode(0, &mode) && mode.w > 0 && mode.h > 0) {
        w = mode.w;
        h = mode.h;
    }
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, (Uint32)flags);
    if (window && !touch_window) touch_window = window;
    return window;
}

bool Shim_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    if (!window) return 0;
    SDL_GetWindowSize(window, w, h);
    return 1;
}

bool Shim_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{
    if (!window) return 0;
    SDL_GetWindowSizeInPixels(window, w, h);
    return 1;
}

bool Shim_SetWindowSize(SDL_Window *window, int w, int h) { (void)window; (void)w; (void)h; return 1; }
bool Shim_SetWindowPosition(SDL_Window *window, int x, int y) { (void)window; (void)x; (void)y; return 1; }
bool Shim_SetWindowMinimumSize(SDL_Window *window, int w, int h) { (void)window; (void)w; (void)h; return 1; }
bool Shim_SetWindowBordered(SDL_Window *window, bool bordered) { (void)window; (void)bordered; return 1; }
bool Shim_SetWindowFullscreen(SDL_Window *window, bool fullscreen) { (void)window; (void)fullscreen; return 1; }
bool Shim_SetWindowFullscreenMode(SDL_Window *window, const Shim_DisplayMode *mode)
{
    (void)window;
    (void)mode;
    return 1;
}
bool Shim_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) { (void)window; (void)icon; return 1; }
bool Shim_RaiseWindow(SDL_Window *window) { (void)window; return 1; }

bool Shim_GetWindowSafeArea(SDL_Window *window, SDL_Rect *rect)
{
    if (!window || !rect) return 0;
    rect->x = rect->y = 0;
    SDL_GetWindowSize(window, &rect->w, &rect->h);
    return 1;
}

Shim_PropertiesID Shim_GetWindowProperties(SDL_Window *window) { (void)window; return 0; }

bool Shim_StartTextInput(SDL_Window *window)
{
    (void)window;
    SDL_StartTextInput();
    return 1;
}

bool Shim_StopTextInput(SDL_Window *window)
{
    (void)window;
    SDL_StopTextInput();
    return 1;
}

bool Shim_ShowCursor(void) { return SDL_ShowCursor(SDL_ENABLE) >= 0; }
bool Shim_HideCursor(void) { return SDL_ShowCursor(SDL_DISABLE) >= 0; }

/* --- displays ------------------------------------------------------------ */

#define DISPLAYS 4

static void mode_from(const SDL_DisplayMode *in, Shim_DisplayID display, Shim_DisplayMode *out)
{
    memset(out, 0, sizeof(*out));
    out->displayID = display;
    out->format = in->format;
    out->w = in->w;
    out->h = in->h;
    out->pixel_density = 1.0f;
    out->refresh_rate = (float)in->refresh_rate;
    out->refresh_rate_numerator = in->refresh_rate;
    out->refresh_rate_denominator = 1;
}

Shim_DisplayID Shim_GetPrimaryDisplay(void) { return SDL_GetNumVideoDisplays() > 0 ? 1 : 0; }

Shim_DisplayID Shim_GetDisplayForWindow(SDL_Window *window)
{
    const int index = window ? SDL_GetWindowDisplayIndex(window) : -1;
    return index >= 0 ? (Shim_DisplayID)index + 1 : Shim_GetPrimaryDisplay();
}

float Shim_GetDisplayContentScale(Shim_DisplayID display) { return display ? 1.0f : 0.0f; }

bool Shim_GetDisplayUsableBounds(Shim_DisplayID display, SDL_Rect *rect)
{
    return display && SDL_GetDisplayUsableBounds((int)display - 1, rect) == 0;
}

static const Shim_DisplayMode *display_mode(Shim_DisplayID display, int desktop)
{
    static Shim_DisplayMode modes[2][DISPLAYS];
    SDL_DisplayMode mode;
    if (!display || display > DISPLAYS) return NULL;
    if ((desktop ? SDL_GetDesktopDisplayMode : SDL_GetCurrentDisplayMode)((int)display - 1, &mode)) return NULL;
    mode_from(&mode, display, &modes[desktop][display - 1]);
    return &modes[desktop][display - 1];
}

const Shim_DisplayMode *Shim_GetCurrentDisplayMode(Shim_DisplayID display) { return display_mode(display, 0); }
const Shim_DisplayMode *Shim_GetDesktopDisplayMode(Shim_DisplayID display) { return display_mode(display, 1); }

/* One allocation, as SDL3 makes it: the pointers (NULL-terminated), then the
 * modes they point at. */
Shim_DisplayMode **Shim_GetFullscreenDisplayModes(Shim_DisplayID display, int *count)
{
    int total = display ? SDL_GetNumDisplayModes((int)display - 1) : -1, i;
    Shim_DisplayMode **list, *modes;
    if (count) *count = 0;
    if (total < 0) return NULL;
    list = SDL_malloc((size_t)(total + 1) * sizeof(*list) + (size_t)total * sizeof(*modes));
    if (!list) return NULL;
    modes = (Shim_DisplayMode *)(list + total + 1);
    for (i = 0; i < total; i++) {
        SDL_DisplayMode mode;
        list[i] = &modes[i];
        if (SDL_GetDisplayMode((int)display - 1, i, &mode)) memset(&mode, 0, sizeof(mode));
        mode_from(&mode, display, &modes[i]);
    }
    list[total] = NULL;
    if (count) *count = total;
    return list;
}

bool Shim_GetClosestFullscreenDisplayMode(Shim_DisplayID display, int w, int h, float refresh, bool high_density,
                                          Shim_DisplayMode *closest)
{
    SDL_DisplayMode want = {0}, got;
    (void)high_density;
    if (!display) return 0;
    want.w = w;
    want.h = h;
    want.refresh_rate = (int)(refresh + 0.5f);
    if (!SDL_GetClosestDisplayMode((int)display - 1, &want, &got)) return 0;
    mode_from(&got, display, closest);
    return 1;
}

/* --- renderer, textures, surfaces ---------------------------------------- */

SDL_Renderer *Shim_CreateRenderer(SDL_Window *window, const char *name)
{
    int index = -1, i;
    if (name) {
        for (i = 0; i < SDL_GetNumRenderDrivers(); i++) {
            SDL_RendererInfo info;
            if (!SDL_GetRenderDriverInfo(i, &info) && info.name && !strcmp(info.name, name)) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            SDL_SetError("no renderer named %s", name);
            return NULL;
        }
    }
    return SDL_CreateRenderer(window, index, SDL_RENDERER_ACCELERATED);
}

const char *Shim_GetRendererName(SDL_Renderer *renderer)
{
    SDL_RendererInfo info;
    return renderer && !SDL_GetRendererInfo(renderer, &info) && info.name ? info.name : "";
}

bool Shim_GetRenderOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    return SDL_GetRendererOutputSize(renderer, w, h) == 0;
}

bool Shim_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *source,
                        const SDL_FRect *destination)
{
    SDL_Rect rect, *from = NULL;
    if (source) {
        rect.x = (int)source->x;
        rect.y = (int)source->y;
        rect.w = (int)source->w;
        rect.h = (int)source->h;
        from = &rect;
    }
    return SDL_RenderCopyF(renderer, texture, from, destination) == 0;
}

SDL_Surface *Shim_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    SDL_Surface *surface;
    int w, h;
    if (rect) {
        w = rect->w;
        h = rect->h;
    } else if (SDL_GetRendererOutputSize(renderer, &w, &h)) {
        return NULL;
    }
    surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface && SDL_RenderReadPixels(renderer, rect, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch)) {
        SDL_FreeSurface(surface);
        surface = NULL;
    }
    return surface;
}

bool Shim_SetRenderVSync(SDL_Renderer *renderer, int vsync) { return SDL_RenderSetVSync(renderer, vsync != 0) == 0; }
bool Shim_FlushRenderer(SDL_Renderer *renderer) { return SDL_RenderFlush(renderer) == 0; }

bool Shim_SetTextureScaleMode(SDL_Texture *texture, int mode)
{
    return SDL_SetTextureScaleMode(texture, mode == 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest) == 0;
}

/* A texture's properties are asked for one thing, its GL name: the texture
 * asked about last is remembered, and the name read when it is wanted. */
#define TEXTURE_PROPERTIES 1u
static SDL_Texture *properties_texture;

Shim_PropertiesID Shim_GetTextureProperties(SDL_Texture *texture)
{
    properties_texture = texture;
    return texture ? TEXTURE_PROPERTIES : 0;
}

Sint64 Shim_GetNumberProperty(Shim_PropertiesID properties, const char *name, Sint64 otherwise)
{
    GLint bound = 0;
    if (properties != TEXTURE_PROPERTIES || !properties_texture || strcmp(name, "SDL.texture.opengles2.texture"))
        return otherwise;
    if (SDL_GL_BindTexture(properties_texture, NULL, NULL)) return otherwise;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    SDL_GL_UnbindTexture(properties_texture);
    return bound ? bound : otherwise;
}

void *Shim_GetPointerProperty(Shim_PropertiesID properties, const char *name, void *otherwise)
{
    (void)properties;
    (void)name;
    return otherwise;
}

SDL_Surface *Shim_CreateSurface(int w, int h, Uint32 format)
{
    return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, format);
}

SDL_Surface *Shim_CreateSurfaceFrom(int w, int h, Uint32 format, void *pixels, int pitch)
{
    return SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, pitch, format);
}

/* A larger icon for a high-density screen: the Switch shows no window icon. */
bool Shim_AddSurfaceAlternateImage(SDL_Surface *surface, SDL_Surface *image)
{
    (void)surface;
    (void)image;
    return 1;
}

bool Shim_SaveBMP(SDL_Surface *surface, const char *path)
{
    return SDL_SaveBMP_RW(surface, SDL_RWFromFile(path, "wb"), 1) == 0;
}

/* --- OpenGL --------------------------------------------------------------- */

bool Shim_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context) { return SDL_GL_MakeCurrent(window, context) == 0; }
bool Shim_GL_SetSwapInterval(int interval) { return SDL_GL_SetSwapInterval(interval) == 0; }
bool Shim_GL_SetAttribute(SDL_GLattr attribute, int value) { return SDL_GL_SetAttribute(attribute, value) == 0; }

bool Shim_GL_SwapWindow(SDL_Window *window)
{
    SDL_GL_SwapWindow(window);
    return 1;
}

/* --- gamepads -------------------------------------------------------------- */

/* SDL3 names a gamepad by its instance from the start; SDL2 opens one by its
 * device index. */
SDL_JoystickID *Shim_GetGamepads(int *count)
{
    const int total = SDL_NumJoysticks();
    SDL_JoystickID *ids = SDL_malloc((size_t)(total > 0 ? total + 1 : 1) * sizeof(*ids));
    int found = 0, i;
    if (count) *count = 0;
    if (!ids) return NULL;
    for (i = 0; i < total; i++)
        if (SDL_IsGameController(i)) ids[found++] = SDL_JoystickGetDeviceInstanceID(i);
    ids[found] = 0;
    if (count) *count = found;
    return ids;
}

SDL_GameController *Shim_OpenGamepad(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) return SDL_GameControllerOpen(i);
    SDL_SetError("no gamepad %d", (int)id);
    return NULL;
}

SDL_JoystickID Shim_GetGamepadID(SDL_GameController *gamepad)
{
    return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
}

bool Shim_GetGamepadButton(SDL_GameController *gamepad, int button)
{
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) return 0; /* SDL3's MISC2-6 */
    return SDL_GameControllerGetButton(gamepad, (SDL_GameControllerButton)button) != 0;
}

/* --- audio ------------------------------------------------------------------ */

/* SDL3's stream, which the port fills from SDL3's callback, as the queue
 * SDL2's callback takes the device's buffer from. Filled and emptied on
 * SDL2's audio thread only; Shim_GetAudioStreamQueued reads the count from
 * the game's, for statistics. */
struct Shim_AudioStream {
    SDL_AudioDeviceID device;
    Shim_AudioStreamCallback callback;
    void *userdata;
    Uint8 *ring;
    int size, head, moved;
    volatile int count;
};

static void SDLCALL pull(void *userdata, Uint8 *out, int length)
{
    Shim_AudioStream *stream = userdata;
    int done = 0;
    if (!stream->moved) {
        /* SDL2 starts its threads on the game's core (0); the mixer, which
         * runs the SPU, gets a core of its own (an application has 0-2). */
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
        stream->moved = 1;
    }
    if (stream->count < length) stream->callback(stream->userdata, stream, length - stream->count, length);
    while (done < length && stream->count) {
        int run = stream->size - stream->head;
        if (run > stream->count) run = stream->count;
        if (run > length - done) run = length - done;
        memcpy(out + done, stream->ring + stream->head, (size_t)run);
        stream->head = (stream->head + run) % stream->size;
        stream->count -= run;
        done += run;
    }
    if (done < length) memset(out + done, 0, (size_t)(length - done));
}

Shim_AudioStream *Shim_OpenAudioDeviceStream(Uint32 device, const Shim_AudioSpec *spec,
                                             Shim_AudioStreamCallback callback, void *userdata)
{
    const char *frames = SDL_GetHint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES");
    SDL_AudioSpec want = {0}, have;
    Shim_AudioStream *stream;
    (void)device;
    if (!spec || !callback) {
        SDL_SetError("no audio spec or callback");
        return NULL;
    }
    if (!(stream = SDL_calloc(1, sizeof(*stream)))) return NULL;
    want.freq = spec->freq;
    want.format = (SDL_AudioFormat)spec->format;
    want.channels = (Uint8)spec->channels;
    want.samples = (Uint16)(frames && atoi(frames) > 0 ? atoi(frames) : 1024);
    want.callback = pull;
    want.userdata = stream;
    stream->callback = callback;
    stream->userdata = userdata;
    /* Room for a device buffer twice over and the port's 1 KiB chunks. */
    stream->size = (int)want.samples * spec->channels * 2 * 4 + 4096;
    stream->ring = SDL_malloc((size_t)stream->size);
    stream->device = stream->ring ? SDL_OpenAudioDevice(NULL, 0, &want, &have, 0) : 0;
    if (!stream->device) {
        SDL_free(stream->ring);
        SDL_free(stream);
        return NULL;
    }
    return stream;
}

bool Shim_ResumeAudioStreamDevice(Shim_AudioStream *stream)
{
    if (!stream) return 0;
    SDL_PauseAudioDevice(stream->device, 0);
    return 1;
}

bool Shim_PutAudioStreamData(Shim_AudioStream *stream, const void *data, int length)
{
    const Uint8 *from = data;
    if (!stream || length < 0) return 0;
    if (length > stream->size - stream->count) length = stream->size - stream->count; /* full: the rest waits */
    while (length > 0) {
        const int tail = (stream->head + stream->count) % stream->size;
        int run = stream->size - tail;
        if (run > length) run = length;
        memcpy(stream->ring + tail, from, (size_t)run);
        stream->count += run;
        from += run;
        length -= run;
    }
    return 1;
}

int Shim_GetAudioStreamQueued(Shim_AudioStream *stream) { return stream ? stream->count : -1; }

/* --- the rest ------------------------------------------------------------------ */

/* SDL2 has no message box on the Switch: the system's error dialog shows
 * the title, with the message one press away. */
static void error_dialog(const char *title, const char *message)
{
    ErrorApplicationConfig config;
    errorApplicationCreate(&config, title && *title ? title : "Yu-Gi-Oh! Forbidden Memories",
                           message && *message ? message : NULL);
    errorApplicationShow(&config);
}

bool Shim_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
    (void)flags;
    (void)window;
    error_dialog(title, message);
    return 1;
}

/* The answer is the button Enter would press: the dialog has only OK. */
bool Shim_ShowMessageBox(const Shim_MessageBoxData *data, int *button)
{
    int i;
    if (!data) return 0;
    error_dialog(data->title, data->message);
    if (button) {
        *button = data->numbuttons > 0 ? data->buttons[0].buttonID : -1;
        for (i = 0; i < data->numbuttons; i++)
            if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) *button = data->buttons[i].buttonID;
    }
    return 1;
}

bool Shim_OpenURL(const char *url)
{
    (void)url;
    return SDL_SetError("the Switch opens no URL") == 0;
}

bool Shim_SetClipboardText(const char *value) { return SDL_SetClipboardText(value) == 0; }

SDL_Keycode Shim_GetKeyFromScancode(SDL_Scancode scancode, Uint16 modifiers, bool key_event)
{
    (void)modifiers;
    (void)key_event;
    return SDL_GetKeyFromScancode(scancode);
}

SDL_Scancode Shim_GetScancodeFromKey(SDL_Keycode key, Uint16 *modifiers)
{
    if (modifiers) *modifiers = 0;
    return SDL_GetScancodeFromKey(key);
}
/* --- the renderer's OpenGL ES state ------------------------------------- */

#define SAVED_ATTRIBUTES 8
#define SAVED_CAPS 5
static const GLenum saved_caps[SAVED_CAPS] = {GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_CULL_FACE,
                                              GL_STENCIL_TEST};
static struct {
    int saved;
    GLint viewport[4], scissor[4], program, active, texture, array_buffer, vertex_array;
    GLint blend_rgb_src, blend_rgb_dst, blend_alpha_src, blend_alpha_dst;
    GLint attributes[SAVED_ATTRIBUTES];
    GLboolean caps[SAVED_CAPS], mask[4];
} gl_saved;
static void (*bind_vertex_array)(GLuint); /* OpenGL ES 3's, when there */

void Shim_SaveRendererGL(void)
{
    int i;
    if (!bind_vertex_array) bind_vertex_array = (void (*)(GLuint))SDL_GL_GetProcAddress("glBindVertexArray");
    glGetIntegerv(GL_VIEWPORT, gl_saved.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, gl_saved.scissor);
    glGetIntegerv(GL_CURRENT_PROGRAM, &gl_saved.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &gl_saved.active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &gl_saved.texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &gl_saved.array_buffer);
    gl_saved.vertex_array = 0;
    if (bind_vertex_array) glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &gl_saved.vertex_array);
    glGetIntegerv(GL_BLEND_SRC_RGB, &gl_saved.blend_rgb_src);
    glGetIntegerv(GL_BLEND_DST_RGB, &gl_saved.blend_rgb_dst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &gl_saved.blend_alpha_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &gl_saved.blend_alpha_dst);
    glGetBooleanv(GL_COLOR_WRITEMASK, gl_saved.mask);
    for (i = 0; i < SAVED_CAPS; i++) gl_saved.caps[i] = glIsEnabled(saved_caps[i]);
    for (i = 0; i < SAVED_ATTRIBUTES; i++)
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &gl_saved.attributes[i]);
    glActiveTexture((GLenum)gl_saved.active);
    gl_saved.saved = 1;
}

void Shim_RestoreRendererGL(void)
{
    int i;
    if (!gl_saved.saved) return;
    gl_saved.saved = 0;
    if (bind_vertex_array) bind_vertex_array((GLuint)gl_saved.vertex_array);
    glViewport(gl_saved.viewport[0], gl_saved.viewport[1], gl_saved.viewport[2], gl_saved.viewport[3]);
    glScissor(gl_saved.scissor[0], gl_saved.scissor[1], gl_saved.scissor[2], gl_saved.scissor[3]);
    glUseProgram((GLuint)gl_saved.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)gl_saved.texture);
    glActiveTexture((GLenum)gl_saved.active);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)gl_saved.array_buffer);
    glBlendFuncSeparate((GLenum)gl_saved.blend_rgb_src, (GLenum)gl_saved.blend_rgb_dst,
                        (GLenum)gl_saved.blend_alpha_src, (GLenum)gl_saved.blend_alpha_dst);
    glColorMask(gl_saved.mask[0], gl_saved.mask[1], gl_saved.mask[2], gl_saved.mask[3]);
    for (i = 0; i < SAVED_CAPS; i++) {
        if (gl_saved.caps[i]) glEnable(saved_caps[i]);
        else glDisable(saved_caps[i]);
    }
    for (i = 0; i < SAVED_ATTRIBUTES; i++) {
        if (gl_saved.attributes[i]) glEnableVertexAttribArray((GLuint)i);
        else glDisableVertexAttribArray((GLuint)i);
    }
}
#endif
