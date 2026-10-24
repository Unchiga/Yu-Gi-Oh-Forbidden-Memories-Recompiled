/* SDL3 owns the window, input, controllers and audio. Presentation uses an
 * explicit OpenGL path by default, avoiding SDL_Render's per-frame texture
 * management and making the accelerated path predictable across drivers.
 * SDL_Render remains a fallback for machines where a GL context cannot be
 * created. The picture and transparent menu are two GPU textures.
 *
 * The interrupt clock (platform_common.c) is a signal on the main thread,
 * so signals are blocked while SDL creates its threads, which inherit the
 * mask. See platform.h for the contract. */
#define _GNU_SOURCE
#include "platform.h"
#include "paths.h"
#include "menu.h"
#include "pc/saves/deck_menu.h"
#include "pc/render/present_pass.h"
#include "mods_window.h"
#include "pc/mods/hd_pack.h"
#include "controls_window.h"
#include "controls_linux.h"
#include "panel.h"
#include "quit_prompt.h"
#include "host_actions.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/debug/cheats.h"
#include "pc/cards/cards.h"
#include "pc/tower/tower.h"
#include "pc/free_duel/duelists.h"
#include "pc/cards/fusion_helper.h"
#include "update_check.h"
#include "pc/debug/log.h"
#include "pc/debug/monitor.h"
#include "pc/debug/crash_test.h"
#include "pc/debug/hud.h"
#include "pc/guest/state.h"
#include "save_icon.h"
#include "touch_pad.h"
#include "touch_pad_art.h"
#include <SDL3/SDL.h>
/* Android: SDL reaches Java (JNI) for events and joysticks, message boxes,
 * the clipboard, URLs, the cursor, the window's mode and audio devices,
 * and the runtime (ART) refuses a call from a native stack it does not
 * know, as the game stack at 0xB0000000 is (a StackOverflowError, or the
 * method skipped; with CheckJNI the next call aborts). Every entry point
 * the game thread calls that may get there runs on the thread's own stack
 * (Memories_OnHostStack, state.c): the presents, which pump the events
 * and apply the display settings every frame (Platform_Present,
 * Platform_PresentPicture, Platform_PresentWidePicture), Platform_Frame,
 * Platform_PumpEvents (a frame that is not shown), Platform_StartAudio
 * (SpuInit), Platform_ShowError, Platform_OpenUrl, Platform_OpenFolder,
 * Platform_CopyText and the second windows. HERE(name) names their bodies,
 * and the wrappers at the end of this file call them; elsewhere HERE(name)
 * is the name itself. jni_guard.h checks every SDL call that may reach
 * Java against the game stack and logs one that runs there.
 * MEMORIES_TEST_JAVA=<frame>[box|guard] makes such calls from inside the
 * game loop, in Platform_Frame and in the present's pump (java_test), and
 * with "guard" calls the guard on the game stack once, to show it fires. */
#ifdef SDL_PLATFORM_ANDROID
#define HERE(name) name##_here
#else
#define HERE(name) name
#endif
#include <SDL3/SDL_opengl.h>
#include "pc/render/gl_picture.h"
#include "pc/render/texture_pack.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "pc/compat/posix.h"
#include "jni_guard.h" /* last: after SDL's own headers */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture, *overlay;
static SDL_GLContext gl_context;
static GLuint gl_picture, gl_overlay;
static int gl_pass_shown, gl_pass_rect[4];
/* A widened picture shown instead of the pass's own (0 for that one), and
 * its size in texels. */
static GLuint gl_pass_texture;
static int gl_pass_size[2]; /* this frame shows the OpenGL picture pass's texture, this part of it */
static int use_gl;
/* OpenGL ES 3 (Android; MEMORIES_GLES=1 on a desktop, which tries that path
 * there): no context of the game's own and no desktop presenter (use_gl 0,
 * so neither glBegin nor present_pass.c runs). SDL's renderer (opengles2)
 * presents, in an ES 3.0 context asked of it, and the OpenGL picture pass
 * (gl_picture.c) draws in that context between the renderer's batches
 * (es_enter, es_leave). The pass's frame is shown through a texture of
 * SDL's own, es_shown, which the pass copies the shown area into. */
static int es_wanted;           /* ask for an ES 3.0 context for the renderer */
static SDL_GLContext es_context; /* the renderer's, when it is ES 3 */
static int es_picture;          /* the pass is on in it */
static SDL_Texture *es_shown;
static int es_shown_w, es_shown_h, es_shown_last; /* last: the window's last frame was the pass's */
static PFNGLBINDFRAMEBUFFERPROC es_bind_framebuffer;
static GLint es_framebuffer;
/* The OpenGL picture pass has a context: the desktop's own or the renderer's. */
static int gl_pass(void) { return use_gl || es_picture; }
static int picture_w, picture_h;
static uint32_t *picture_pixels, *overlay_pixels;
static int picture_scale = 1; /* picture pixels per game pixel (internal resolution) */
static int logical_w, logical_h;  /* the frame in game pixels: what the window is laid out for */
static MenuCanvas canvas;
/* Menu/input geometry is in SDL window coordinates (logical pixels). The
 * renderer target may have more physical pixels on a high-DPI display. */
static struct { int win_w, win_h; float pixel_x, pixel_y; SDL_FRect dst; } layout;
static int menu_visible = 1;
static int display_settings_pending;
static int menu_reveal_frames;
/* 4 puts the 320x240 picture on screen at 1280x960. */
static int scale = 4, pending_scale, quit, state_slot = 1;
static struct { int x, y, w, h; } shown_menu; /* the menu's bounds as last painted */
static volatile uint16_t scripted_bits2, scripted_bits, mouse_bits;
static uint16_t wheel_bits;
static int wheel_frames;
static volatile uint16_t wheel_now;
/* The on-screen controller's bits (touch_pad.h), and whether the mouse
 * button SDL made of a touch went down on the pad (its events are the pad's
 * until it comes up). */
static volatile uint16_t touch_bits;
static int touch_mouse_on_pad;
static int pointer_x, pointer_y, pointer_inside, cursor_hidden;
/* The pointer's place is a mouse's (it hovers): the mouse events SDL makes
 * of a touch move it only while the finger is down, and leave it where the
 * finger lifted, so they never reveal the bar by hovering. */
#ifdef SDL_PLATFORM_ANDROID
static int pointer_is_mouse;
#else
static int pointer_is_mouse = 1;
#endif
static unsigned last_pointer_motion, current_frame;
static int focus_clock_rate = 100, focus_paused;
/* A phone or tablet app sent to the background (SDL's application events,
 * which desktops never send): the game clock stops until it comes back. */
static int background_clock_rate, background_paused;
/* Android: until when (real_now_us) a still picture is repainted after
 * the app comes back (repaint_menu, below). */
static uint64_t foreground_repaint_until;
static float known_refresh; /* what the wayland driver reported before a fallback to x11 */

static void show(void);
static void repaint_menu(void);
static void reset_renderer(void);
/* MEMORIES_TEST_GL_RESET=<frame>[fail] (reset_renderer): the frame, and
 * whether the pass is then made not to start again. */
static long test_reset_at = -2;
static int test_reset_fails;
/* MEMORIES_TEST_GL_COPY_FAIL=<frame> (es_take_frame): the frame whose copy
 * into es_shown fails, as when SDL cannot make that texture. */
static long test_copy_fail_at = -2;
/* Menu changes from events are coalesced: a pointer sweeping the bar
 * reports hundreds of motions a second, and each used to repaint and
 * present a frame. Now they mark the menu dirty and it is repainted once,
 * with the next game frame or at the end of an idle pump. */
static int menu_dirty;
static void save_window_image(void);
static int window_shot_pending;
/* The picture the last present showed from the OpenGL pass's textures
 * (valid), which F12 reads back when asked, and not per frame: what area of
 * the picture, or of which widescreen target's texture (0: the picture's).
 * Cleared when a frame is shown from picture_pixels instead. */
static struct { int valid; unsigned texture; int x, y, w, h; } pass_shown;

/* Window pixels per density-independent pixel where the screen is touched
 * (Android: the display's densityDpi / 160, as the system has it now; SDL's
 * content scale is the density at the start, and stays so after Display
 * size changes it: pump() reads it again and lays out anew), 0 on a
 * desktop, which keeps the mouse's sizes. */
static float touch_density(void)
{
#ifdef SDL_PLATFORM_ANDROID
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    float density = Android_Density();
    if (density <= 0.0f) density = display ? SDL_GetDisplayContentScale(display) : 0.0f;
    return density > 0.0f ? density : 1.0f;
#else
    return 0.0f;
#endif
}

/* The menu's size: the setting, or from the height the window has or is
 * about to have. */
static void update_menu_scale(int window_h)
{
    int wanted = Settings_Get(SET_MENU_SCALE);
    float density = touch_density();
    if (density > 0.0f) {
        /* The screen is touched: Automatic sets the text at the system's
         * size (13 px at 1 dp, about 14 sp), the bar, rows and buttons a
         * finger's target (48 dp) tall, and the touch controls a thumb's
         * width, from the density rather than the window. */
        if (!wanted) wanted = density >= 4.0f ? 4 : density < 1.0f ? 1 : (int)(density + 0.5f);
        Menu_SetTouchTarget((int)(48.0f * density + 0.5f));
        if (TouchPad_SetDensity(density)) menu_dirty = 1;
    }
    Menu_SetScale(wanted ? wanted : Menu_AutoScale(window_h));
}

/* Where the window is always the whole screen (a phone), the bar is drawn
 * over the picture when it shows: the picture and the touch controls stay
 * where they are. In a desktop's fullscreen it pushes the picture down. */
static int bar_overlays(void) { return !Platform_HasWindowModes(); }
static void relayout(void);
static void show_cursor(void);
static void block_signals(sigset_t *previous);
static void restore_signals(const sigset_t *previous);

/* SDL_Render may leave its own OpenGL context current. Game texture uploads
 * must never inherit that context (or its pixel unpack stride). */
static void restore_game_context(void)
{
    if (use_gl && !SDL_GL_MakeCurrent(window, gl_context)) {
        fprintf(stderr, "memories-pc: restoring game OpenGL context: %s\n", SDL_GetError());
        quit = 1;
    }
}

/* OpenGL ES: around every call into gl_picture.c. The renderer's queued
 * drawing goes out first, and SDL_FlushRenderer also marks the state SDL
 * keeps (program, textures, viewport, blending, scissor) as unknown, so its
 * next batch sets it again. What SDL sets once and relies on is put back
 * after: the framebuffer it draws into and its pixel alignments (1). */
static void es_enter(void)
{
    if (!es_context) return;
    SDL_FlushRenderer(renderer);
    if (SDL_GL_GetCurrentContext() != es_context) SDL_GL_MakeCurrent(window, es_context);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &es_framebuffer);
}

static void es_leave(void)
{
    if (!es_context) return;
    es_bind_framebuffer(GL_FRAMEBUFFER, (GLuint)es_framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}
static SDL_Window *mods_window;
static SDL_Renderer *mods_renderer;
static SDL_Texture *mods_texture;
static MenuCanvas mods_canvas;
static int mods_dirty; /* drawn once after the events, not per event */
static void close_mods(void)
{
    if (mods_texture) SDL_DestroyTexture(mods_texture);
    if (mods_renderer) SDL_DestroyRenderer(mods_renderer);
    if (mods_window) SDL_DestroyWindow(mods_window);
    free(mods_canvas.pixels);
    mods_texture = NULL; mods_renderer = NULL; mods_window = NULL;
    mods_canvas.pixels = NULL;
    restore_game_context();
}
static void draw_mods(void)
{
    ModsWindow_Draw(&mods_canvas);
    SDL_UpdateTexture(mods_texture, NULL, mods_canvas.pixels, mods_canvas.stride * 4);
    SDL_RenderClear(mods_renderer);
    SDL_RenderTexture(mods_renderer, mods_texture, NULL, NULL);
    SDL_RenderPresent(mods_renderer);
    restore_game_context();
}
static void resize_mods(int w, int h)
{
    SDL_Texture *texture;
    uint32_t *pixels;
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return;
    pixels = calloc((size_t)w * h, 4);
    texture = SDL_CreateTexture(mods_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    restore_game_context();
    if (!pixels || !texture) { free(pixels); if (texture) SDL_DestroyTexture(texture); restore_game_context(); return; }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_DestroyTexture(mods_texture); free(mods_canvas.pixels);
    mods_texture = texture; mods_canvas.pixels = pixels;
    mods_canvas.width = mods_canvas.stride = w; mods_canvas.height = h;
    ModsWindow_Resize(w, h);
    restore_game_context();
}
int HERE(Platform_OpenFolder)(const char *path)
{
#ifdef SDL_PLATFORM_ANDROID
    /* An app has no file manager window to open: SDL_OpenURL there takes a
     * URI for an activity, and a plain path opens nothing. */
    (void)path;
    return -1;
#endif
    /* xdg-open on Linux, ShellExecute on Windows: both take a plain path. */
#ifdef _WIN32
    char native[1024];
    if ((size_t)snprintf(native, sizeof(native), "%s", path) >= sizeof(native)) return -1;
    for (char *c = native; *c; c++) if (*c == '/') *c = '\\'; /* Explorer wants backslashes */
    path = native;
#endif
    return SDL_OpenURL(path) ? 0 : -1;
}
int HERE(Platform_OpenUrl)(const char *url) { return SDL_OpenURL(url) ? 0 : -1; }
static int panel_overlay(void);
static void open_panel(int kind);
void HERE(Platform_OpenMods)(void)
{
    sigset_t previous;
    if (panel_overlay()) { open_panel(PANEL_MODS); return; }
    if (mods_window) { SDL_RaiseWindow(mods_window); return; }
    ModsWindow_Init();
    ModsWindow_Size(&mods_canvas.width, &mods_canvas.height);
    {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
            if (mods_canvas.width > bounds.w - 40) mods_canvas.width = bounds.w - 40;
            if (mods_canvas.height > bounds.h - 60) mods_canvas.height = bounds.h - 60;
            ModsWindow_Resize(mods_canvas.width, mods_canvas.height);
        }
    }
    mods_canvas.stride = mods_canvas.width;
    mods_canvas.pixels = calloc((size_t)mods_canvas.width * mods_canvas.height, 4);
    /* Driver worker threads must inherit the blocked game timer signals. */
    block_signals(&previous);
    mods_window = SDL_CreateWindow("Mods", mods_canvas.width, mods_canvas.height, SDL_WINDOW_RESIZABLE);
    mods_renderer = mods_window ? SDL_CreateRenderer(mods_window, NULL) : NULL;
    mods_texture = mods_renderer ? SDL_CreateTexture(mods_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, mods_canvas.width, mods_canvas.height) : NULL;
    restore_signals(&previous);
    /* The canvas is opaque; SDL would otherwise blend an ARGB texture by alpha. */
    if (mods_texture) SDL_SetTextureBlendMode(mods_texture, SDL_BLENDMODE_NONE);
    if (!mods_canvas.pixels || !mods_texture) {
        Monitor_Modal(1);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MODS", "Could not open the mods window.", window);
        Monitor_Modal(0);
        close_mods(); return;
    }
    /* Never above the opened size: growing the window here would stretch the
     * texture made for the canvas until the resize event arrives. */
    SDL_SetWindowMinimumSize(mods_window, mods_canvas.width < 620 ? mods_canvas.width : 620,
                             mods_canvas.height < 480 ? mods_canvas.height : 480);
    SDL_StartTextInput(mods_window);
    show_cursor();
    draw_mods();
}

static void controls_key_labels(void);
static void controls_sync_keys(void);
static SDL_Window *controls_window;
static SDL_Renderer *controls_renderer;
static SDL_Texture *controls_texture;
static MenuCanvas controls_canvas;
static void close_controls(void)
{
    if (controls_texture)
        SDL_DestroyTexture(controls_texture);
    if (controls_renderer)
        SDL_DestroyRenderer(controls_renderer);
    if (controls_window)
        SDL_DestroyWindow(controls_window);
    free(controls_canvas.pixels);
    memset(&controls_canvas, 0, sizeof(controls_canvas));
    controls_window = NULL;
    controls_renderer = NULL;
    controls_texture = NULL;
    mouse_bits = wheel_now = 0;
    wheel_frames = 0;
    ControlsRuntime_Block(0);
    restore_game_context();
}
static void draw_controls(void)
{
    ControlsWindow_Draw(&controls_canvas);
    SDL_UpdateTexture(controls_texture,NULL,controls_canvas.pixels,controls_canvas.stride*4);
    SDL_RenderClear(controls_renderer);SDL_RenderTexture(controls_renderer,controls_texture,NULL,NULL);
    SDL_RenderPresent(controls_renderer);restore_game_context();
}
/* The window is resizable: rebuild the canvas and texture for the new size
 * and repaint, keeping the old ones when the new pair cannot be made. */
static void resize_controls(int w, int h)
{
    sigset_t previous;
    if(!controls_window||w<1||h<1||(w==controls_canvas.width&&h==controls_canvas.height))return;
    uint32_t *pixels=calloc((size_t)w*h,4);
    block_signals(&previous);
    SDL_Texture *texture=pixels?SDL_CreateTexture(controls_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h):NULL;
    restore_signals(&previous);
    if(!texture){free(pixels);return;}
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
    block_signals(&previous);
    SDL_DestroyTexture(controls_texture);
    restore_signals(&previous);
    free(controls_canvas.pixels);
    controls_texture=texture;
    controls_canvas.pixels=pixels;
    controls_canvas.width=controls_canvas.stride=w;
    controls_canvas.height=h;
    draw_controls();
}
void HERE(Platform_OpenControls)(void)
{
    sigset_t previous;
    if (panel_overlay()) { open_panel(PANEL_CONTROLS); return; }
    if(controls_window){SDL_RaiseWindow(controls_window);return;}
    controls_key_labels();
    ControlsWindow_Init();controls_sync_keys();ControlsWindow_Size(&controls_canvas.width,&controls_canvas.height);
    SDL_Rect usable;
    if(SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window),&usable)) {
        if(controls_canvas.width>usable.w)controls_canvas.width=usable.w;
        if(controls_canvas.height>usable.h-50)controls_canvas.height=usable.h-50;
    }
    controls_canvas.stride=controls_canvas.width;
    controls_canvas.pixels=calloc((size_t)controls_canvas.width*controls_canvas.height,4);
    block_signals(&previous);
    controls_window=SDL_CreateWindow("Controls",controls_canvas.width,controls_canvas.height,SDL_WINDOW_RESIZABLE);
    controls_renderer=controls_window?SDL_CreateRenderer(controls_window,NULL):NULL;
    controls_texture=controls_renderer?SDL_CreateTexture(controls_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,controls_canvas.width,controls_canvas.height):NULL;
    restore_signals(&previous);
    if(!controls_texture||!controls_canvas.pixels) {close_controls();return;}
    SDL_SetTextureBlendMode(controls_texture,SDL_BLENDMODE_NONE);
    int min_w,min_h;
    ControlsWindow_MinSize(&min_w,&min_h);
    SDL_SetWindowMinimumSize(controls_window,min_w,min_h);
    mouse_bits=wheel_now=0;wheel_frames=0;show_cursor();draw_controls();
}

