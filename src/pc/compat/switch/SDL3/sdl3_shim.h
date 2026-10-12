/* The part of SDL3 the port uses (src/pc/platform/sdl.c, src/pc/render/
 * gl_picture.c and gl_desktop_none.c), over devkitPro's SDL2 for the Switch:
 * SDL3's own Switch port is not public. Types whose shape changed (the
 * event, the display mode, the audio spec and stream) are this shim's own;
 * calls whose arguments or result changed (SDL3 returns true on success
 * where SDL2 returned 0) go through Shim_ functions, in
 * src/pc/platform/switch_sdl3.c, which sees SDL2 as it is. Only the Switch
 * build has src/pc/compat/switch on its include path. Nothing here is a
 * general SDL3: what the port does not call is left out. */
#ifndef MEMORIES_PC_COMPAT_SWITCH_SDL3_SHIM_H
#define MEMORIES_PC_COMPAT_SWITCH_SDL3_SHIM_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* --- types whose shape changed ------------------------------------------ */

typedef Uint32 Shim_DisplayID;     /* SDL2's display index, plus 1: 0 is no display */
typedef Uint32 Shim_PropertiesID;  /* a texture's or window's: shim_properties() */

typedef struct Shim_DisplayMode {
    Shim_DisplayID displayID;
    Uint32 format;
    int w, h;
    float pixel_density;
    float refresh_rate;
    int refresh_rate_numerator, refresh_rate_denominator;
    void *internal;
} Shim_DisplayMode;

typedef struct Shim_AudioSpec {
    int format; /* SDL_AUDIO_S16 */
    int channels;
    int freq;
} Shim_AudioSpec;

typedef struct Shim_AudioStream Shim_AudioStream;
typedef void (SDLCALL *Shim_AudioStreamCallback)(void *userdata, Shim_AudioStream *stream, int additional, int total);

/* SDL3's event numbers, which sdl.c compares against and pushes. */
enum {
    SHIM_EVENT_FIRST = 0x100,
    SHIM_EVENT_QUIT = 0x100,
    SHIM_EVENT_WILL_ENTER_BACKGROUND = 0x103,
    SHIM_EVENT_DID_ENTER_BACKGROUND,
    SHIM_EVENT_WILL_ENTER_FOREGROUND,
    SHIM_EVENT_DID_ENTER_FOREGROUND,
    SHIM_EVENT_WINDOW_EXPOSED = 0x204,
    SHIM_EVENT_WINDOW_MOVED,
    SHIM_EVENT_WINDOW_RESIZED,
    SHIM_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
    SHIM_EVENT_WINDOW_MOUSE_ENTER = 0x20C,
    SHIM_EVENT_WINDOW_MOUSE_LEAVE,
    SHIM_EVENT_WINDOW_FOCUS_GAINED,
    SHIM_EVENT_WINDOW_FOCUS_LOST,
    SHIM_EVENT_WINDOW_CLOSE_REQUESTED,
    SHIM_EVENT_WINDOW_DISPLAY_CHANGED = 0x213,
    SHIM_EVENT_KEY_DOWN = 0x300,
    SHIM_EVENT_KEY_UP,
    SHIM_EVENT_TEXT_EDITING,
    SHIM_EVENT_TEXT_INPUT,
    SHIM_EVENT_KEYMAP_CHANGED,
    SHIM_EVENT_MOUSE_MOTION = 0x400,
    SHIM_EVENT_MOUSE_BUTTON_DOWN,
    SHIM_EVENT_MOUSE_BUTTON_UP,
    SHIM_EVENT_MOUSE_WHEEL,
    SHIM_EVENT_GAMEPAD_AXIS_MOTION = 0x650,
    SHIM_EVENT_GAMEPAD_BUTTON_DOWN,
    SHIM_EVENT_GAMEPAD_BUTTON_UP,
    SHIM_EVENT_GAMEPAD_ADDED,
    SHIM_EVENT_GAMEPAD_REMOVED,
    SHIM_EVENT_FINGER_DOWN = 0x700,
    SHIM_EVENT_FINGER_UP,
    SHIM_EVENT_FINGER_MOTION,
    SHIM_EVENT_FINGER_CANCELED,
    SHIM_EVENT_RENDER_DEVICE_RESET = 0x2001,
};