/* --- Mods and Controls as panels inside the window (panel.h) --------- */

/* Where a second window cannot open (Android), Mods and Controls are
 * panels inside this one; MEMORIES_PANELS=overlay shows them so on a
 * desktop too, for testing. The game is paused while one shows (its picture
 * is covered and the panel has the input) and goes on at its speed after. */
static int panel_paused, panel_clock_rate, panel_typing;
static int panel_overlay(void)
{
#ifdef SDL_PLATFORM_ANDROID
    return 1;
#else
    const char *wanted = getenv("MEMORIES_PANELS");
    return wanted && !strcmp(wanted, "overlay");
#endif
}
/* The window's size, and the part clear of a phone's cutouts: across, the
 * safe area; down, the whole height, since the system bars it also leaves
 * out are hidden over the game (a fullscreen, immersive window) and a
 * landscape phone's cutout is at a side. */
static void layout_panel(void)
{
    SDL_Rect safe;
    static SDL_Rect logged;
    if (window && SDL_GetWindowSafeArea(window, &safe)) {
        if (memcmp(&safe, &logged, sizeof(safe))) {
            logged = safe;
            LOG(LOG_WINDOW, "panel: safe area %d,%d %dx%d of %dx%d", safe.x, safe.y, safe.w, safe.h, layout.win_w,
                layout.win_h);
        }
        Panel_Layout(layout.win_w, layout.win_h, safe.x, 0, safe.w, layout.win_h);
    } else
        Panel_Layout(layout.win_w, layout.win_h, 0, 0, layout.win_w, layout.win_h);
}
static void open_panel(int kind)
{
    if (kind == PANEL_CONTROLS) controls_key_labels();
#ifdef SDL_PLATFORM_ANDROID
    /* An app has no mods folder the player can open: Import mod... takes a
     * mod's .zip through the system's file picker instead. */
    if (kind == PANEL_MODS) {
        /* and HD pack... downloads the release's HD pack into it */
        HdPack_SetNet(Platform_HdNet());
        ModsWindow_SetImport(Platform_PickModZip, Platform_PickedModZip, Platform_FetchModZip);
    }
#endif
    if (!Panel_Open(kind)) return;
    if (kind == PANEL_CONTROLS) controls_sync_keys();
    layout_panel();
    if (!panel_paused) {
        panel_paused = 1;
        /* Turbo ends with the panel (its key is let go behind it), so
         * closing it resumes the chosen speed, not 400%, as focus loss does. */
        panel_clock_rate = Platform_ClockRate() && ControlsRuntime_HostHeld() >> CTRL_HOST_TURBO & 1
                               ? Settings_Get(SET_SPEED)
                               : Platform_ClockRate();
        if (Platform_ClockRate()) Platform_SetClockRate(0);
    }
    mouse_bits = wheel_now = 0;
    wheel_frames = 0;
    menu_dirty = 1;
    LOG(LOG_WINDOW, "panel %s shown in the window, game paused (was %d%%)", kind == PANEL_MODS ? "Mods" : "Controls",
        panel_clock_rate);
}
/* After every event the panel took: the on-screen keyboard follows the
 * focused field, and a panel that closed gives the game back. */
static void panel_after(int tapped)
{
    int typing = Panel_TextFocus();
    if (typing && (!panel_typing || tapped)) SDL_StartTextInput(window); /* a tap on the field shows it again */
    else if (!typing && panel_typing) SDL_StopTextInput(window);
    panel_typing = typing;
    if (Panel_Shown()) return;
    if (panel_paused) {
        panel_paused = 0;
        if (panel_clock_rate && Platform_ClockRate() == 0 && !background_paused) Platform_SetClockRate(panel_clock_rate);
    }
    mouse_bits = wheel_now = 0;
    wheel_frames = 0;
    ControlsRuntime_ResetKeys();
    menu_dirty = 1;
    LOG(LOG_WINDOW, "panel closed, game at %d%%", Platform_ClockRate());
}

static void pump(void);

#ifdef SDL_PLATFORM_ANDROID
/* MEMORIES_TEST_JAVA=<frame>, or <frame>box: at that frame, from inside
 * the game loop, the Java calls a player's input or a menu can make (the
 * cursor hidden and shown, the clipboard; with "box" a message box too),
 * and where they ran. On a phone, before they ran on the thread's stack,
 * the runtime refused them (logcat: StackOverflowError, or a CheckJNI
 * abort on the next call). */
static long java_test_at = -2;
static int java_test_box, java_test_guard;

static int java_test_now(unsigned frame)
{
    if (java_test_at == -2) {
        const char *value = getenv("MEMORIES_TEST_JAVA");
        char *end = NULL;
        java_test_at = value && *value ? strtol(value, &end, 10) : -1;
        java_test_box = end && !strcmp(end, "box");
        java_test_guard = end && !strcmp(end, "guard");
    }
    return java_test_at >= 0 && frame == (unsigned)java_test_at;
}

static void java_test(unsigned frame)
{
    int hid, shown, copied, boxed = -1;
    int box;
    if (!java_test_now(frame)) return;
    box = java_test_box;
    hid = SDL_HideCursor();
    shown = SDL_ShowCursor();
    if (cursor_hidden) SDL_HideCursor();
    copied = SDL_SetClipboardText("memories-pc: Java test");
    if (box) boxed = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Java test",
                                              "A message box from inside the game loop.", window);
    fprintf(stderr, "memories-pc: java test at frame %u: cursor hide %d show %d, clipboard %d, box %d; "
            "native stack %p (the game stack is 0xb0000000-0xb0800000); on the game stack so far: %u\n", frame,
            hid, shown, copied, boxed, __builtin_frame_address(0), Memories_JniGuardCount());
}

/* The same at the present's pump (begin_present): the joysticks polled
 * (Android_JNI_PollInputDevices), the display settings applied as View >
 * Menu size or File > Reload settings do (SDL_SetWindowFullscreen), and
 * the cursor shown and hidden as on focus loss. Before the presents ran on
 * the thread's stack, these ran on the game stack every frame. */
static void apply_display_settings(void);
static void java_test_pump(unsigned frame)
{
    int hid, shown;
    if (!java_test_now(frame)) return;
    SDL_UpdateJoysticks();
    apply_display_settings();
    hid = SDL_HideCursor();
    shown = SDL_ShowCursor();
    if (cursor_hidden) SDL_HideCursor();
    fprintf(stderr, "memories-pc: java test (pump) at frame %u: joysticks polled, display settings applied, "
            "cursor hide %d show %d; native stack %p; on the game stack so far: %u\n", frame, hid, shown,
            __builtin_frame_address(0), Memories_JniGuardCount());
}
#endif

static void show_cursor(void)
{
    if (cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = 0;
    }
}

static uint64_t real_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* MEMORIES_TRACE=frames (Debug > Trace > Frames; logcat on Android): where
 * a shown frame's present goes, over 120 of them, beside libetc.c's game
 * and present and gl_picture.c's replay: the events and the menu (pump),
 * the picture into the window's texture (the OpenGL picture's copy, or the
 * software picture's upload), and the swap, which waits while the GPU is
 * behind, or for the display with vsync on. Untimed while the channel is
 * off. */
enum { PHASE_EVENTS, PHASE_COPY, PHASE_SWAP, PHASES };
static struct { uint64_t total[PHASES]; unsigned max[PHASES], frames; } phases;

static uint64_t phase_start(void) { return Log_Enabled(LOG_FRAMES) ? real_now_us() : 0; }

static void phase_end(int phase, uint64_t started)
{
    unsigned us;
    if (!started) return;
    us = (unsigned)(real_now_us() - started);
    phases.total[phase] += us;
    if (us > phases.max[phase]) phases.max[phase] = us;
}

static void phases_shown(void)
{
    static int counting; /* the channel was on at the last frame */
    unsigned images, image_us, image_max;
    if (!Log_Enabled(LOG_FRAMES)) {
        counting = 0;
        return;
    }
    if (!counting) { /* turned on just now: count from here, not from the launch */
        counting = 1;
        memset(&phases, 0, sizeof(phases));
        TexturePack_ReadStats(&images, &image_us, &image_max);
        return;
    }
    if (++phases.frames < 120) return;
    TexturePack_ReadStats(&images, &image_us, &image_max);
    LOG(LOG_FRAMES, "present phases per shown frame: events %u us, copy %u us, swap %u us (max %u, %u, %u); "
        "texture pack: %u images read in %u us (longest %u us)",
        (unsigned)(phases.total[PHASE_EVENTS] / 120), (unsigned)(phases.total[PHASE_COPY] / 120),
        (unsigned)(phases.total[PHASE_SWAP] / 120), phases.max[PHASE_EVENTS], phases.max[PHASE_COPY],
        phases.max[PHASE_SWAP], images, image_us, image_max);
    memset(&phases, 0, sizeof(phases));
}

/* XWayland reports no refresh rate for the current or desktop mode, but its
 * fullscreen mode list has them: take the fastest at the desktop's size.
 * Unknown (0) makes the "display refresh" cap show every game frame and
 * lets vsync pace the game only at 100% or slower. */
static void update_display_refresh(void)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    float refresh = mode ? mode->refresh_rate : 0.0f;
    if (refresh <= 0.0f && desktop) refresh = desktop->refresh_rate;
    if (refresh <= 0.0f) refresh = known_refresh;
    if (refresh <= 0.0f && desktop) {
        int count = 0, i;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
        for (i = 0; modes && i < count; i++) {
            if (modes[i]->w == desktop->w && modes[i]->h == desktop->h && modes[i]->refresh_rate > refresh) {
                refresh = modes[i]->refresh_rate;
            }
        }
        SDL_free(modes);
    }
    Platform_SetPresentRefresh(refresh);
    LOG(LOG_WINDOW, "display refresh %.2f Hz", refresh);
}

/* A blocking vsync present may only pace the game while game frames come no
 * faster than the display refreshes; faster than that it would hold the
 * game to the refresh rate, so presents go out unsynced and the frame-rate
 * cap alone limits them. Re-checked at every present: the speed can change
 * at any time (Tab turbo). */
static int swap_interval = -1;

static void apply_swap_interval(void)
{
    int wanted = Settings_Get(SET_VSYNC) && Platform_VSyncPacesGame() ? 1 : 0;
    if (wanted == swap_interval) return;
    swap_interval = wanted;
    if (use_gl) SDL_GL_SetSwapInterval(wanted);
    else if (renderer) SDL_SetRenderVSync(renderer, wanted);
    Platform_SetVSync(wanted);
    LOG(LOG_WINDOW, "vsync %s (game %.2f Hz, display %.2f Hz)", wanted ? "on" : "off",
        Platform_GameHz(), Platform_PresentRefresh());
}

/* Fullscreen or borderless fullscreen: either way the menu bar hides until
 * the pointer reaches the top. */
static int covers_screen(void) { return Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_BORDERLESS); }

static void update_menu_visibility(void)
{
    int wanted = !covers_screen() || Settings_Get(SET_SHOW_MENU_FULLSCREEN) ||
                 (pointer_inside && pointer_is_mouse && pointer_y < Menu_Height()) || Menu_IsOpen() ||
                 menu_reveal_frames > 0;
    if (wanted == menu_visible) return;
    menu_visible = wanted;
    Menu_SetVisible(wanted);
    relayout();
    menu_dirty = 1;
}

/* A screenshot that could not be written: where and why (paths.h), on
 * stderr and in a notice, since F12 is otherwise silent. */
static void screenshot_not_saved(const char *why)
{
    static const char *const ok[] = {"OK"};
    char text[1300];
    snprintf(text, sizeof(text), "Could not save the screenshot to %s", why);
    fprintf(stderr, "memories-pc: %s\n", text);
    Menu_ShowNotice("Screenshot not saved", text, ok, 1, 0, NULL);
}

static int screenshot_path(char *path, size_t size, const char *extension)
{
    const char *directory = getenv("MEMORIES_SCREENSHOT_DIR");
    time_t now = time(NULL);
    struct tm local;
    char stamp[32];
    char user[1024];
    if (!directory || !*directory) {
        if (Paths_User(user, sizeof(user), "screenshots")) return 0;
        directory = user;
    }
    Paths_WriteBegin();
    if (Paths_MakeDirs(directory)) { /* mkdir's own reason, not the file's "path not found" */
        char why[1200];
        screenshot_not_saved(Paths_WriteError(why, sizeof(why), directory));
        return 0;
    }
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
    return snprintf(path, size, "%s/%s-%u.%s", directory, stamp, current_frame, extension) < (int)size;
}

static void save_surface(SDL_Surface *surface, const char *path)
{
    if (!surface) {
        fprintf(stderr, "memories-pc: screenshot failed: %s\n", SDL_GetError());
        return;
    }
    {
        /* Opened here first, as every other file the port writes is, so a
         * folder that refuses it says where and why (paths.h); SDL then
         * writes over the empty file. */
        char why[1200];
        FILE *file;
        Paths_WriteBegin();
        file = fopen(path, "wb");
        if (!file || fclose(file)) {
            screenshot_not_saved(Paths_WriteError(why, sizeof(why), path));
        } else if (SDL_SaveBMP(surface, path)) {
            fprintf(stderr, "memories-pc: screenshot: %s\n", path);
            Paths_WriteDone(path);
        } else {
            snprintf(why, sizeof(why), "%s: %s", path, SDL_GetError());
            remove(path);
            screenshot_not_saved(why);
        }
    }
    SDL_DestroySurface(surface);
}

/* GL: read the composed frame from the back buffer, before it is swapped. */
static void save_window_image(void)
{
    SDL_Surface *surface;
    char path[1024];
    int w, h, y;
    uint32_t *pixels, *row;
    if (!screenshot_path(path, sizeof(path), "bmp") || !SDL_GetWindowSizeInPixels(window, &w, &h)) return;
    surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        pixels = surface->pixels;
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        row = malloc((size_t)surface->pitch);
        if (row) {
            for (y = 0; y < h / 2; y++) {
                uint8_t *a = (uint8_t *)surface->pixels + (size_t)y * surface->pitch;
                uint8_t *b = (uint8_t *)surface->pixels + (size_t)(h - 1 - y) * surface->pitch;
                memcpy(row, a, (size_t)surface->pitch);
                memcpy(a, b, (size_t)surface->pitch);
                memcpy(b, row, (size_t)surface->pitch);
            }
            free(row);
        }
    }
    save_surface(surface, path);
}

int HERE(Platform_CopyText)(const char *text) { return SDL_SetClipboardText(text); }

/* F12 when the last present showed the OpenGL pass's picture: it lives in
 * GPU textures, and picture_pixels holds nothing of it. */
static void save_pass_picture(void)
{
    SDL_Surface *surface;
    char path[1024];
    uint32_t *pixels = malloc((size_t)pass_shown.w * (size_t)pass_shown.h * 4);
    int read;
    if (!pixels) return;
    es_enter();
    read = GlPicture_ReadShown(pass_shown.texture, pass_shown.x, pass_shown.y, pass_shown.w, pass_shown.h, pixels);
    es_leave();
    if (!read) {
        fprintf(stderr, "memories-pc: screenshot: the OpenGL picture shown is gone\n");
    } else if (screenshot_path(path, sizeof(path), "bmp")) {
        surface = SDL_CreateSurfaceFrom(pass_shown.w, pass_shown.h, SDL_PIXELFORMAT_XRGB8888, pixels, pass_shown.w * 4);
        save_surface(surface, path); /* the surface does not own the pixels */
    }
    free(pixels);
}

void Platform_Screenshot(int window_image)
{
    SDL_Surface *surface;
    char path[1024];
    if ((!renderer && !use_gl) || !picture_pixels) return;
    if (window_image) {
        /* Taken by the next show(), before its swap or present: after a
         * present the renderer's back buffer holds nothing defined. */
        window_shot_pending = 1;
        return;
    }
    if (pass_shown.valid) {
        save_pass_picture();
        return;
    }
    if (!screenshot_path(path, sizeof(path), "bmp")) return;
    surface = SDL_CreateSurfaceFrom(picture_w, picture_h, SDL_PIXELFORMAT_XRGB8888, picture_pixels, picture_w * 4);
    save_surface(surface, path);
}

/* Arrows d-pad; X cross, S circle, Z square, A triangle; Q/W L1/R1, E/R
 * L2/R2, T/Y L3/R3; Enter start, right Shift select. */
static int controls_key(SDL_Scancode code)
{
    switch (code) {
    case SDL_SCANCODE_CAPSLOCK:
        return CTRL_KEY_CAPS_LOCK;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return CTRL_KEY_NUM_LOCK;
    case SDL_SCANCODE_PRINTSCREEN:
        return CTRL_KEY_PRINT_SCREEN;
    case SDL_SCANCODE_SCROLLLOCK:
        return CTRL_KEY_SCROLL_LOCK;
    case SDL_SCANCODE_PAUSE:
        return CTRL_KEY_PAUSE;
    case SDL_SCANCODE_SPACE:
        return CTRL_KEY_SPACE;
    case SDL_SCANCODE_ESCAPE:
        return CTRL_KEY_ESCAPE;
    case SDL_SCANCODE_TAB:
        return CTRL_KEY_TAB;
    case SDL_SCANCODE_BACKSPACE:
        return CTRL_KEY_BACKSPACE;
    case SDL_SCANCODE_RETURN:
        return CTRL_KEY_ENTER;
    case SDL_SCANCODE_MINUS:
        return CTRL_KEY_MINUS;
    case SDL_SCANCODE_EQUALS:
        return CTRL_KEY_EQUAL;
    case SDL_SCANCODE_LEFTBRACKET:
        return CTRL_KEY_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET:
        return CTRL_KEY_RBRACKET;
    case SDL_SCANCODE_BACKSLASH:
        return CTRL_KEY_BACKSLASH;
    case SDL_SCANCODE_SEMICOLON:
        return CTRL_KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE:
        return CTRL_KEY_APOSTROPHE;
    case SDL_SCANCODE_GRAVE:
        return CTRL_KEY_GRAVE;
    case SDL_SCANCODE_COMMA:
        return CTRL_KEY_COMMA;
    case SDL_SCANCODE_PERIOD:
        return CTRL_KEY_PERIOD;
    case SDL_SCANCODE_SLASH:
        return CTRL_KEY_SLASH;
    case SDL_SCANCODE_UP:
        return CTRL_KEY_ARROW_UP;
    case SDL_SCANCODE_DOWN:
        return CTRL_KEY_ARROW_DOWN;
    case SDL_SCANCODE_LEFT:
        return CTRL_KEY_ARROW_LEFT;
    case SDL_SCANCODE_RIGHT:
        return CTRL_KEY_ARROW_RIGHT;
    case SDL_SCANCODE_INSERT:
        return CTRL_KEY_INSERT;
    case SDL_SCANCODE_HOME:
        return CTRL_KEY_HOME;
    case SDL_SCANCODE_END:
        return CTRL_KEY_END;
    case SDL_SCANCODE_PAGEUP:
        return CTRL_KEY_PAGE_UP;
    case SDL_SCANCODE_PAGEDOWN:
        return CTRL_KEY_PAGE_DOWN;
    case SDL_SCANCODE_DELETE:
        return CTRL_KEY_DELETE;
    case SDL_SCANCODE_LSHIFT:
        return CTRL_KEY_LEFT_SHIFT;
    case SDL_SCANCODE_RSHIFT:
        return CTRL_KEY_RIGHT_SHIFT;
    case SDL_SCANCODE_LCTRL:
        return CTRL_KEY_LEFT_CTRL;
    case SDL_SCANCODE_RCTRL:
        return CTRL_KEY_RIGHT_CTRL;
    case SDL_SCANCODE_LALT:
        return CTRL_KEY_LEFT_ALT;
    case SDL_SCANCODE_RALT:
        return CTRL_KEY_RIGHT_ALT;
    case SDL_SCANCODE_LGUI:
        return CTRL_KEY_LEFT_SUPER;
    case SDL_SCANCODE_RGUI:
        return CTRL_KEY_RIGHT_SUPER;
    case SDL_SCANCODE_A:
        return CTRL_KEY_A;
    case SDL_SCANCODE_B:
        return CTRL_KEY_B;
    case SDL_SCANCODE_C:
        return CTRL_KEY_C;
    case SDL_SCANCODE_D:
        return CTRL_KEY_D;
    case SDL_SCANCODE_E:
        return CTRL_KEY_E;
    case SDL_SCANCODE_F:
        return CTRL_KEY_F;
    case SDL_SCANCODE_G:
        return CTRL_KEY_G;
    case SDL_SCANCODE_H:
        return CTRL_KEY_H;
    case SDL_SCANCODE_I:
        return CTRL_KEY_I;
    case SDL_SCANCODE_J:
        return CTRL_KEY_J;
    case SDL_SCANCODE_K:
        return CTRL_KEY_K;
    case SDL_SCANCODE_L:
        return CTRL_KEY_L;
    case SDL_SCANCODE_M:
        return CTRL_KEY_M;
    case SDL_SCANCODE_N:
        return CTRL_KEY_N;
    case SDL_SCANCODE_O:
        return CTRL_KEY_O;
    case SDL_SCANCODE_P:
        return CTRL_KEY_P;
    case SDL_SCANCODE_Q:
        return CTRL_KEY_Q;
    case SDL_SCANCODE_R:
        return CTRL_KEY_R;
    case SDL_SCANCODE_S:
        return CTRL_KEY_S;
    case SDL_SCANCODE_T:
        return CTRL_KEY_T;
    case SDL_SCANCODE_U:
        return CTRL_KEY_U;
    case SDL_SCANCODE_V:
        return CTRL_KEY_V;
    case SDL_SCANCODE_W:
        return CTRL_KEY_W;
    case SDL_SCANCODE_X:
        return CTRL_KEY_X;
    case SDL_SCANCODE_Y:
        return CTRL_KEY_Y;
    case SDL_SCANCODE_Z:
        return CTRL_KEY_Z;
    case SDL_SCANCODE_0:
        return CTRL_KEY_0;
    case SDL_SCANCODE_1:
        return CTRL_KEY_1;
    case SDL_SCANCODE_2:
        return CTRL_KEY_2;
    case SDL_SCANCODE_3:
        return CTRL_KEY_3;
    case SDL_SCANCODE_4:
        return CTRL_KEY_4;
    case SDL_SCANCODE_5:
        return CTRL_KEY_5;
    case SDL_SCANCODE_6:
        return CTRL_KEY_6;
    case SDL_SCANCODE_7:
        return CTRL_KEY_7;
    case SDL_SCANCODE_8:
        return CTRL_KEY_8;
    case SDL_SCANCODE_9:
        return CTRL_KEY_9;
    case SDL_SCANCODE_F1:
        return CTRL_KEY_F1;
    case SDL_SCANCODE_F2:
        return CTRL_KEY_F2;
    case SDL_SCANCODE_F3:
        return CTRL_KEY_F3;
    case SDL_SCANCODE_F4:
        return CTRL_KEY_F4;
    case SDL_SCANCODE_F5:
        return CTRL_KEY_F5;
    case SDL_SCANCODE_F6:
        return CTRL_KEY_F6;
    case SDL_SCANCODE_F7:
        return CTRL_KEY_F7;
    case SDL_SCANCODE_F8:
        return CTRL_KEY_F8;
    case SDL_SCANCODE_F9:
        return CTRL_KEY_F9;
    case SDL_SCANCODE_F10:
        return CTRL_KEY_F10;
    case SDL_SCANCODE_F11:
        return CTRL_KEY_F11;
    case SDL_SCANCODE_F12:
        return CTRL_KEY_F12;
    case SDL_SCANCODE_KP_0:
        return CTRL_KEY_KP_0;
    case SDL_SCANCODE_KP_1:
        return CTRL_KEY_KP_1;
    case SDL_SCANCODE_KP_2:
        return CTRL_KEY_KP_2;
    case SDL_SCANCODE_KP_3:
        return CTRL_KEY_KP_3;
    case SDL_SCANCODE_KP_4:
        return CTRL_KEY_KP_4;
    case SDL_SCANCODE_KP_5:
        return CTRL_KEY_KP_5;
    case SDL_SCANCODE_KP_6:
        return CTRL_KEY_KP_6;
    case SDL_SCANCODE_KP_7:
        return CTRL_KEY_KP_7;
    case SDL_SCANCODE_KP_8:
        return CTRL_KEY_KP_8;
    case SDL_SCANCODE_KP_9:
        return CTRL_KEY_KP_9;
    case SDL_SCANCODE_KP_COMMA:
        return CTRL_KEY_KP_COMMA;
    case SDL_SCANCODE_KP_PERIOD:
        return CTRL_KEY_KP_DOT;
    case SDL_SCANCODE_KP_DIVIDE:
        return CTRL_KEY_KP_SLASH;
    case SDL_SCANCODE_KP_MULTIPLY:
        return CTRL_KEY_KP_ASTERISK;
    case SDL_SCANCODE_KP_MINUS:
        return CTRL_KEY_KP_MINUS;
    case SDL_SCANCODE_KP_PLUS:
        return CTRL_KEY_KP_PLUS;
    case SDL_SCANCODE_KP_ENTER:
        return CTRL_KEY_KP_ENTER;
    case SDL_SCANCODE_KP_EQUALS:
        return CTRL_KEY_KP_EQUAL;
    default:
        return 0;
    }
}
static void controls_sync_keys(void)
{
    int count=0;const bool *held=SDL_GetKeyboardState(&count);
    for(int sc=1;sc<count;sc++) {int key=controls_key((SDL_Scancode)sc);if(key)ControlsRuntime_Key(key,held[sc]);}
}
static void controls_key_labels(void)
{
    for(int sc=1;sc<SDL_SCANCODE_COUNT;sc++) {
        int key=controls_key((SDL_Scancode)sc);
        if(key)Controls_SetKeyLabel(key,SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)sc,SDL_KMOD_NONE,false)));
    }
}
/* Mouse: right circle (cancel), middle triangle; left is the menu bar's. */
static const uint16_t mouse_buttons[4] = {0, 0, 0x1000, 0x2000};

static void block_signals(sigset_t *previous)
{
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, previous);
}

static void restore_signals(const sigset_t *previous) { pthread_sigmask(SIG_SETMASK, previous, NULL); }

int Platform_Scale(void) { return scale; }
#ifdef SDL_PLATFORM_ANDROID
int Platform_HasWindowModes(void) { return 0; } /* the whole screen, always (android.c) */
#else
int Platform_HasWindowModes(void) { return 1; }
#endif

void Platform_ApplyDisplaySettings(void) /* only marks them: begin_present applies them */
{
    display_settings_pending = 1;
}

void Platform_SetScale(int wanted) /* the same, with the next frame */
{
    if (wanted < 1 || wanted > 8) {
        return;
    }
    if (window) {
        pending_scale = wanted; /* applied with the next frame */
    } else {
        scale = wanted;
    }
}

/* --- controllers ------------------------------------------------------ */

static SDL_Gamepad *pads[CONTROLS_DEVICES];
static void open_gamepad(SDL_JoystickID id)
{
    if (getenv("MEMORIES_NO_GAMEPAD"))
        return;
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id)
            return;
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (!pads[i]) {
            pads[i] = SDL_OpenGamepad(id);
            if (!pads[i])
                return;
            ControllerDevice *d = ControlsRuntime_Device(i);
            memset(d, 0, sizeof(*d));
            d->connected = 1;
            d->threshold = 1.0f / 3;
            const char *name = SDL_GetGamepadName(pads[i]);
            snprintf(d->name, sizeof(d->name), "%s", name ? name : "Controller");
            LOG(LOG_INPUT, "controller %u opened: %s", (unsigned)id, d->name);
            const char *serial = SDL_GetGamepadSerial(pads[i]);
            if (!ControlsLinux_Identity(SDL_GetGamepadPath(pads[i]), d->identity, sizeof(d->identity))) {
                snprintf(d->identity, sizeof(d->identity), "pad:%04x:%04x:%s", SDL_GetGamepadVendor(pads[i]),
                         SDL_GetGamepadProduct(pads[i]), serial ? serial : "");
                d->ambiguous = !serial || !*serial;
                if (d->ambiguous) {
                    static uint64_t session;
                    if (!session)
                        session = ControlsRuntime_Now();
                    snprintf(d->identity, sizeof(d->identity), "session:sdl:%llu:%u",
                             (unsigned long long)session, (unsigned)id);
                }
            }
            SDL_GamepadType type = SDL_GetGamepadType(pads[i]);
            d->style = type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE ? CTRL_ICON_XBOX
                       : type >= SDL_GAMEPAD_TYPE_PS3 && type <= SDL_GAMEPAD_TYPE_PS5 ? CTRL_ICON_PLAYSTATION
                       : type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO &&
                               type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
                           ? CTRL_ICON_NINTENDO
                           : CTRL_ICON_GENERIC;
            return;
        }
}
static void scan_gamepads(void)
{
    int count=0;SDL_JoystickID *ids;
    if(getenv("MEMORIES_NO_GAMEPAD"))return;
    ids=SDL_GetGamepads(&count);
    for(int i=0;ids && i<count;i++)open_gamepad(ids[i]);
    SDL_free(ids);
}
static void close_gamepad(SDL_JoystickID id)
{
    for(int i=0;i<CONTROLS_DEVICES;i++) if(pads[i]&&SDL_GetGamepadID(pads[i])==id) {
        SDL_CloseGamepad(pads[i]);pads[i]=NULL;memset(ControlsRuntime_Device(i),0,sizeof(ControllerDevice));
        ControlsRuntime_Gate();
    }
}
void Gamepad_Poll(unsigned frame)
{
    static uint64_t last_scan;
    static const SDL_GamepadButton buttons[CTRL_BTN_COUNT]={
        SDL_GAMEPAD_BUTTON_INVALID,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_GUIDE,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_MISC1,SDL_GAMEPAD_BUTTON_MISC2,
        SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
        SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
        SDL_GAMEPAD_BUTTON_MISC3, SDL_GAMEPAD_BUTTON_MISC4, SDL_GAMEPAD_BUTTON_MISC5, SDL_GAMEPAD_BUTTON_MISC6};
    uint64_t now=ControlsRuntime_Now();(void)frame;
    if(now-last_scan>=1000000){scan_gamepads();last_scan=now;}
    for(int i=0;i<CONTROLS_DEVICES;i++) if(pads[i]) {
        if(!SDL_GamepadConnected(pads[i])){close_gamepad(SDL_GetGamepadID(pads[i]));continue;}
        ControllerDevice *d=ControlsRuntime_Device(i);ControllerSnapshot *snap=&d->snapshot;memset(snap,0,sizeof(*snap));
        for(int b=1;b<CTRL_BTN_COUNT;b++) if(SDL_GetGamepadButton(pads[i],buttons[b])) {
            if(b>=CTRL_BTN_DPAD_UP && b<=CTRL_BTN_DPAD_RIGHT) {
                static const unsigned char hats[]={1,4,8,2};snap->hat_down|=hats[b-CTRL_BTN_DPAD_UP];
            } else snap->buttons_down|=1u<<(b-1);
        }
        for(int a=0;a<4;a++) {int value=SDL_GetGamepadAxis(pads[i],(SDL_GamepadAxis)a);snap->axis[a+1]=value/(value<0?32768.0f:32767.0f);}
        for(int a=0;a<2;a++) {int value=SDL_GetGamepadAxis(pads[i],(SDL_GamepadAxis)(SDL_GAMEPAD_AXIS_LEFT_TRIGGER+a));snap->trigger[a+1]=value>0?value/32767.0f:0;}
    }
    ControlsRuntime_Update();
}
uint16_t Gamepad_Bits(int p){return ControlsRuntime_Pad(p);}
int Gamepad_Connected(int p){return ControlsRuntime_Connected(p);}