/* Every member starts with the same four fields, as SDL3's do, so the
 * window an event is for is in one place (Shim_GetWindowFromEvent). */
#define SHIM_EVENT_HEADER \
    Uint32 type;          \
    Uint32 reserved;      \
    Uint64 timestamp;     \
    Uint32 windowID
typedef struct { SHIM_EVENT_HEADER; } Shim_CommonEvent;
typedef struct { SHIM_EVENT_HEADER; Sint32 data1, data2; } Shim_WindowEvent;
typedef struct {
    SHIM_EVENT_HEADER;
    Uint32 which;
    SDL_Scancode scancode;
    SDL_Keycode key;
    Uint16 mod, raw;
    bool down, repeat;
} Shim_KeyboardEvent;
typedef struct { SHIM_EVENT_HEADER; const char *text; } Shim_TextInputEvent;
typedef struct { SHIM_EVENT_HEADER; Uint32 which; Uint32 state; float x, y, xrel, yrel; } Shim_MouseMotionEvent;
typedef struct {
    SHIM_EVENT_HEADER;
    Uint32 which;
    Uint8 button;
    bool down;
    Uint8 clicks;
    float x, y;
} Shim_MouseButtonEvent;
typedef struct {
    SHIM_EVENT_HEADER;
    Uint32 which;
    float x, y;
    Uint32 direction;
    float mouse_x, mouse_y;
} Shim_MouseWheelEvent;
typedef struct { SHIM_EVENT_HEADER; SDL_JoystickID which; } Shim_GamepadDeviceEvent;
typedef struct { SHIM_EVENT_HEADER; SDL_JoystickID which; Uint8 button; bool down; } Shim_GamepadButtonEvent;
typedef struct { SHIM_EVENT_HEADER; SDL_JoystickID which; Uint8 axis; Sint16 value; } Shim_GamepadAxisEvent;
typedef struct {
    SHIM_EVENT_HEADER;
    Uint64 touchID, fingerID;
    float x, y, dx, dy, pressure;
} Shim_TouchFingerEvent;
typedef union Shim_Event {
    Uint32 type;
    Shim_CommonEvent common;
    Shim_WindowEvent window;
    Shim_KeyboardEvent key;
    Shim_TextInputEvent text;
    Shim_MouseMotionEvent motion;
    Shim_MouseButtonEvent button;
    Shim_MouseWheelEvent wheel;
    Shim_GamepadDeviceEvent gdevice;
    Shim_GamepadButtonEvent gbutton;
    Shim_GamepadAxisEvent gaxis;
    Shim_TouchFingerEvent tfinger;
    Shim_CommonEvent render;
    Shim_CommonEvent quit;
    Uint8 padding[128];
} Shim_Event;
typedef bool (SDLCALL *Shim_EventFilter)(void *userdata, Shim_Event *event);

typedef struct { int flags; int buttonID; const char *text; } Shim_MessageBoxButtonData;
typedef struct {
    Uint32 flags;
    SDL_Window *window;
    const char *title, *message;
    int numbuttons;
    const Shim_MessageBoxButtonData *buttons;
    const void *colorScheme;
} Shim_MessageBoxData;

/* --- calls whose arguments or result changed ----------------------------- */

bool Shim_Init(Uint32 flags);
bool Shim_InitSubSystem(Uint32 flags);
int Shim_GetVersion(void);
Uint64 Shim_GetTicksNS(void);
bool Shim_PollEvent(Shim_Event *event);
bool Shim_PushEvent(Shim_Event *event);
bool Shim_AddEventWatch(Shim_EventFilter filter, void *userdata);
SDL_Window *Shim_GetWindowFromEvent(const Shim_Event *event);