/* --- audio ------------------------------------------------------------ */

static void (*mixer)(int16_t *, size_t);
static SDL_AudioStream *stream;
static volatile unsigned audio_underruns;

static void SDLCALL feed(void *userdata, SDL_AudioStream *to, int additional, int total)
{
    static int16_t buffer[256 * 2];
    (void)userdata;
    (void)total;
    if (SDL_GetAudioStreamQueued(to) == 0) audio_underruns++;
    while (additional > 0) {
        mixer(buffer, 256);
        SDL_PutAudioStreamData(to, buffer, (int)sizeof(buffer));
        additional -= (int)sizeof(buffer);
    }
}

void Platform_AudioStats(int *queued_frames, unsigned *underruns)
{
    if (queued_frames) *queued_frames = stream ? SDL_GetAudioStreamQueued(stream) / (int)(sizeof(int16_t) * 2) : 0;
    if (underruns) *underruns = audio_underruns;
}

int HERE(Platform_StartAudio)(void (*mix)(int16_t *, size_t))
{
    const char *dump = getenv("MEMORIES_DUMP_AUDIO");
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    sigset_t previous;
    if (dump || getenv("MEMORIES_NO_AUDIO") || getenv("MEMORIES_HEADLESS") || !window) {
        return Platform_StartSilentAudio(mix, dump);
    }
    mixer = mix;
#ifdef __ANDROID__
    /* 256 frames starved AAudio on a phone (Xiaomi 11T Pro, Android 14): its
     * track got 5 s of sound every 6.3 s, crackling and gaps; 1024 (23 ms)
     * keeps it fed. */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
#else
    /* SDL's ALSA backend at 256 frames (5.8 ms) ran dry hundreds of times
     * in 25 seconds on a PipeWire desktop (Arch, no 32-bit libpulse, so SDL
     * falls back to ALSA; this SDL is built without PipeWire), whatever the
     * game speed: the music cut in and out. 1024 (23 ms) had none, as on
     * Android. PulseAudio and the other backends keep 256. */
    {
        const char *driver = SDL_GetCurrentAudioDriver();
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, driver && !strcmp(driver, "alsa") ? "1024" : "256");
    }
#endif
    block_signals(&previous);
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream);
    }
    restore_signals(&previous);
    if (!stream) {
        fprintf(stderr, "memories-pc: no audio device (%s); continuing silently\n", SDL_GetError());
        return Platform_StartSilentAudio(mix, NULL);
    }
    return 0;
}

/* --- window ----------------------------------------------------------- */

static void relayout(void)
{
    static int logged_window_w, logged_window_h, logged_output_w, logged_output_h;
    int output_w, output_h, window_w, window_h, menu;
    int aspect = Settings_Get(SET_ASPECT);
    /* Widescreen pictures arrive 4/3 as wide as the 4:3 ones (libgpu.c), so
     * both keep the same pixel shape; the frame's size in game pixels is
     * what the window is laid out for, whatever the internal resolution. */
    int pw = aspect == 1 ? logical_w : aspect == 2 ? logical_h * 16 / 9 : logical_h * 4 / 3;
    int ph = logical_h, area_h, mode = Settings_Get(SET_SCALING);
    float factor;
    if ((!renderer && !use_gl) || picture_w <= 0 || picture_h <= 0 ||
        !SDL_GetWindowSize(window, &window_w, &window_h) ||
        !(use_gl ? SDL_GetWindowSizeInPixels(window, &output_w, &output_h) :
                    SDL_GetRenderOutputSize(renderer, &output_w, &output_h)) ||
        window_w <= 0 || window_h <= 0) {
        return;
    }
    update_menu_scale(window_h);
    menu = menu_visible && !bar_overlays() ? Menu_Height() : 0;
    area_h = window_h - menu;
    if (area_h < 1) area_h = 1;
    layout.win_w = window_w;
    layout.win_h = window_h;
    if (TouchPad_Layout(window_w, window_h, menu)) menu_dirty = 1;
    if (Panel_Shown()) layout_panel();
    layout.pixel_x = (float)output_w / (float)window_w;
    layout.pixel_y = (float)output_h / (float)window_h;
    if (window_w != logged_window_w || window_h != logged_window_h ||
        output_w != logged_output_w || output_h != logged_output_h) {
        LOG(LOG_WINDOW, "layout %dx%d logical -> %dx%d pixels (%.2fx, %.2fy)",
            window_w, window_h, output_w, output_h, layout.pixel_x, layout.pixel_y);
        logged_window_w = window_w; logged_window_h = window_h;
        logged_output_w = output_w; logged_output_h = output_h;
    }
    if (mode == 2) {
        layout.dst.x = 0;
        layout.dst.y = (float)menu;
        layout.dst.w = (float)window_w;
        layout.dst.h = (float)area_h;
    } else if (mode == 1) {
        factor = SDL_min((float)window_w / (float)pw, (float)area_h / (float)ph);
        layout.dst.w = (float)pw * factor;
        layout.dst.h = (float)ph * factor;
        layout.dst.x = ((float)window_w - layout.dst.w) * 0.5f;
        layout.dst.y = (float)menu + ((float)area_h - layout.dst.h) * 0.5f;
    } else {
        int k = SDL_min(window_w / pw, area_h / ph);
        if (k < 1) k = 1;
        layout.dst.w = (float)(pw * k);
        layout.dst.h = (float)(ph * k);
        layout.dst.x = (float)(window_w - pw * k) * 0.5f;
        layout.dst.y = (float)menu + (float)(area_h - ph * k) * 0.5f;
    }
    if (overlay && (canvas.width != window_w || canvas.height != window_h)) {
        SDL_DestroyTexture(overlay);
        overlay = NULL;
    }
    if (use_gl && gl_overlay && (canvas.width != window_w || canvas.height != window_h)) {
        glDeleteTextures(1, &gl_overlay);
        gl_overlay = 0;
    }
    if ((use_gl && !gl_overlay) || (!use_gl && !overlay)) {
        if (use_gl) {
            glGenTextures(1, &gl_overlay);
            glBindTexture(GL_TEXTURE_2D, gl_overlay);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, window_w, window_h, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        } else {
            overlay = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        window_w, window_h);
            SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_LINEAR);
        }
        overlay_pixels = realloc(overlay_pixels, (size_t)window_w * (size_t)window_h * 4);
        memset(overlay_pixels, 0, (size_t)window_w * (size_t)window_h * 4);
        canvas.pixels = overlay_pixels;
        canvas.stride = window_w;
        canvas.width = window_w;
        canvas.height = window_h;
        canvas.alpha = 1;
        Menu_Draw(&canvas);
        if (use_gl) {
            glBindTexture(GL_TEXTURE_2D, gl_overlay);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, window_w, window_h,
                            GL_BGRA, GL_UNSIGNED_BYTE, overlay_pixels);
        } else {
            SDL_UpdateTexture(overlay, NULL, overlay_pixels, window_w * 4);
        }
    }
}

static void display_picture_size(int *w, int *h)
{
    int aspect = Settings_Get(SET_ASPECT);
    *h = logical_h > 0 ? logical_h : 240;
    *w = aspect == 2 ? *h * 16 / 9
                     : aspect == 1 ? (logical_w > 0 ? logical_w : 320)
                                   : *h * 4 / 3;
}

static void apply_display_settings(void)
{
    int fullscreen = Settings_Get(SET_FULLSCREEN), pw, ph;
    display_picture_size(&pw, &ph);
#ifdef SDL_PLATFORM_ANDROID
    Android_ApplyScreenRotation(Settings_Get(SET_SCREEN_ROTATION));
#endif
    if (fullscreen == 2) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *current = SDL_GetCurrentDisplayMode(display);
        SDL_DisplayMode closest;
        float refresh = current ? current->refresh_rate : 0.0f;
        if (SDL_GetClosestFullscreenDisplayMode(display, pw * scale, ph * scale, refresh, true, &closest)) {
            SDL_SetWindowFullscreenMode(window, &closest);
            SDL_SetWindowFullscreen(window, true);
        } else {
            fprintf(stderr, "memories-pc: no exclusive fullscreen mode; using desktop fullscreen\n");
            Settings_Set(SET_FULLSCREEN, 1);
            Settings_Save();
            SDL_SetWindowFullscreenMode(window, NULL);
            SDL_SetWindowFullscreen(window, true);
        }
    } else if (fullscreen == 1 || Settings_Get(SET_BORDERLESS)) {
        /* Borderless is fullscreen at the desktop's mode: a window sized to
         * the display is kept off the panels by the window manager. */
        SDL_SetWindowFullscreenMode(window, NULL);
        SDL_SetWindowFullscreen(window, true);
    } else {
        int x = Settings_Get(SET_WINDOW_X), y = Settings_Get(SET_WINDOW_Y);
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowBordered(window, true);
        update_menu_scale(ph * scale + 26 * Menu_AutoScale(ph * scale));
        SDL_SetWindowSize(window, pw * scale, ph * scale + Menu_Height());
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    if (use_gl && gl_picture) {
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (picture) {
        SDL_SetTextureScaleMode(picture, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    }
    apply_swap_interval();
    relayout();
    show();
}

static void resize(int w, int h)
{
    if (use_gl && gl_picture && (picture_w != w || picture_h != h)) {
        glDeleteTextures(1, &gl_picture);
        gl_picture = 0;
    }
    if (picture && (picture_w != w || picture_h != h)) {
        SDL_DestroyTexture(picture);
        picture = NULL;
    }
    if (use_gl && !gl_picture) {
        glGenTextures(1, &gl_picture);
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        picture_pixels = realloc(picture_pixels, (size_t)w * (size_t)h * 4);
        picture_w = w;
        picture_h = h;
    } else if (!use_gl && !picture) {
        picture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(picture, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        picture_pixels = realloc(picture_pixels, (size_t)w * (size_t)h * 4);
        picture_w = w;
        picture_h = h;
    }
    relayout();
}

static void upload_overlay(int x, int y, int w, int h)
{
    SDL_Rect rect;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > layout.win_w) { w = layout.win_w - x; }
    if (y + h > layout.win_h) { h = layout.win_h - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (use_gl) {
        /* GL has no source-row pitch for this upload; rows are contiguous in
         * the full-width canvas, so upload whole rows for the dirty span. */
        glBindTexture(GL_TEXTURE_2D, gl_overlay);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, layout.win_w);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE,
                        overlay_pixels + (size_t)y * (size_t)layout.win_w + (size_t)x);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        SDL_UpdateTexture(overlay, &rect, overlay_pixels + (size_t)y * (size_t)layout.win_w + (size_t)x,
                          layout.win_w * 4);
    }
}

static void gl_quad_part(GLuint texture, float x, float y, float w, float h, float s0, float t0, float s1, float t1)
{
    glBindTexture(GL_TEXTURE_2D, texture);
#ifdef __APPLE__
    PresentPass_Quad(2 * x / layout.win_w - 1, 1 - 2 * y / layout.win_h,
                     2 * w / layout.win_w, -2 * h / layout.win_h, s0, t0, s1, t1);
#else
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex2f(x, y);
    glTexCoord2f(s1, t0); glVertex2f(x + w, y);
    glTexCoord2f(s1, t1); glVertex2f(x + w, y + h);
    glTexCoord2f(s0, t1); glVertex2f(x, y + h);
    glEnd();
#endif
}

static void gl_quad(GLuint texture, float x, float y, float w, float h)
{
    gl_quad_part(texture, x, y, w, h, 0, 0, 1, 1);
}

/* OpenGL ES: the pass's frame into es_shown, a texture of SDL's own the
 * size of the shown area (made again when that changes): the area of the
 * picture, or the widened picture, as the desktop presenter samples them.
 * SDL owns the texture; the pass only draws into it, so either side can
 * make its textures again without the other's. 0 when nothing was copied. */
static int es_copy_shown(void)
{
    int w = gl_pass_rect[2], h = gl_pass_rect[3], ok;
    GLuint from = gl_pass_texture ? gl_pass_texture : (GLuint)GlPicture_Texture(NULL, NULL), to;
    if (!es_picture || !from || w <= 0 || h <= 0) return 0;
    if (es_shown && (es_shown_w != w || es_shown_h != h)) {
        SDL_DestroyTexture(es_shown);
        es_shown = NULL;
    }
    if (!es_shown) {
        es_shown = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!es_shown) {
            fprintf(stderr, "memories-pc: the OpenGL picture's %dx%d texture: %s\n", w, h, SDL_GetError());
            return 0;
        }
        SDL_SetTextureBlendMode(es_shown, SDL_BLENDMODE_NONE);
        es_shown_w = w;
        es_shown_h = h;
    }
    to = (GLuint)SDL_GetNumberProperty(SDL_GetTextureProperties(es_shown), SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0);
    {
        uint64_t started = phase_start();
        es_enter();
        ok = GlPicture_CopyInto(from, gl_pass_rect[0], gl_pass_rect[1], w, h, to);
        es_leave();
        phase_end(PHASE_COPY, started);
    }
    return ok;
}

/* OpenGL ES, before the pass's frame is shown: into es_shown. Where it
 * cannot be (SDL's texture cannot be made or drawn into) the pass is given
 * up, so the software GPU draws the picture again, and the caller shows
 * another picture this time (0). Given up for good: a device reset starts
 * again only a pass that was on (reset_renderer). */
static int es_take_frame(void)
{
    if (test_copy_fail_at == -2) {
        const char *at = getenv("MEMORIES_TEST_GL_COPY_FAIL");
        test_copy_fail_at = at && *at ? strtol(at, NULL, 10) : -1;
    }
    if (test_copy_fail_at >= 0 && current_frame >= (unsigned long)test_copy_fail_at) {
        test_copy_fail_at = -1;
        fprintf(stderr, "memories-pc: MEMORIES_TEST_GL_COPY_FAIL: frame %u's copy fails\n", current_frame);
        es_shown_last = 0;
    } else {
        es_shown_last = es_copy_shown();
    }
    if (!es_shown_last) {
        fprintf(stderr, "memories-pc: the OpenGL picture cannot be shown; the software GPU draws it\n");
        es_picture = 0;
        pass_shown.valid = 0;
        es_enter(); /* its names are deleted in the renderer's context */
        GlPicture_Stop();
        es_leave();
        if (es_shown) SDL_DestroyTexture(es_shown); /* nothing copies into it now */
        es_shown = NULL;
        es_shown_w = es_shown_h = 0;
        Menu_SetHdPicture(0);
    }
    return es_shown_last;
}

static void draw_overlay(int *x, int *y, int *w, int *h)
{
    int hx, hy, hw, hh, tx, ty, tw, th, left, right, row;
    FusionHelper_Viewport((int)layout.dst.x, (int)layout.dst.y, (int)layout.dst.w, (int)layout.dst.h);
    if (Panel_Shown()) { /* the panel covers the window: nothing else shows */
        Panel_Draw(&canvas);
        *x = *y = 0;
        *w = layout.win_w;
        *h = layout.win_h;
        return;
    }
    /* The mods' overlays go under the pad, so a mod's full-screen list does
     * not hide where to tap; the port's HUD and the menu stay over both.
     * Without the pad they keep their place in Hud_Draw. */
    if (TouchPad_Shown()) Hud_DrawMods(&canvas);
    TouchPadArt_Draw(&canvas, &tx, &ty, &tw, &th); /* under the menu and the HUD */
    /* The save and deck slot menus keep between the pad's columns (they are
     * played with the pad) and below its top row, and below the bar while
     * it shows (pushing the picture down or drawn over it); from the top
     * only where a bar drawn over the picture is hidden. */
    if (!TouchPad_FreeSpan(&left, &right)) left = right = 0;
    row = TouchPad_TopRowBottom();
    Menu_SetOverlayArea(left, right, row ? row : bar_overlays() && !menu_visible ? 0 : -1);
    if (Settings_Get(SET_SHOW_HUD) == 2 || Menu_IsOpen()) {
        Hud_Draw(&canvas);
        Menu_Draw(&canvas); /* dropdowns stay above the full statistics panel */
    } else {
        Menu_Draw(&canvas);
        Hud_Draw(&canvas);
    }
    Menu_Bounds(x, y, w, h);
    Hud_Bounds(&hx, &hy, &hw, &hh);
    if (tw && th) {
        if (!hw || !hh) { hx = tx; hy = ty; hw = tw; hh = th; }
        else {
            int x1 = hx + hw > tx + tw ? hx + hw : tx + tw, y1 = hy + hh > ty + th ? hy + hh : ty + th;
            hx = hx < tx ? hx : tx;
            hy = hy < ty ? hy : ty;
            hw = x1 - hx;
            hh = y1 - hy;
        }
    }
    if (!*w || !*h) { *x = hx; *y = hy; *w = hw; *h = hh; return; }
    if (hw && hh) {
        int x1 = *x + *w > hx + hw ? *x + *w : hx + hw;
        int y1 = *y + *h > hy + hh ? *y + *h : hy + hh;
        *x = *x < hx ? *x : hx;
        *y = *y < hy ? *y : hy;
        *w = x1 - *x;
        *h = y1 - *y;
    }
}

static void show(void)
{
    SDL_FRect physical;
    if (use_gl) {
        int output_w, output_h, effects;
        if (!gl_picture || !gl_overlay || !SDL_GetWindowSizeInPixels(window, &output_w, &output_h)) return;
        glViewport(0, 0, output_w, output_h);
#ifndef __APPLE__
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, layout.win_w, layout.win_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
#endif
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
#ifndef __APPLE__
        glEnable(GL_TEXTURE_2D);
        glColor4f(1, 1, 1, 1);
#endif
        glDisable(GL_BLEND);
        if (gl_pass_shown) {
            int pw = gl_pass_size[0], ph = gl_pass_size[1], x = gl_pass_rect[0], y = gl_pass_rect[1];
            GLuint texture = gl_pass_texture, shown;
            /* Every filter reads the shown area alone: past its edges lies
             * the rest of VRAM, which bilinear would blend in (gl_picture.h),
             * and the area moves between the game's two buffers every frame,
             * so nearest at a scale that is not whole would round its edge
             * texels one way, then the other, and the picture would shake.
             * The whole picture only when that area runs past its edge
             * (with anti-aliasing it is then resolved whole). */
            if (!gl_pass_texture &&
                (shown = (GLuint)GlPicture_ShownTexture(x, y, gl_pass_rect[2], gl_pass_rect[3]))) {
                texture = shown;
                pw = gl_pass_rect[2];
                ph = gl_pass_rect[3];
                x = y = 0;
            } else if (!gl_pass_texture) {
                texture = (GLuint)GlPicture_Texture(&pw, &ph);
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
            effects = PresentPass_Wanted() &&
                      PresentPass_Begin(texture, gl_pass_rect[3], (float)x / (float)pw, (float)y / (float)ph,
                                        (float)(x + gl_pass_rect[2]) / (float)pw,
                                        (float)(y + gl_pass_rect[3]) / (float)ph, GlPicture_Scale() >= 2);
            gl_quad_part(texture, layout.dst.x, layout.dst.y, layout.dst.w, layout.dst.h, (float)x / (float)pw,
                         (float)y / (float)ph, (float)(x + gl_pass_rect[2]) / (float)pw,
                         (float)(y + gl_pass_rect[3]) / (float)ph);
        } else {
            effects = PresentPass_Wanted() && PresentPass_Begin(gl_picture, picture_h, 0.0f, 0.0f, 1.0f, 1.0f, 0);
            gl_quad(gl_picture, layout.dst.x, layout.dst.y, layout.dst.w, layout.dst.h);
        }
        if (effects) PresentPass_End(); /* the menu and the HUD are not filtered */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl_quad(gl_overlay, 0, 0, (float)layout.win_w, (float)layout.win_h);
        if (window_shot_pending) {
            window_shot_pending = 0;
            save_window_image(); /* the back buffer holds this frame until the swap */
        }
        apply_swap_interval();
        {
            uint64_t started = phase_start();
            SDL_GL_SwapWindow(window);
            phase_end(PHASE_SWAP, started);
        }
        if (swap_interval == 1) Platform_NotifyPresent(real_now_us(), 1);
        phases_shown();
        return;
    }
    if (!renderer || !picture || !overlay) return;
    physical.x = layout.dst.x * layout.pixel_x;
    physical.y = layout.dst.y * layout.pixel_y;
    physical.w = layout.dst.w * layout.pixel_x;
    physical.h = layout.dst.h * layout.pixel_y;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    /* OpenGL ES: the pass's frame (es_take_frame), also when the menu is
     * repainted over it. */
    if (es_shown_last && es_shown) {
        SDL_SetTextureScaleMode(es_shown, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_RenderTexture(renderer, es_shown, NULL, &physical);
    } else {
        SDL_RenderTexture(renderer, picture, NULL, &physical);
    }
    SDL_RenderTexture(renderer, overlay, NULL, NULL);
    if (window_shot_pending) {
        char path[1024];
        window_shot_pending = 0;
        if (screenshot_path(path, sizeof(path), "bmp")) save_surface(SDL_RenderReadPixels(renderer, NULL), path);
    }
    apply_swap_interval();
    {
        uint64_t started = phase_start();
        SDL_RenderPresent(renderer);
        phase_end(PHASE_SWAP, started);
    }
    if (swap_interval == 1) Platform_NotifyPresent(real_now_us(), 1);
    phases_shown();
}

/* Repaint the menu where it was and where it is, and upload that much of
 * the overlay. */
static unsigned compose_count; /* MEMORIES_TRACE=window: compositions per 120 frames */

static void compose_menu(void)
{
    int x, y, w, h, x0, y0, x1, y1, row;
    menu_dirty = 0;
    compose_count++;
    for (row = shown_menu.y; row < shown_menu.y + shown_menu.h && row < layout.win_h; row++) {
        memset(overlay_pixels + (size_t)row * (size_t)layout.win_w, 0, (size_t)layout.win_w * 4);
    }
    draw_overlay(&x, &y, &w, &h);
    x0 = x < shown_menu.x ? x : shown_menu.x;
    y0 = y < shown_menu.y ? y : shown_menu.y;
    x1 = x + w > shown_menu.x + shown_menu.w ? x + w : shown_menu.x + shown_menu.w;
    y1 = y + h > shown_menu.y + shown_menu.h ? y + h : shown_menu.y + shown_menu.h;
    shown_menu.x = x;
    shown_menu.y = y;
    shown_menu.w = w;
    shown_menu.h = h;
    upload_overlay(x0, y0, x1 - x0, y1 - y0);
}

/* The menu changed under a still picture: repaint it and show the window. */
static void repaint_menu(void)
{
    compose_menu();
    show();
}

/* The overlay only changes with the menu, or when the HUD's text does;
 * otherwise the texture already holds it. */
static void compose_menu_if_changed(void)
{
    static unsigned hud_signature;
    unsigned now = Hud_Signature();
    if (menu_dirty || now != hud_signature) {
        hud_signature = now;
        compose_menu();
    }
}

static MenuKey menu_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_ESCAPE: return MENU_KEY_ESCAPE;
#ifdef SDL_PLATFORM_ANDROID
    case SDLK_AC_BACK: return MENU_KEY_ESCAPE; /* a phone's Back closes a menu or answers a notice as Esc does */
#endif
    case SDLK_F10: return MENU_KEY_F10;
    case SDLK_TAB: return MENU_KEY_TAB;
    case SDLK_BACKSPACE: return MENU_KEY_BACKSPACE;
    case SDLK_LEFT: return MENU_KEY_LEFT;
    case SDLK_RIGHT: return MENU_KEY_RIGHT;
    case SDLK_UP: return MENU_KEY_UP;
    case SDLK_DOWN: return MENU_KEY_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return MENU_KEY_ENTER;
    default: return MENU_KEY_OTHER;
    }
}

static void translate(const SDL_Event *event, MenuEvent *out)
{
    memset(out, 0, sizeof(*out));
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        out->type = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? MENU_EVENT_BUTTON_DOWN : MENU_EVENT_BUTTON_UP;
        out->button = event->button.button == SDL_BUTTON_LEFT ? 1 : event->button.button == SDL_BUTTON_MIDDLE ? 2
                    : event->button.button == SDL_BUTTON_RIGHT ? 3 : 0;
        out->x = (int)event->button.x;
        out->y = (int)event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        out->type = MENU_EVENT_MOTION;
        out->x = (int)event->motion.x;
        out->y = (int)event->motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        out->type = MENU_EVENT_WHEEL;
        out->wheel = event->wheel.y > 0 ? 1 : event->wheel.y < 0 ? -1 : 0;
        out->x = (int)event->wheel.mouse_x;
        out->y = (int)event->wheel.mouse_y;
        break;
    case SDL_EVENT_TEXT_INPUT:
        out->type = MENU_EVENT_TEXT;
        snprintf(out->text, sizeof(out->text), "%s", event->text.text);
        Menu_TextTrim(out->text);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: out->type = MENU_EVENT_LEAVE; break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        out->type = event->type == SDL_EVENT_KEY_DOWN ? MENU_EVENT_KEY_DOWN : MENU_EVENT_KEY_UP;
        out->key = menu_key(event->key.key);
        break;
    default: break;
    }
}

static int dispatch_controls(const SDL_Event *event, const MenuEvent *menu_event)
{
    if (controls_window && SDL_GetWindowFromEvent(event) == controls_window) {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
            if (event->key.repeat)
                return 1;
            int key = controls_key(event->key.scancode), down = event->type == SDL_EVENT_KEY_DOWN;
            ControlsRuntime_Key(key, down);
            int mods = (event->key.mod & SDL_KMOD_SHIFT ? 1 : 0) |
                       (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI) ? 2 : 0);
            /* Right Shift alone is the default Select binding. */
            if ((event->key.mod & ~SDL_KMOD_RSHIFT &
                 (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0)
                mods = 0;
            ControlsWindow_Key(key, down, event->key.repeat, mods);
            ControlsWindow_Tick();
        } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            ControlsWindow_RequestClose();
        else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
            ControlsWindow_FocusLost();
        else if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED)
            controls_sync_keys();
        else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event->type == SDL_EVENT_WINDOW_RESIZED)
            resize_controls(event->window.data1, event->window.data2);
        else
            ControlsWindow_Event(menu_event);
        if (ControlsWindow_ShouldClose())
            close_controls();
        return 1;
    }
    if (controls_window && (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL))
        return 1;

    return 0;
}

/* Input while a panel shows is the panel's (window events, quitting and
 * controllers coming and going are not input). A key's release still lets
 * go of what the game held. */
static int dispatch_panel(const SDL_Event *event, const MenuEvent *menu_event)
{
    int redraw = 0, tapped = 0;
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        redraw = Panel_Pointer(menu_event, event->button.which == SDL_TOUCH_MOUSEID);
        tapped = event->type == SDL_EVENT_MOUSE_BUTTON_UP && Panel_Tapped(); /* not a drag's end */
        break;
    case SDL_EVENT_MOUSE_MOTION:
        redraw = Panel_Pointer(menu_event, event->motion.which == SDL_TOUCH_MOUSEID);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        redraw = Panel_Pointer(menu_event, 0);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        Panel_Pointer(menu_event, 0);
        return 0; /* the window's own handling too */
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        /* Back is Esc here, as in a menu: it closes the panel (or what is open in it). */
        int key = event->key.key == SDLK_AC_BACK ? CTRL_KEY_ESCAPE : controls_key(event->key.scancode);
        int mods = (event->key.mod & SDL_KMOD_SHIFT ? 1 : 0) |
                   (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI) ? 2 : 0);
        if ((event->key.mod & ~SDL_KMOD_RSHIFT & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0)
            mods = 0; /* Right Shift alone is the default Select binding */
        if (event->type == SDL_EVENT_KEY_UP) ControlsRuntime_Key(controls_key(event->key.scancode), 0);
        redraw = Panel_Key(menu_event, key, event->key.repeat, mods);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        redraw = Panel_Key(menu_event, 0, 0, 0);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: /* MEMORIES_PANELS=overlay: the quit prompt would ask under the panel */
        if (SDL_GetWindowFromEvent(event) != window) return 0;
        Panel_RequestClose();
        redraw = 1;
        break;
    default:
        return 0;
    }
    if (redraw) menu_dirty = 1;
    panel_after(tapped);
    return 1;
}

/* The application events SDL sends only to event watchers, from inside
 * the pump (SDL_PollEvent) on this thread: a phone or tablet app going to
 * the background and coming back; desktops never send them. SDL stops its
 * event loop (and so the game) and the audio until the app is back;
 * stopping the clock too keeps the game from running to catch up the time
 * it was away. */
static bool SDLCALL app_event(void *userdata, SDL_Event *event)
{
    (void)userdata;
    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND: case SDL_EVENT_DID_ENTER_BACKGROUND:
        if (!background_paused) {
            background_paused = 1;
            background_clock_rate = Platform_ClockRate();
            if (background_clock_rate) Platform_SetClockRate(0);
            ControlsRuntime_ResetKeys();
            LOG(LOG_WINDOW, "app in the background: clock stopped (was %d%%)", background_clock_rate);
        }
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (background_paused) {
            background_paused = 0;
            if (background_clock_rate && Platform_ClockRate() == 0) Platform_SetClockRate(background_clock_rate);
            menu_dirty = 1;
#ifdef SDL_PLATFORM_ANDROID
            /* The window's surface comes back a moment after this event:
             * a still picture (a panel, which pauses the game, or the
             * system's file picker it opened) repainted only now stayed
             * black until the next touch. Repainted for a second instead. */
            foreground_repaint_until = real_now_us() + 1000000;
#endif
            LOG(LOG_WINDOW, "app in the foreground: clock at %d%%", Platform_ClockRate());
        }
        break;
    default:
        break;
    }
    return true;
}