SDL_Window *Shim_CreateWindow(const char *title, int w, int h, Uint64 flags);
bool Shim_GetWindowSize(SDL_Window *window, int *w, int *h);
bool Shim_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h);
bool Shim_SetWindowSize(SDL_Window *window, int w, int h);
bool Shim_SetWindowPosition(SDL_Window *window, int x, int y);
bool Shim_SetWindowMinimumSize(SDL_Window *window, int w, int h);
bool Shim_SetWindowBordered(SDL_Window *window, bool bordered);
bool Shim_SetWindowFullscreen(SDL_Window *window, bool fullscreen);
bool Shim_SetWindowFullscreenMode(SDL_Window *window, const Shim_DisplayMode *mode);
bool Shim_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);
bool Shim_RaiseWindow(SDL_Window *window);
bool Shim_GetWindowSafeArea(SDL_Window *window, SDL_Rect *rect);
Shim_PropertiesID Shim_GetWindowProperties(SDL_Window *window);
bool Shim_StartTextInput(SDL_Window *window);
bool Shim_StopTextInput(SDL_Window *window);
bool Shim_ShowCursor(void);
bool Shim_HideCursor(void);

Shim_DisplayID Shim_GetPrimaryDisplay(void);
Shim_DisplayID Shim_GetDisplayForWindow(SDL_Window *window);
float Shim_GetDisplayContentScale(Shim_DisplayID display);
bool Shim_GetDisplayUsableBounds(Shim_DisplayID display, SDL_Rect *rect);
const Shim_DisplayMode *Shim_GetCurrentDisplayMode(Shim_DisplayID display);
const Shim_DisplayMode *Shim_GetDesktopDisplayMode(Shim_DisplayID display);
Shim_DisplayMode **Shim_GetFullscreenDisplayModes(Shim_DisplayID display, int *count);
bool Shim_GetClosestFullscreenDisplayMode(Shim_DisplayID display, int w, int h, float refresh, bool high_density,
                                          Shim_DisplayMode *closest);

SDL_Renderer *Shim_CreateRenderer(SDL_Window *window, const char *name);
const char *Shim_GetRendererName(SDL_Renderer *renderer);
bool Shim_GetRenderOutputSize(SDL_Renderer *renderer, int *w, int *h);
bool Shim_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *source,
                        const SDL_FRect *destination);
SDL_Surface *Shim_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect);
bool Shim_SetRenderVSync(SDL_Renderer *renderer, int vsync);
bool Shim_FlushRenderer(SDL_Renderer *renderer);
bool Shim_SetTextureScaleMode(SDL_Texture *texture, int mode);
Shim_PropertiesID Shim_GetTextureProperties(SDL_Texture *texture);
Sint64 Shim_GetNumberProperty(Shim_PropertiesID properties, const char *name, Sint64 otherwise);
void *Shim_GetPointerProperty(Shim_PropertiesID properties, const char *name, void *otherwise);

SDL_Surface *Shim_CreateSurface(int w, int h, Uint32 format);
SDL_Surface *Shim_CreateSurfaceFrom(int w, int h, Uint32 format, void *pixels, int pitch);
bool Shim_AddSurfaceAlternateImage(SDL_Surface *surface, SDL_Surface *image);
bool Shim_SaveBMP(SDL_Surface *surface, const char *path);

bool Shim_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
bool Shim_GL_SetSwapInterval(int interval);
bool Shim_GL_SetAttribute(SDL_GLattr attribute, int value);
bool Shim_GL_SwapWindow(SDL_Window *window);

SDL_JoystickID *Shim_GetGamepads(int *count);
SDL_GameController *Shim_OpenGamepad(SDL_JoystickID id);
SDL_JoystickID Shim_GetGamepadID(SDL_GameController *gamepad);
bool Shim_GetGamepadButton(SDL_GameController *gamepad, int button);

Shim_AudioStream *Shim_OpenAudioDeviceStream(Uint32 device, const Shim_AudioSpec *spec,
                                             Shim_AudioStreamCallback callback, void *userdata);
bool Shim_ResumeAudioStreamDevice(Shim_AudioStream *stream);
bool Shim_PutAudioStreamData(Shim_AudioStream *stream, const void *data, int length);
int Shim_GetAudioStreamQueued(Shim_AudioStream *stream);

bool Shim_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window);
bool Shim_ShowMessageBox(const Shim_MessageBoxData *data, int *button);
bool Shim_OpenURL(const char *url);
bool Shim_SetClipboardText(const char *text);
SDL_Keycode Shim_GetKeyFromScancode(SDL_Scancode scancode, Uint16 modifiers, bool key_event);
SDL_Scancode Shim_GetScancodeFromKey(SDL_Keycode key, Uint16 *modifiers);
#endif