static void pump(void)
{
    SDL_Event event;
#ifdef SDL_PLATFORM_ANDROID
    /* The display's density changed under the running game (Display size in
     * the system settings): the window keeps its pixels, so no resize
     * comes; the menu, its touch targets and the touch controls are laid
     * out again for the new density. */
    if (window && Android_DensityChanged()) {
        relayout();
        menu_dirty = 1;
    }
#endif
    if (test_reset_at == -2) {
        const char *at = getenv("MEMORIES_TEST_GL_RESET");
        test_reset_at = at && *at ? strtol(at, NULL, 10) : -1;
        test_reset_fails = at && strstr(at, "fail") != NULL;
    }
    if (test_reset_at >= 0 && current_frame >= (unsigned long)test_reset_at) {
        SDL_zero(event);
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        event.render.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&event);
        test_reset_at = -1;
    }
    while (SDL_PollEvent(&event)) {
        MenuEvent menu_event;
        translate(&event, &menu_event);
        if(event.type==SDL_EVENT_KEYMAP_CHANGED){controls_key_labels();continue;}
        if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
            event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
            int kind = event.type == SDL_EVENT_FINGER_DOWN     ? TOUCH_FINGER_DOWN
                       : event.type == SDL_EVENT_FINGER_MOTION ? TOUCH_FINGER_MOVE
                                                               : TOUCH_FINGER_UP;
            int fx = (int)(event.tfinger.x * (float)layout.win_w), fy = (int)(event.tfinger.y * (float)layout.win_h);
            if (SDL_GetWindowFromEvent(&event) == window &&
                TouchPad_Finger(kind, (uint64_t)event.tfinger.fingerID, fx, fy))
                menu_dirty = 1;
            if (kind != TOUCH_FINGER_MOVE)
                LOG(LOG_INPUT, "finger %d %s at %d,%d: touch pad %s", (int)event.tfinger.fingerID,
                    kind == TOUCH_FINGER_DOWN ? "down" : "up", fx, fy, TouchPad_Shown() ? "shown" : "hidden");
            continue;
        }
        /* The mouse events SDL makes of a touch: on the pad they are the
         * pad's, not the menu's (from the button going down to it coming
         * up, and a hover over it). */
        if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
            event.button.which == SDL_TOUCH_MOUSEID) {
            int on_pad = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                             ? (touch_mouse_on_pad = TouchPad_Covers((int)event.button.x, (int)event.button.y))
                             : touch_mouse_on_pad;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) touch_mouse_on_pad = 0;
            if (on_pad) continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID &&
            (touch_mouse_on_pad || TouchPad_Covers((int)event.motion.x, (int)event.motion.y)))
            continue;
        if (Panel_Shown() && dispatch_panel(&event, &menu_event)) continue;
        /* A tap at the top of the screen shows the hidden bar under it, so
         * the press lands on the bar's menu there. */
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.which == SDL_TOUCH_MOUSEID) {
            pointer_is_mouse = 0;
            if (covers_screen() && event.button.y < Menu_Height() && menu_reveal_frames < 2) menu_reveal_frames = 2;
        }
        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) && !event.key.repeat)
            LOG(LOG_INPUT, "key %s %s (scancode %d), input %s", SDL_GetScancodeName(event.key.scancode),
                event.type == SDL_EVENT_KEY_DOWN ? "down" : "up", (int)event.key.scancode,
                ControlsRuntime_Blocked() ? "held back" : "open");
        /* A key or a controller's button: Automatic hides the pad again. */
        if (((event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key != SDLK_AC_BACK) ||
             event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) && TouchPad_OtherInput())
            menu_dirty = 1;
        if(event.type==SDL_EVENT_GAMEPAD_ADDED){open_gamepad(event.gdevice.which);continue;}
        if(event.type==SDL_EVENT_GAMEPAD_REMOVED){close_gamepad(event.gdevice.which);continue;}
        /* Every release, before a menu can take it: else a held action
         * (turbo) would run on after the menu closes. */
        if(event.type==SDL_EVENT_KEY_UP)ControlsRuntime_Key(controls_key(event.key.scancode),0);
        if(dispatch_controls(&event, &menu_event))continue;
        if (mods_window && SDL_GetWindowFromEvent(&event) == mods_window) {
            if (event.type == SDL_EVENT_WINDOW_RESIZED) resize_mods(event.window.data1, event.window.data2);
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (ModsWindow_RequestClose()) close_mods(); else mods_dirty = 1;
                continue;
            }
            if (ModsWindow_Event(&menu_event)) close_mods();
            else if (ModsWindow_Redraws(&menu_event)) mods_dirty = 1;
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID) {
            pointer_is_mouse = 0;
            /* A finger has no hover: SDL moves the mouse to a tap just
             * before pressing there, which the menu would take for the
             * pointer sliding along the bar (and open the menu the press
             * then closes). Only a drag, with the finger down, is the menu's. */
            if (!(event.motion.state & SDL_BUTTON_LMASK)) continue;
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            static unsigned logged_frame = ~0u;
            pointer_is_mouse = 1;
            pointer_x = menu_event.x;
            pointer_y = menu_event.y;
            pointer_inside = 1;
            if (current_frame - logged_frame >= 3) { /* a sample, not every motion */
                logged_frame = current_frame;
                LOG(LOG_MENU, "pointer at %d,%d", pointer_x, pointer_y);
            }
            last_pointer_motion = current_frame;
            show_cursor();
            if (covers_screen() && pointer_y < Menu_Height()) menu_reveal_frames = 120;
        } else if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
            pointer_inside = 1;
        } else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            pointer_inside = 0;
            show_cursor();
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10) menu_reveal_frames = 120;
        update_menu_visibility();
        if (menu_event.type != MENU_EVENT_NONE && Menu_Event(&menu_event, &quit)) {
            static unsigned logged;
            if (current_frame - logged >= 30) {
                logged = current_frame;
                LOG(LOG_WINDOW, "menu dirty from SDL event %#x (menu event %d)", (unsigned)event.type, (int)menu_event.type);
            }
            menu_dirty = 1;
            continue;
        }
        switch (event.type) {
        /* Closing the main window (its button, Alt+F4) asks, like File >
         * Exit; SDL_EVENT_QUIT is left to signals and the session ending. */
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: QuitPrompt_Request(&quit); break;
        case SDL_EVENT_QUIT: quit = 1; break;
        case SDL_EVENT_WINDOW_EXPOSED: show(); break;
        case SDL_EVENT_RENDER_DEVICE_RESET:
            if (renderer && !strcmp(SDL_GetRendererName(renderer), "opengles2")) reset_renderer();
            break;
        case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            relayout();
            menu_dirty = 1;
            break;
        case SDL_EVENT_WINDOW_MOVED:
            if (!covers_screen()) {
                Settings_Set(SET_WINDOW_X, event.window.data1);
                Settings_Set(SET_WINDOW_Y, event.window.data2);
                Settings_Save();
            }
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            update_display_refresh();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if(!controls_window)ControlsRuntime_ResetKeys(); mouse_bits=wheel_now=0;wheel_frames=0;
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) Spu_SetOutputVolume(0);
            if (Settings_Get(SET_PAUSE_ON_FOCUS_LOSS) && Platform_ClockRate() != 0) {
                /* Turbo ends with the focus (the keys are let go), so
                 * coming back resumes the chosen speed, not 400%. */
                focus_clock_rate = ControlsRuntime_HostHeld() >> CTRL_HOST_TURBO & 1 ? Settings_Get(SET_SPEED)
                                                                                     : Platform_ClockRate();
                focus_paused = 1;
                Platform_SetClockRate(0);
            }
            show_cursor();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) {
                Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
            }
            if (focus_paused && Platform_ClockRate() == 0) {
                Platform_SetClockRate(focus_clock_rate);
                focus_paused = 0;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED: open_gamepad(event.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close_gamepad(event.gdevice.which); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.y >= Menu_Height() && menu_event.button >= 2 && menu_event.button <= 3) {
                mouse_bits = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                 ? (uint16_t)(mouse_bits | mouse_buttons[menu_event.button])
                                 : (uint16_t)(mouse_bits & ~mouse_buttons[menu_event.button]);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (menu_event.wheel) {
                wheel_bits = menu_event.wheel > 0 ? 0x0010 : 0x0040; /* one notch: a short tap */
                wheel_frames = 3;
            }
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            SDL_Keycode key = event.key.key;
            int down = event.type == SDL_EVENT_KEY_DOWN;
            /* The shortcuts are the Game list's bindings (host_actions.c,
             * after this loop); only these keys stay fixed. */
            if (event.key.repeat) {
                break;
            }
            /* Alt+Enter (either Enter), beside the Fullscreen binding. The
             * press stops here, so the Enter under Alt never reaches the pad
             * as Start; Alt itself is a reserved modifier (controls.c). */
            if (down && (key == SDLK_RETURN || key == SDLK_KP_ENTER) && (event.key.mod & SDL_KMOD_ALT) &&
                Platform_HasWindowModes()) {
                int on = covers_screen();
                Settings_Set(SET_FULLSCREEN, !on);
                if (on) Settings_Set(SET_BORDERLESS, 0);
                Settings_Save();
                Platform_ApplyDisplaySettings();
                break;
            }
#ifdef SDL_PLATFORM_ANDROID
            /* Back (android.c traps it): the deck slot screen closes, else
             * the game asks before it quits, as closing a window does. An
             * open menu or a notice took it above, as Esc. */
            if (key == SDLK_AC_BACK) {
                if (down && DeckMenu_Active()) DeckMenu_Close();
                else if (down) QuitPrompt_Back(&quit);
                break;
            }
#endif
            if (down && key == SDLK_ESCAPE && DeckMenu_Active()) {
                DeckMenu_Close(); /* the deck slot screen, not the game */
                break;
            }
            /* Esc leaves fullscreen first, then borderless; in a window it is
             * only the Exit game control's default key. */
            if (down && key == SDLK_ESCAPE && covers_screen() && Platform_HasWindowModes()) {
                Settings_Set(Settings_Get(SET_FULLSCREEN) ? SET_FULLSCREEN : SET_BORDERLESS, 0);
                Settings_Save();
                Platform_ApplyDisplaySettings();
                break;
            }
            ControlsRuntime_Key(controls_key(event.key.scancode),down);
            break;
        }
        default: break;
        }
    }
    if (mods_window && mods_dirty) draw_mods();
    mods_dirty = 0;
    if (TouchPad_SetMode(Settings_Get(SET_TOUCH_PAD))) menu_dirty = 1;
    /* Once per pump, after every event of it: SDL delivers a tap as finger
     * events and mouse events made of them, and the pad yielding or coming
     * back between the two would hand one half to the menu and the other to
     * the pad. MENU opens the Game menu and STATES the File menu (save and
     * load a state, its slot); SPEED steps Game speed through x1 to x4, back
     * to x1 after x4 or from any other speed, and saves it as the menu does.
     * While a menu, a notice or the bar drawn over the picture is up, the
     * pad steps aside (touch_pad.h). */
    if (TouchPad_TakeMenu()) {
        Menu_OpenAt(MENU_BAR_GAME);
        menu_dirty = 1;
    }
    if (TouchPad_TakeStates()) {
        Menu_OpenAt(MENU_BAR_FILE);
        menu_dirty = 1;
    }
    if (TouchPad_TakeSpeed()) {
        int speed = Settings_Get(SET_SPEED);
        Settings_Set(SET_SPEED, speed >= 100 && speed < 400 ? (speed / 100 + 1) * 100 : 100);
        Settings_Save();
        menu_dirty = 1;
    }
    update_menu_visibility();
    if (TouchPad_Block(Menu_IsOpen() || Menu_NoticeShown() || (bar_overlays() && menu_visible) || Panel_Shown()))
        menu_dirty = 1;
    touch_bits = TouchPad_Update();
    Gamepad_Poll(current_frame);
    if (HostActions_Run(&quit)) menu_dirty = 1;
    /* A notice answers a controller and keeps the game's input at rest. */
    ControlsRuntime_Hold(Menu_NoticeShown());
    if (Menu_NoticePad(ControlsRuntime_TakePadPresses() | TouchPad_TakePresses(), &quit)) menu_dirty = 1;
    if (Panel_Shown()) {
        if (Panel_Tick()) menu_dirty = 1;
        if (!Panel_Shown()) panel_after(0);
    }
    if(controls_window) {
        static uint64_t last_draw;
        ControlsWindow_Tick();
        if(ControlsWindow_ShouldClose())close_controls();
        else if(ControlsRuntime_Now()-last_draw>=16000){draw_controls();last_draw=ControlsRuntime_Now();}
    }

}

#ifdef _WIN32
/* MEMORIES_GLES=1 on Windows: the ES context must be the driver's own
 * (WGL_EXT_create_context_es2_profile). Where the driver has no ES profile,
 * or one older than asked, SDL falls back to an EGL library found on the
 * search path (WIN_GL_UseEGL), which can be ANGLE's copy from any other
 * program; so the profile is checked first, from the desktop context, as
 * SDL checks it (SDL_GL_DeduceMaxSupportedESProfile): 3.0 or later where the
 * driver has GL_ARB_ES3_compatibility or a later one. */
typedef const char *(APIENTRY *WglExtensions)(void *hdc);

static int driver_has_es3(void)
{
    WglExtensions extensions = (WglExtensions)SDL_GL_GetProcAddress("wglGetExtensionsStringARB");
    void *hdc = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HDC_POINTER, NULL);
    const char *wgl = extensions && hdc ? extensions(hdc) : NULL;
    return wgl && strstr(wgl, "WGL_EXT_create_context_es2_profile") &&
           (SDL_GL_ExtensionSupported("GL_ARB_ES3_compatibility") ||
            SDL_GL_ExtensionSupported("GL_ARB_ES3_1_compatibility") ||
            SDL_GL_ExtensionSupported("GL_ARB_ES3_2_compatibility"));
}
#elif defined(__APPLE__)
static int driver_has_es3(void) { return 0; } /* macOS supplies desktop GL, not a native ES profile */
#else
static int driver_has_es3(void) { return 1; } /* GLX's ES profile, or the system's EGL (Mesa) */
#endif

/* The ES 3.0 context the renderer is to make (es_wanted). */
static void ask_for_es3(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
}

static void create_window(const char *title)
{
    const char *gles = getenv("MEMORIES_GLES");
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    update_menu_scale(240 * scale + 26 * Menu_AutoScale(240 * scale));
#ifdef __APPLE__
    /* The default macOS context is 2.1. Integer textures need 3.2 core,
     * with shader-based presentation instead of immediate-mode quads. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    /* Desktop GL where the system has it (platform.h); else, or without a
     * context, the SDL renderer. */
    window = Platform_HasDesktopGL() ? SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(),
                                                        flags | SDL_WINDOW_OPENGL) : NULL;
    gl_context = window ? SDL_GL_CreateContext(window) : NULL;
    use_gl = gl_context != NULL;
    /* OpenGL ES 3 for the renderer: always where the system's GL is GLES,
     * on a desktop only when asked (MEMORIES_GLES=1) and the driver has it. */
    es_wanted = !Platform_HasDesktopGL() || (use_gl && gles && *gles && strcmp(gles, "0") != 0);
    if (use_gl && es_wanted) {
        if (driver_has_es3()) {
            SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0"); /* the driver's ES profile, not an EGL library */
            SDL_GL_DestroyContext(gl_context);
            gl_context = NULL;
            use_gl = 0;
        } else {
            fprintf(stderr, "memories-pc: MEMORIES_GLES: this driver has no OpenGL ES 3 profile; desktop OpenGL it is\n");
            es_wanted = 0;
        }
    }
    /* macOS needs the core-profile quad presenter; the ES renderer uses its
     * own texture path and must not initialize desktop presentation state. */
    if (use_gl && !es_wanted && !PresentPass_InitCore()) {
        fprintf(stderr, "memories-pc: core presenter unavailable; falling back to SDL\n");
        SDL_GL_DestroyContext(gl_context);
        gl_context = NULL;
        use_gl = 0;
    }
    if (!use_gl) {
        if (window) SDL_DestroyWindow(window);
        window = NULL;
        if (es_wanted) {
            ask_for_es3();
            window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(), flags | SDL_WINDOW_OPENGL);
            if (!window) {
                fprintf(stderr, "memories-pc: no window for OpenGL ES 3 (%s)\n", SDL_GetError());
                SDL_GL_ResetAttributes();
                es_wanted = 0;
            }
        }
        if (!window) window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(), flags);
    }
}

/* The renderer's context is OpenGL ES 3: the pass in it, when start_pass. */
static void es_open(int start_pass)
{
    es_context = SDL_GL_GetCurrentContext();
    es_bind_framebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
    if (!es_context || !es_bind_framebuffer) {
        es_context = NULL;
        return;
    }
    es_enter();
    Monitor_Fact("gpu", "%s | %s | %s (SDL renderer %s)", (const char *)glGetString(GL_RENDERER),
                 (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_VERSION),
                 SDL_GetRendererName(renderer));
    LOG(LOG_WINDOW, "OpenGL ES renderer %s, version %s, video %s", glGetString(GL_RENDERER), glGetString(GL_VERSION),
        SDL_GetCurrentVideoDriver());
    es_picture = start_pass ? GlPicture_Init() : 0;
    es_leave();
}

/* The SDL renderer. On the OpenGL ES path SDL's opengles2 in the ES 3.0
 * context create_window asked for, with the pass in it; where that cannot be
 * had, or off that path, the renderer SDL picks, which shows the software
 * GPU's picture. start_pass 0: the pass is not started (reset_renderer). */
static void open_renderer(int start_pass)
{
    renderer = NULL;
    if (window && es_wanted) {
        renderer = SDL_CreateRenderer(window, "opengles2");
        if (renderer) {
            es_open(start_pass);
        } else {
            fprintf(stderr, "memories-pc: no OpenGL ES 3 renderer (%s); the software GPU draws the picture\n",
                    SDL_GetError());
            SDL_GL_ResetAttributes(); /* else SDL would ask for ES 3.0 again */
        }
    }
    if (!renderer && window && !use_gl) renderer = SDL_CreateRenderer(window, NULL);
}

/* SDL_EVENT_RENDER_DEVICE_RESET: the GL context was lost while the app was
 * away (Android), and SDL made a new one. SDL's renderer cannot go on in it
 * (its own context is the lost one), so it is made again, its textures with
 * it, and the pass in the new context, whose first replay draws the
 * picture again from VRAM; where the pass does not start again, the
 * software GPU draws the picture. A pass that was off (given up by
 * es_take_frame or an earlier reset) stays off: this runs inside a present
 * (pump, from begin_present) that may hold the software GPU's picture, and
 * GlPicture_Init's SoftGpu_SetRecorder would free it under the copy.
 * MEMORIES_TEST_GL_RESET=<frame> sends the
 * event at that frame, with nothing lost, to try this anywhere;
 * <frame>fail also keeps the pass from starting again. */
static void reset_renderer(void)
{
    int was_on = es_picture;
    fprintf(stderr, "memories-pc: the renderer's device was reset (%s); making it again\n",
            SDL_GetRendererName(renderer));
    GlPicture_Lost();
    es_picture = 0;
    es_context = NULL;
    SDL_DestroyRenderer(renderer); /* and the textures */
    picture = overlay = es_shown = NULL;
    es_shown_last = 0;
    pass_shown.valid = 0;
    picture_w = picture_h = 0;
    if (es_wanted) ask_for_es3();
    if (test_reset_fails) SDL_setenv_unsafe("MEMORIES_GL_PICTURE", "0", 1); /* the pass does not start again */
    open_renderer(was_on);
    if (!renderer) {
        fprintf(stderr, "memories-pc: SDL: %s\n", SDL_GetError());
        quit = 1;
        return;
    }
    if (!es_picture) { /* off, or not started again: the software GPU draws the picture */
        es_enter();
        GlPicture_Stop();
        pass_shown.valid = 0;
        es_leave();
    }
    Menu_SetHdPicture(es_picture);
    swap_interval = -1; /* set again on the new renderer */
    menu_dirty = 1;
}

static void destroy_window(void)
{
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    if (window) SDL_DestroyWindow(window);
    gl_context = NULL;
    window = NULL;
    use_gl = 0;
}

static int software_gl_renderer(void)
{
    const char *name = (const char *)glGetString(GL_RENDERER);
    return name && (strstr(name, "llvmpipe") || strstr(name, "softpipe") || strstr(name, "SwiftShader") ||
                    strstr(name, "Software Rasterizer"));
}

#ifndef SDL_PLATFORM_ANDROID /* android.c: the picker copies the image into the app */
typedef struct DiscSelection {
    SDL_AtomicInt done;
    char *path;
    size_t size;
    char *why;
    size_t why_size;
    int result;
} DiscSelection;

static void SDLCALL disc_selected(void *userdata, const char *const *files, int filter)
{
    DiscSelection *selection = userdata;
    (void)filter;
    if (!files) {
        snprintf(selection->why, selection->why_size,
                 "Could not open the ROM picker: %s\n\nYou can also put your .bin file in the game folder beside the program.",
                 SDL_GetError());
        selection->result = -1;
    } else if (files[0]) {
        if (strlen(files[0]) >= selection->size) {
            snprintf(selection->why, selection->why_size, "The ROM path is too long. Move it to a folder with a shorter path.");
            selection->result = -1;
        } else {
            snprintf(selection->path, selection->size, "%s", files[0]);
            selection->result = 1;
        }
    }
    /* SDL may invoke this callback on a worker thread. Publish only after
     * copying the path/error; the file list belongs to SDL. */
    SDL_SetAtomicInt(&selection->done, 1);
}

int Platform_SelectDisc(char *path, size_t size, char *why, size_t why_size)
{
    static const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose ROM..."}
    };
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image (.bin)", "bin;BIN"}};
    const SDL_MessageBoxData welcome = {
        SDL_MESSAGEBOX_INFORMATION, NULL, "Welcome to Forbidden Memories Recompiled",
        "Choose your copy of Yu-Gi-Oh! Forbidden Memories to begin.\n\n"
        "Select the .bin ROM from your USA disc (SLUS-01411).\n"
        "Your ROM stays where it is. We'll remember its location for next time.\n\n"
        "If you moved a previously selected ROM, choose its new location.",
        2, buttons, NULL
    };
    DiscSelection selection = {0};
    int button = 0;
    Monitor_Modal(1);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        snprintf(why, why_size, "Could not open ROM setup: %s", SDL_GetError());
        Monitor_Modal(0);
        return -1;
    }
    if (!SDL_ShowMessageBox(&welcome, &button)) {
        snprintf(why, why_size, "Could not open ROM setup: %s", SDL_GetError());
        selection.result = -1;
    } else if (button == 1) {
        selection.path = path;
        selection.size = size;
        selection.why = why;
        selection.why_size = why_size;
        SDL_ShowOpenFileDialog(disc_selected, &selection, NULL, filters, 1, NULL, false);
        while (!SDL_GetAtomicInt(&selection.done)) {
            SDL_PumpEvents(); /* Linux portal replies need the event loop. */
            SDL_Delay(10);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    Monitor_Modal(0);
    return selection.result;
}
#endif

void HERE(Platform_ShowError)(const char *title, const char *message)
{
    const char *headless = getenv("MEMORIES_HEADLESS");
    fprintf(stderr, "memories-pc: %s\n", message);
    Monitor_Shared()->error_shown = 1;
    if (!headless || !*headless || !strcmp(headless, "0")) {
        Monitor_Modal(1);
#ifdef __ANDROID__
        {
            /* SDL_ShowSimpleMessageBox's one button, in the system's alert */
            static const SDL_MessageBoxButtonData ok[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"}};
            const SDL_MessageBoxData box = {SDL_MESSAGEBOX_ERROR, NULL, title, message, 1, ok, NULL};
            int button;
            Android_ShowMessageBox(&box, &button);
        }
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
#endif
        Monitor_Modal(0);
    }
}

int Platform_Open(const char *title)
{
    sigset_t previous;
    ControlsRuntime_Init();
    Menu_LoadSettings(); /* the volume and scale apply with or without a window */
    if (getenv("MEMORIES_HEADLESS")) {
        return 0;
    }
    /* Let SDL select the native backend. In particular, do not force an
     * XWayland window on a Wayland desktop: that gives the window a separate
     * cursor/DPI scale from the rest of the desktop. SDL_VIDEODRIVER remains
     * available for diagnostics and compatibility overrides. */
    block_signals(&previous);
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0"); /* pump asks first */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        restore_signals(&previous);
        fprintf(stderr, "memories-pc: SDL: %s; set MEMORIES_HEADLESS=1 to run without a window\n", SDL_GetError());
        return -1;
    }
    SDL_AddEventWatch(app_event, NULL);
    create_window(title);
    /* A software GL renderer cannot present a scaled 4K frame in the 4 ms a
     * 400% game frame allows. The 32-bit build on an NVIDIA Wayland desktop
     * gets llvmpipe through Mesa's EGL while the X11 (XWayland) path reaches
     * the real driver, so retry there. SDL_VIDEODRIVER pins a choice. */
    if (use_gl && software_gl_renderer() && !getenv("SDL_VIDEODRIVER") &&
        SDL_GetCurrentVideoDriver() && !strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        const char *software = (const char *)glGetString(GL_RENDERER);
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        fprintf(stderr, "memories-pc: OpenGL on wayland is %s (software); retrying with the x11 driver\n",
                software ? software : "unknown");
        known_refresh = mode ? mode->refresh_rate : 0.0f; /* XWayland will not know it */
        destroy_window();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, NULL);
            SDL_InitSubSystem(SDL_INIT_VIDEO);
        }
        create_window(title);
    }
    if (window) {
        int x = Settings_Get(SET_WINDOW_X), y = Settings_Get(SET_WINDOW_Y);
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    open_renderer(1);
    restore_signals(&previous);
    if (!use_gl && !renderer) {
        fprintf(stderr, "memories-pc: SDL: %s\n", SDL_GetError());
        return -1;
    }
    update_display_refresh();
    Monitor_Fact("video", "SDL %d.%d.%d, video %s, audio %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                 SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                 SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    if (use_gl) {
        Monitor_Fact("gpu", "%s | %s | OpenGL %s", (const char *)glGetString(GL_RENDERER),
                     (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_VERSION));
        LOG(LOG_WINDOW, "OpenGL renderer %s, version %s, video %s", glGetString(GL_RENDERER),
            glGetString(GL_VERSION), SDL_GetCurrentVideoDriver());
        Menu_SetHdPicture(GlPicture_Init());
    } else if (es_context) {
        Menu_SetHdPicture(es_picture); /* es_open said the rest */
    } else {
        Monitor_Fact("gpu", "no OpenGL: SDL renderer %s", SDL_GetRendererName(renderer));
        LOG(LOG_WINDOW, "SDL fallback renderer %s, video %s", SDL_GetRendererName(renderer), SDL_GetCurrentVideoDriver());
    }
    Menu_SetPresentPass(use_gl); /* present_pass.c: the desktop presenter's alone */
    Menu_Init();
#ifdef SDL_PLATFORM_ANDROID
    /* One window, always the whole screen, no update check (android.c):
     * the window's size and mode and the update check are dimmed; Mods and
     * Controls open as panels inside the window. */
    Menu_SetPlatformItems(1, 0, 0);
#endif
    apply_display_settings();
    menu_visible = !covers_screen() || Settings_Get(SET_SHOW_MENU_FULLSCREEN);
    Menu_SetVisible(menu_visible);
#ifdef SDL_PLATFORM_ANDROID
    /* A crash in an earlier run: its report is offered to share or save,
     * since the player cannot reach the reports folder (android_report.c). */
    Android_OfferCrashReport();
#endif
    return 0;
}

/* The OpenGL pass's replay (gl_picture.h), in the renderer's context on
 * OpenGL ES. */
static int replay(void)
{
    int drawn;
    es_enter();
    drawn = GlPicture_Replay();
    es_leave();
    return drawn;
}

/* Before a frame: input and the menu, then any window change. */
static int pumped; /* this frame's pump ran already: the OpenGL pass could not show it (Platform_PresentPicture) */

static void begin_present(int w, int h, int at_scale)
{
    if (pumped) pumped = 0;
    else { /* before the frame, so its input and menu state are current */
        uint64_t started = phase_start();
        pump();
        phase_end(PHASE_EVENTS, started);
    }
#ifdef SDL_PLATFORM_ANDROID
    java_test_pump(current_frame);
#endif
    CrashTest_Present(); /* MEMORIES_CRASH_TEST=present */
    if (pending_scale) {
        scale = pending_scale;
        pending_scale = 0;
        display_settings_pending = 1;
    }
    /* The internal resolution changes the picture, not the frame the
     * window is laid out for: the movie at 1x beside the game at 2x keeps
     * the window where it is. */
    if (picture_scale != at_scale || logical_w != w / at_scale || logical_h != h / at_scale) {
        picture_scale = at_scale;
        logical_w = w / at_scale;
        logical_h = h / at_scale;
        relayout();
    }
    if (display_settings_pending) {
        display_settings_pending = 0;
        apply_display_settings();
    }
    resize(w, h);
}

/* After the picture's pixels are in place: to the texture and the window. */
static void finish_present(int w, int h)
{
    uint64_t started = phase_start();
    if (use_gl) {
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, picture_pixels);
    } else {
        SDL_UpdateTexture(picture, NULL, picture_pixels, w * 4);
        es_shown_last = 0; /* this one shows instead of the pass's */
    }
    phase_end(PHASE_COPY, started);
    pass_shown.valid = 0; /* F12 saves picture_pixels, which this frame filled */
    compose_menu_if_changed();
    show();
}

int HERE(Platform_PresentPicture)(const uint32_t *pixels, int stride, int x, int y, int w, int h, int at_scale)
{
    int j;
    if ((!renderer && !use_gl) || w <= 0 || h <= 0 || at_scale < 1) return 0;
    if (!pixels) {
        /* The OpenGL pass's picture: replayed now, shown from its texture. */
        if (!gl_pass()) return 0;
        begin_present(w, h, at_scale);
        if (!replay() || GlPicture_Scale() != at_scale) {
            pumped = 1; /* the caller shows VRAM instead: one pump a frame */
            return 0;
        }
        gl_pass_shown = 1;
        gl_pass_rect[0] = x;
        gl_pass_rect[1] = y;
        gl_pass_rect[2] = w;
        gl_pass_rect[3] = h;
        if (es_context && !es_take_frame()) {
            gl_pass_shown = 0;
            pumped = 1;
            return 0;
        }
        pass_shown.valid = 1;
        pass_shown.texture = 0;
        pass_shown.x = x;
        pass_shown.y = y;
        pass_shown.w = w;
        pass_shown.h = h;
        compose_menu_if_changed();
        show();
        gl_pass_shown = 0;
        return 1;
    }
    if (x < 0 || x + w > stride) return 0;
    if (gl_pass()) replay(); /* keep the normal picture current during widescreen */
    begin_present(w, h, at_scale);
    for (j = 0; j < h; j++) { /* the picture wraps at the bottom of VRAM, as VRAM does */
        memcpy(picture_pixels + (size_t)j * (size_t)w,
               pixels + (size_t)((y + j) & (512 * at_scale - 1)) * (size_t)stride + x, (size_t)w * 4);
    }
    finish_present(w, h);
    return 1;
}

void HERE(Platform_Present)(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24)
{
    int i, j;
    if ((!renderer && !use_gl) || w <= 0 || h <= 0) {
        return;
    }
    if (gl_pass()) replay(); /* a frame shown from VRAM (a movie, a widescreen frame) still keeps the pass in step */
    begin_present(w, h, 1);
    for (j = 0; j < h; j++) {
        const uint16_t *row = vram + ((y + j) & 511) * stride;
        uint32_t *out = picture_pixels + (size_t)j * (size_t)w;
        for (i = 0; i < w; i++) {
            if (rgb24) {
                const uint8_t *bytes = (const uint8_t *)(row + x) + i * 3;
                out[i] = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
            } else {
                uint16_t c = row[(x + i) & 1023];
                uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
                out[i] = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
        }
    }
    finish_present(w, h);
}

int Platform_ReadPicture(uint32_t *out, int x, int y, int w, int h)
{
    int read;
    if (!gl_pass()) return 0;
    es_enter();
    GlPicture_Replay(); /* what was recorded since the last present */
    read = GlPicture_Read(x, y, w, h, out);
    es_leave();
    return read;
}

int HERE(Platform_PresentWidePicture)(int x, int y, int w, int h, int wide_w, int at_scale)
{
    GLuint texture = 0;
    int pw, ph, replayed;
    if (!gl_pass() || at_scale < 2) return 0;
    begin_present(wide_w * at_scale, h * at_scale, at_scale);
    es_enter();
    replayed = GlPicture_Replay() && GlPicture_Scale() == at_scale;
    if (replayed) texture = (GLuint)GlPicture_WideTexture(x, y, w, h, &pw, &ph);
    es_leave();
    if (!texture) {
        pumped = 1; /* the caller shows another picture: one pump a frame */
        return 0;
    }
    gl_pass_shown = 1;
    gl_pass_texture = texture;
    gl_pass_size[0] = pw;
    gl_pass_size[1] = ph;
    gl_pass_rect[0] = 0;
    gl_pass_rect[1] = 0;
    gl_pass_rect[2] = pw;
    gl_pass_rect[3] = h * at_scale;
    if (es_context && !es_take_frame()) {
        gl_pass_shown = 0;
        gl_pass_texture = 0;
        pumped = 1;
        return 0;
    }
    pass_shown.valid = 1;
    pass_shown.texture = texture;
    pass_shown.x = pass_shown.y = 0;
    pass_shown.w = pw;
    pass_shown.h = h * at_scale;
    compose_menu_if_changed();
    show();
    gl_pass_shown = 0;
    gl_pass_texture = 0;
    return 1;
}

int Platform_ReadWidePicture(uint32_t *out, int x, int y, int w, int h, int wide_w, int scale)
{
    int read;
    if (!gl_pass()) return 0;
    es_enter();
    GlPicture_Replay();
    read = GlPicture_ReadWide(x, y, w, h, wide_w, scale, out);
    es_leave();
    return read;
}

int Platform_ShouldQuit(void) { return quit; }
void Platform_RequestQuit(void) { quit = 1; }
int Platform_Widescreen(void) { return Settings_Get(SET_ASPECT) == 2; }
int Platform_StateSlot(void) { return state_slot; }
void Platform_SetStateSlot(int slot)
{
    if (slot < 1 || slot > 4) return;
    state_slot = slot;
}
/* No frame is coming (paused, or a frame that is not shown): answer the
 * menu now, at most once per call. */
void HERE(Platform_PumpEvents)(void)
{
    static uint64_t last_repaint_us;
    uint64_t now;
    if (!window) return;
    pump();
    Update_Frame();
    if (Menu_TakeChanged()) menu_dirty = 1;
    if (foreground_repaint_until && real_now_us() < foreground_repaint_until) menu_dirty = 1;
    else foreground_repaint_until = 0;
    if (gl_pass() && GlPicture_Behind()) replay();
    /* A running game shows the change with its next frame; paused, the wait
     * loop pumps every half millisecond, so keep hover repaints to ~120/s. */
    if (menu_dirty && overlay_pixels && Platform_ClockRate() == 0 &&
        (now = real_now_us()) - last_repaint_us >= 8000) {
        last_repaint_us = now;
        repaint_menu();
    }
}

uint16_t Platform_Pad(int port)
{
    return port == 0 ? (uint16_t)(ControlsRuntime_Keyboard() | (ControlsRuntime_Blocked()?0:(mouse_bits | wheel_now | touch_bits)) | scripted_bits | Gamepad_Bits(0))
                     : (uint16_t)(Gamepad_Bits(1) | scripted_bits2);
}

uint16_t Platform_PadFixedBits(int port)
{
    return port == 0 ? (uint16_t)((ControlsRuntime_Blocked()?0:(mouse_bits | wheel_now | touch_bits)) | scripted_bits) : scripted_bits2;
}

int Platform_PadConnected(int port) { return port == 0 || Gamepad_Connected(port) || Platform_ScriptedPad2(); }

/* MEMORIES_SDL_SCRIPT="200:click:20:13,260:move:60:69,420:key:escape": events
 * pushed into SDL's queue at presented frames, for testing the menu without
 * a pointer (external synthetic X input does not reach SDL correctly).
 * "key" presses and releases at once; "keydown" and "keyup" hold a key
 * between two frames, so the game sees it ("600:keydown:s,606:keyup:s").
 * "tap:x:y", "fdown:x:y" and "fup:x:y" do the same with a finger, for the
 * touch controls. "300:shot" saves the composed window (menu included)
 * like Shift+F12. */
static void run_event_script(unsigned frame)
{
    static const char *script;
    static int loaded;
    if (!loaded) {
        loaded = 1;
        script = getenv("MEMORIES_SDL_SCRIPT");
    }
    while (script && *script) {
        char *end, kind[8];
        unsigned long at = strtoul(script, &end, 10);
        SDL_Event event;
        int x = 0, y = 0;
        size_t n;
        if (*end != ':' || at > frame) {
            return;
        }
        script = end + 1;
        n = strcspn(script, ":,");
        snprintf(kind, sizeof(kind), "%.*s", (int)(n < 7 ? n : 7), script);
        script += n;
        memset(&event, 0, sizeof(event));
        if (strcmp(kind, "shot") == 0) {
            Platform_Screenshot(1);
        } else if (strcmp(kind, "key") == 0 || strcmp(kind, "keydown") == 0 || strcmp(kind, "keyup") == 0 ||
                   strcmp(kind, "down") == 0 || strcmp(kind, "up") == 0) {
            /* "key" presses and releases in one go, a tap the game sees for
             * one pad update (controls_runtime.c); "keydown"/"keyup" (or
             * "down"/"up") hold a key across frames. */
            const int press = strcmp(kind, "keyup") != 0 && strcmp(kind, "up") != 0;
            const int release = strcmp(kind, "keydown") != 0 && strcmp(kind, "down") != 0;
            const char *name = script + 1;
            n = strcspn(name, ",");
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.down = true;
            /* "alt+<name>": the same key with Left Alt held. */
            if (n > 4 && strncmp(name, "alt+", 4) == 0) {
                event.key.mod = SDL_KMOD_LALT;
                name += 4;
                n -= 4;
            }
            event.key.key = strncmp(name, "escape", n) == 0 ? SDLK_ESCAPE : strncmp(name, "f10", n) == 0 ? SDLK_F10
                          : strncmp(name, "f3", n) == 0 ? SDLK_F3
                          : strncmp(name, "f5", n) == 0 ? SDLK_F5
                          : strncmp(name, "f6", n) == 0 ? SDLK_F6
                          : strncmp(name, "f7", n) == 0 ? SDLK_F7
                          : strncmp(name, "f8", n) == 0 ? SDLK_F8
                          : strncmp(name, "f11", n) == 0 ? SDLK_F11
                          : strncmp(name, "f12", n) == 0 ? SDLK_F12
                          : strncmp(name, "left", n) == 0 ? SDLK_LEFT : strncmp(name, "right", n) == 0 ? SDLK_RIGHT
                          : strncmp(name, "up", n) == 0 ? SDLK_UP : strncmp(name, "down", n) == 0 ? SDLK_DOWN
                          : strncmp(name, "return", n) == 0 ? SDLK_RETURN
                          : strncmp(name, "tab", n) == 0 ? SDLK_TAB : strncmp(name, "p", n) == 0 ? SDLK_P
                          : strncmp(name, "m", n) == 0 ? SDLK_M
                          : strncmp(name, "period", n) == 0 ? SDLK_PERIOD
                          : strncmp(name, "kp_plus", n) == 0 ? SDLK_KP_PLUS
                          : strncmp(name, "kp_minus", n) == 0 ? SDLK_KP_MINUS
                          : strncmp(name, "s", n) == 0 ? SDLK_S /* Circle */
                          : strncmp(name, "x", n) == 0 ? SDLK_X /* Cross */
                          : SDLK_UNKNOWN;
            event.key.scancode = SDL_GetScancodeFromKey(event.key.key, NULL);
            event.key.windowID = SDL_GetWindowID(window);
            if (press) SDL_PushEvent(&event);
            event.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            if (release) SDL_PushEvent(&event);
            script = name + n;
        } else if (strcmp(kind, "tap") == 0 || strcmp(kind, "fdown") == 0 || strcmp(kind, "fup") == 0) {
            /* A finger at window pixels x, y (the touch controls): "tap"
             * lands and lifts in one go, "fdown"/"fup" hold it across frames. */
            x = (int)strtol(script + 1, &end, 10);
            y = (int)strtol(end + 1, &end, 10);
            script = end;
            event.type = SDL_EVENT_FINGER_DOWN;
            event.tfinger.fingerID = 1;
            event.tfinger.x = layout.win_w > 0 ? (float)x / (float)layout.win_w : 0;
            event.tfinger.y = layout.win_h > 0 ? (float)y / (float)layout.win_h : 0;
            event.tfinger.windowID = SDL_GetWindowID(window);
            if (strcmp(kind, "fup") != 0) SDL_PushEvent(&event);
            event.type = SDL_EVENT_FINGER_UP;
            if (strcmp(kind, "fdown") != 0) SDL_PushEvent(&event);
        } else {
            x = (int)strtol(script + 1, &end, 10);
            y = (int)strtol(end + 1, &end, 10);
            script = end;
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = (float)x;
            event.motion.y = (float)y;
            event.motion.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&event);
            if (strcmp(kind, "click") == 0) {
                memset(&event, 0, sizeof(event));
                event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.down = true;
                event.button.clicks = 1;
                event.button.x = (float)x;
                event.button.y = (float)y;
                event.button.windowID = SDL_GetWindowID(window);
                SDL_PushEvent(&event);
                event.type = SDL_EVENT_MOUSE_BUTTON_UP;
                event.button.down = false;
                SDL_PushEvent(&event);
            }
        }
        if (*script == ',') {
            script++;
        }
    }
}

/* The window's icon: the game's memory card icon (save_icon.h), once the
 * game is in memory, at 16 pixels and pixel-doubled for larger taskbars. */
static void set_icon(void)
{
    static int set;
    uint8_t small[SAVE_ICON_SIZE * SAVE_ICON_SIZE * 4];
    SDL_Surface *icon;
    int times;
    if (set || !window || !SaveIcon_Rgba(0, small)) return;
    set = 1;
    icon = SDL_CreateSurface(SAVE_ICON_SIZE, SAVE_ICON_SIZE, SDL_PIXELFORMAT_RGBA32);
    if (!icon) return;
    memcpy(icon->pixels, small, sizeof(small)); /* 16 x 4 bytes a row: no padding */
    for (times = 2; times <= 4; times++) {
        SDL_Surface *large = SDL_CreateSurface(SAVE_ICON_SIZE * times, SAVE_ICON_SIZE * times, SDL_PIXELFORMAT_RGBA32);
        int x, y;
        if (!large) continue;
        for (y = 0; y < SAVE_ICON_SIZE * times; y++) {
            for (x = 0; x < SAVE_ICON_SIZE * times; x++) {
                memcpy((uint8_t *)large->pixels + y * large->pitch + x * 4,
                       small + ((y / times) * SAVE_ICON_SIZE + x / times) * 4, 4);
            }
        }
        SDL_AddSurfaceAlternateImage(icon, large);
        SDL_DestroySurface(large);
    }
    if (!SDL_SetWindowIcon(window, icon)) LOG(LOG_WINDOW, "window icon: %s", SDL_GetError());
    else LOG(LOG_WINDOW, "window icon: the game's memory card icon");
    SDL_DestroySurface(icon);
}

void HERE(Platform_Frame)(unsigned frame)
{
    static unsigned composed_then;
    current_frame = frame;
    set_icon();
    Log_Drain();
    if (frame % 120 == 0) {
        LOG(LOG_WINDOW, "overlay composed %u times in 120 frames", compose_count - composed_then);
        composed_then = compose_count;
    }
    Gamepad_Poll(frame);
    Cheats_Frame();
    Cards_Frame();
    Duelists_Frame();
    Tower_Frame();
    Update_Frame();
    if (Menu_TakeChanged()) menu_dirty = 1;
    if (window) {
        run_event_script(frame);
    }
    if (menu_reveal_frames > 0) menu_reveal_frames--;
    update_menu_visibility();
#ifdef SDL_PLATFORM_ANDROID
    java_test(frame);
#ifdef __aarch64__
    if (frame % 1000 == 0) {
        size_t painted = 0, stack_size = 0, used = Memories_HostStackUsed(&painted, &stack_size);
        if (used) fprintf(stderr, "memories-pc: host stack at frame %u: %zu bytes used at most below the switch "
                          "point (%zu painted; thread stack %zu)\n", frame, used, painted, stack_size);
    }
#endif
#endif
    if (window && Settings_Get(SET_HIDE_CURSOR) && pointer_inside && !cursor_hidden &&
        pointer_x >= layout.dst.x && pointer_x < layout.dst.x + layout.dst.w &&
        pointer_y >= layout.dst.y && pointer_y < layout.dst.y + layout.dst.h &&
        frame - last_pointer_motion >= 120) {
        SDL_HideCursor();
        cursor_hidden = 1;
    } else if ((!Settings_Get(SET_HIDE_CURSOR) || pointer_y < Menu_Height()) && cursor_hidden) {
        show_cursor();
    }
    wheel_now = wheel_frames > 0 && wheel_frames-- ? wheel_bits : 0;
    scripted_bits = Platform_ScriptedBits(frame);
    scripted_bits2 = Platform_ScriptedBits2(frame);
}

#ifdef SDL_PLATFORM_ANDROID
/* The entry points named by HERE above, on the thread's own stack. */
typedef struct {
    const char *text, *more;
    int number;
    int result;
} HostCall;

/* A present's arguments (Platform_Present*, Platform_StartAudio). */
typedef struct {
    const void *pixels;
    int a[7];
    void (*mix)(int16_t *, size_t);
    int result;
} PresentCall;

static void frame_on_host(void *call) { Platform_Frame_here((unsigned)((HostCall *)call)->number); }
static void pump_on_host(void *call) { (void)call; Platform_PumpEvents_here(); }
static void show_error_on_host(void *call) { Platform_ShowError_here(((HostCall *)call)->text, ((HostCall *)call)->more); }
static void open_url_on_host(void *call) { ((HostCall *)call)->result = Platform_OpenUrl_here(((HostCall *)call)->text); }
static void open_folder_on_host(void *call)
{
    ((HostCall *)call)->result = Platform_OpenFolder_here(((HostCall *)call)->text);
}
static void copy_text_on_host(void *call) { ((HostCall *)call)->result = Platform_CopyText_here(((HostCall *)call)->text); }
static void open_mods_on_host(void *call) { (void)call; Platform_OpenMods_here(); }
static void open_controls_on_host(void *call) { (void)call; Platform_OpenControls_here(); }
static void present_on_host(void *call)
{
    PresentCall *c = call;
    Platform_Present_here(c->pixels, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5]);
}
static void present_picture_on_host(void *call)
{
    PresentCall *c = call;
    c->result = Platform_PresentPicture_here(c->pixels, c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5]);
}
static void present_wide_on_host(void *call)
{
    PresentCall *c = call;
    c->result = Platform_PresentWidePicture_here(c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5]);
}
static void start_audio_on_host(void *call)
{
    PresentCall *c = call;
    c->result = Platform_StartAudio_here(c->mix);
}

void Platform_Frame(unsigned frame)
{
    HostCall call = {NULL, NULL, (int)frame, 0};
    /* MEMORIES_TEST_JAVA=<frame>guard: the guard itself, called here on the
     * game stack as a missed path would (no Java call is made), so its log
     * line and count can be seen to work. */
    if (java_test_now(frame) && java_test_guard) Memories_JniGuard("MEMORIES_TEST_JAVA=<frame>guard");
    Memories_OnHostStack(frame_on_host, &call);
}
void Platform_PumpEvents(void) { Memories_OnHostStack(pump_on_host, NULL); }
void Platform_ShowError(const char *title, const char *message)
{
    HostCall call = {title, message, 0, 0};
    Memories_OnHostStack(show_error_on_host, &call);
}
int Platform_OpenUrl(const char *url)
{
    HostCall call = {url, NULL, 0, -1};
    Memories_OnHostStack(open_url_on_host, &call);
    return call.result;
}
int Platform_OpenFolder(const char *path)
{
    HostCall call = {path, NULL, 0, -1};
    Memories_OnHostStack(open_folder_on_host, &call);
    return call.result;
}
int Platform_CopyText(const char *text)
{
    HostCall call = {text, NULL, 0, 0};
    Memories_OnHostStack(copy_text_on_host, &call);
    return call.result;
}
void Platform_OpenMods(void) { Memories_OnHostStack(open_mods_on_host, NULL); }
void Platform_OpenControls(void) { Memories_OnHostStack(open_controls_on_host, NULL); }
void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24)
{
    PresentCall call = {vram, {stride, x, y, w, h, rgb24, 0}, NULL, 0};
    Memories_OnHostStack(present_on_host, &call);
}
int Platform_PresentPicture(const uint32_t *pixels, int stride, int x, int y, int w, int h, int at_scale)
{
    PresentCall call = {pixels, {stride, x, y, w, h, at_scale, 0}, NULL, 0};
    Memories_OnHostStack(present_picture_on_host, &call);
    return call.result;
}
int Platform_PresentWidePicture(int x, int y, int w, int h, int wide_w, int at_scale)
{
    PresentCall call = {NULL, {x, y, w, h, wide_w, at_scale, 0}, NULL, 0};
    Memories_OnHostStack(present_wide_on_host, &call);
    return call.result;
}
int Platform_StartAudio(void (*mix)(int16_t *, size_t))
{
    PresentCall call = {NULL, {0}, mix, 0};
    Memories_OnHostStack(start_audio_on_host, &call);
    return call.result;
}
#endif
