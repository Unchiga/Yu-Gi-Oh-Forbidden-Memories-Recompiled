/* <SDL3/SDL.h> for the Switch: SDL3's names over SDL2 (sdl3_shim.h says
 * how). A name maps straight to SDL2 where only the name changed, else to
 * the shim's own. */
#ifndef MEMORIES_PC_COMPAT_SWITCH_SDL3_H
#define MEMORIES_PC_COMPAT_SWITCH_SDL3_H
#define SDL_MAIN_HANDLED
#include "sdl3_shim.h"

#define SDL_PLATFORM_SWITCH 1

/* --- types --------------------------------------------------------------- */
#define SDL_Event Shim_Event
#define SDL_DisplayMode Shim_DisplayMode
#define SDL_DisplayID Shim_DisplayID
#define SDL_PropertiesID Shim_PropertiesID
#define SDL_AudioSpec Shim_AudioSpec
#define SDL_AudioStream Shim_AudioStream
#define SDL_MessageBoxButtonData Shim_MessageBoxButtonData
#define SDL_MessageBoxData Shim_MessageBoxData
#define SDL_Gamepad SDL_GameController
#define SDL_GamepadAxis SDL_GameControllerAxis
#define SDL_GamepadButton int
#define SDL_GamepadType SDL_GameControllerType
#define SDL_AtomicInt SDL_atomic_t
#define SDL_WindowID Uint32
#undef SDL_WindowFlags
#define SDL_WindowFlags Uint64

/* --- events -------------------------------------------------------------- */
#define SDL_EVENT_QUIT SHIM_EVENT_QUIT
#define SDL_EVENT_WILL_ENTER_BACKGROUND SHIM_EVENT_WILL_ENTER_BACKGROUND
#define SDL_EVENT_DID_ENTER_BACKGROUND SHIM_EVENT_DID_ENTER_BACKGROUND
#define SDL_EVENT_WILL_ENTER_FOREGROUND SHIM_EVENT_WILL_ENTER_FOREGROUND
#define SDL_EVENT_DID_ENTER_FOREGROUND SHIM_EVENT_DID_ENTER_FOREGROUND
#define SDL_EVENT_WINDOW_EXPOSED SHIM_EVENT_WINDOW_EXPOSED
#define SDL_EVENT_WINDOW_MOVED SHIM_EVENT_WINDOW_MOVED
#define SDL_EVENT_WINDOW_RESIZED SHIM_EVENT_WINDOW_RESIZED
#define SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED SHIM_EVENT_WINDOW_PIXEL_SIZE_CHANGED
#define SDL_EVENT_WINDOW_MOUSE_ENTER SHIM_EVENT_WINDOW_MOUSE_ENTER
#define SDL_EVENT_WINDOW_MOUSE_LEAVE SHIM_EVENT_WINDOW_MOUSE_LEAVE
#define SDL_EVENT_WINDOW_FOCUS_GAINED SHIM_EVENT_WINDOW_FOCUS_GAINED
#define SDL_EVENT_WINDOW_FOCUS_LOST SHIM_EVENT_WINDOW_FOCUS_LOST
#define SDL_EVENT_WINDOW_CLOSE_REQUESTED SHIM_EVENT_WINDOW_CLOSE_REQUESTED
#define SDL_EVENT_WINDOW_DISPLAY_CHANGED SHIM_EVENT_WINDOW_DISPLAY_CHANGED
#define SDL_EVENT_KEY_DOWN SHIM_EVENT_KEY_DOWN
#define SDL_EVENT_KEY_UP SHIM_EVENT_KEY_UP
#define SDL_EVENT_TEXT_INPUT SHIM_EVENT_TEXT_INPUT
#define SDL_EVENT_KEYMAP_CHANGED SHIM_EVENT_KEYMAP_CHANGED
#define SDL_EVENT_MOUSE_MOTION SHIM_EVENT_MOUSE_MOTION
#define SDL_EVENT_MOUSE_BUTTON_DOWN SHIM_EVENT_MOUSE_BUTTON_DOWN
#define SDL_EVENT_MOUSE_BUTTON_UP SHIM_EVENT_MOUSE_BUTTON_UP
#define SDL_EVENT_MOUSE_WHEEL SHIM_EVENT_MOUSE_WHEEL
#define SDL_EVENT_GAMEPAD_BUTTON_DOWN SHIM_EVENT_GAMEPAD_BUTTON_DOWN
#define SDL_EVENT_GAMEPAD_ADDED SHIM_EVENT_GAMEPAD_ADDED
#define SDL_EVENT_GAMEPAD_REMOVED SHIM_EVENT_GAMEPAD_REMOVED
#define SDL_EVENT_FINGER_DOWN SHIM_EVENT_FINGER_DOWN
#define SDL_EVENT_FINGER_UP SHIM_EVENT_FINGER_UP
#define SDL_EVENT_FINGER_MOTION SHIM_EVENT_FINGER_MOTION
#define SDL_EVENT_FINGER_CANCELED SHIM_EVENT_FINGER_CANCELED
#define SDL_EVENT_RENDER_DEVICE_RESET SHIM_EVENT_RENDER_DEVICE_RESET

#define SDL_PollEvent Shim_PollEvent
#define SDL_PushEvent Shim_PushEvent
#define SDL_AddEventWatch Shim_AddEventWatch
#define SDL_GetWindowFromEvent Shim_GetWindowFromEvent

/* --- init, time, hints --------------------------------------------------- */
#define SDL_INIT_GAMEPAD SDL_INIT_GAMECONTROLLER
#define SDL_Init Shim_Init
#define SDL_InitSubSystem Shim_InitSubSystem
#define SDL_GetVersion Shim_GetVersion
#define SDL_VERSIONNUM_MAJOR(version) ((version) / 1000000)
#define SDL_VERSIONNUM_MINOR(version) (((version) / 1000) % 1000)
#define SDL_VERSIONNUM_MICRO(version) ((version) % 1000)
#define SDL_GetTicksNS Shim_GetTicksNS
#define SDL_setenv_unsafe setenv
#define SDL_HINT_VIDEO_DRIVER SDL_HINT_VIDEODRIVER
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES" /* read by Shim_OpenAudioDeviceStream */

/* --- windows and displays ------------------------------------------------ */
#define SDL_WINDOW_HIGH_PIXEL_DENSITY SDL_WINDOW_ALLOW_HIGHDPI
#define SDL_CreateWindow Shim_CreateWindow
#define SDL_GetWindowSize Shim_GetWindowSize
#define SDL_GetWindowSizeInPixels Shim_GetWindowSizeInPixels
#define SDL_SetWindowSize Shim_SetWindowSize
#define SDL_SetWindowPosition Shim_SetWindowPosition
#define SDL_SetWindowMinimumSize Shim_SetWindowMinimumSize
#define SDL_SetWindowBordered Shim_SetWindowBordered
#define SDL_SetWindowFullscreen Shim_SetWindowFullscreen
#define SDL_SetWindowFullscreenMode Shim_SetWindowFullscreenMode
#define SDL_SetWindowIcon Shim_SetWindowIcon
#define SDL_RaiseWindow Shim_RaiseWindow
#define SDL_GetWindowSafeArea Shim_GetWindowSafeArea
#define SDL_GetWindowProperties Shim_GetWindowProperties
#define SDL_PROP_WINDOW_WIN32_HDC_POINTER "SDL.window.win32.hdc"
#define SDL_StartTextInput Shim_StartTextInput
#define SDL_StopTextInput Shim_StopTextInput
#define SDL_ShowCursor Shim_ShowCursor
#define SDL_HideCursor Shim_HideCursor
#define SDL_GetPrimaryDisplay Shim_GetPrimaryDisplay
#define SDL_GetDisplayForWindow Shim_GetDisplayForWindow
#define SDL_GetDisplayContentScale Shim_GetDisplayContentScale
#define SDL_GetDisplayUsableBounds Shim_GetDisplayUsableBounds
#define SDL_GetCurrentDisplayMode Shim_GetCurrentDisplayMode
#define SDL_GetDesktopDisplayMode Shim_GetDesktopDisplayMode
#define SDL_GetFullscreenDisplayModes Shim_GetFullscreenDisplayModes
#define SDL_GetClosestFullscreenDisplayMode Shim_GetClosestFullscreenDisplayMode

/* --- renderer, textures, surfaces ---------------------------------------- */
#define SDL_CreateRenderer Shim_CreateRenderer
#define SDL_GetRendererName Shim_GetRendererName
#define SDL_GetRenderOutputSize Shim_GetRenderOutputSize
#define SDL_RenderTexture Shim_RenderTexture
#define SDL_RenderReadPixels Shim_RenderReadPixels
#define SDL_SetRenderVSync Shim_SetRenderVSync
#define SDL_FlushRenderer Shim_FlushRenderer
#define SDL_RenderClear(renderer) ((SDL_RenderClear)(renderer) == 0)
#define SDL_RenderPresent(renderer) ((SDL_RenderPresent)(renderer), true)
#define SDL_SetRenderDrawColor(renderer, r, g, b, a) ((SDL_SetRenderDrawColor)(renderer, r, g, b, a) == 0)
#define SDL_UpdateTexture(texture, rect, pixels, pitch) ((SDL_UpdateTexture)(texture, rect, pixels, pitch) == 0)
#define SDL_SetTextureBlendMode(texture, mode) ((SDL_SetTextureBlendMode)(texture, mode) == 0)
#define SDL_SetTextureScaleMode Shim_SetTextureScaleMode
#define SDL_SCALEMODE_NEAREST 0
#define SDL_SCALEMODE_LINEAR 1
#define SDL_GetTextureProperties Shim_GetTextureProperties
#define SDL_GetNumberProperty Shim_GetNumberProperty
#define SDL_GetPointerProperty Shim_GetPointerProperty
#define SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER "SDL.texture.opengles2.texture"
#define SDL_PIXELFORMAT_XRGB8888 SDL_PIXELFORMAT_RGB888
#define SDL_PIXELFORMAT_XBGR8888 SDL_PIXELFORMAT_BGR888
#define SDL_CreateSurface Shim_CreateSurface
#define SDL_CreateSurfaceFrom Shim_CreateSurfaceFrom
#define SDL_DestroySurface SDL_FreeSurface
#define SDL_AddSurfaceAlternateImage Shim_AddSurfaceAlternateImage
#undef SDL_SaveBMP
#define SDL_SaveBMP Shim_SaveBMP

/* --- OpenGL --------------------------------------------------------------- */
#define SDL_GL_MakeCurrent Shim_GL_MakeCurrent
#define SDL_GL_SetSwapInterval Shim_GL_SetSwapInterval
#define SDL_GL_SetAttribute Shim_GL_SetAttribute
#define SDL_GL_SwapWindow Shim_GL_SwapWindow
#define SDL_GL_DestroyContext SDL_GL_DeleteContext

/* --- gamepads ------------------------------------------------------------- */
#define SDL_GetGamepads Shim_GetGamepads
#define SDL_OpenGamepad Shim_OpenGamepad
#define SDL_CloseGamepad SDL_GameControllerClose
#define SDL_GetGamepadID Shim_GetGamepadID
#define SDL_GamepadConnected SDL_GameControllerGetAttached
#define SDL_GetGamepadName SDL_GameControllerName
#define SDL_GetGamepadPath SDL_GameControllerPath
#define SDL_GetGamepadSerial SDL_GameControllerGetSerial
#define SDL_GetGamepadVendor SDL_GameControllerGetVendor
#define SDL_GetGamepadProduct SDL_GameControllerGetProduct
#define SDL_GetGamepadType SDL_GameControllerGetType
#define SDL_GetGamepadButton Shim_GetGamepadButton
#define SDL_GetGamepadAxis SDL_GameControllerGetAxis
#define SDL_UpdateJoysticks SDL_JoystickUpdate
#define SDL_GAMEPAD_AXIS_LEFT_TRIGGER SDL_CONTROLLER_AXIS_TRIGGERLEFT
#define SDL_GAMEPAD_TYPE_XBOX360 SDL_CONTROLLER_TYPE_XBOX360
#define SDL_GAMEPAD_TYPE_XBOXONE SDL_CONTROLLER_TYPE_XBOXONE
#define SDL_GAMEPAD_TYPE_PS3 SDL_CONTROLLER_TYPE_PS3
#define SDL_GAMEPAD_TYPE_PS5 SDL_CONTROLLER_TYPE_PS5
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO
#define SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
/* SDL3's buttons are by place (SOUTH is the bottom face button), as SDL2's
 * Xbox names are once it stops swapping by label (switch_sdl3.c). SDL2 has
 * no MISC2-6: they are never down. */
#define SDL_GAMEPAD_BUTTON_INVALID SDL_CONTROLLER_BUTTON_INVALID
#define SDL_GAMEPAD_BUTTON_SOUTH SDL_CONTROLLER_BUTTON_A
#define SDL_GAMEPAD_BUTTON_EAST SDL_CONTROLLER_BUTTON_B
#define SDL_GAMEPAD_BUTTON_WEST SDL_CONTROLLER_BUTTON_X
#define SDL_GAMEPAD_BUTTON_NORTH SDL_CONTROLLER_BUTTON_Y
#define SDL_GAMEPAD_BUTTON_BACK SDL_CONTROLLER_BUTTON_BACK
#define SDL_GAMEPAD_BUTTON_GUIDE SDL_CONTROLLER_BUTTON_GUIDE
#define SDL_GAMEPAD_BUTTON_START SDL_CONTROLLER_BUTTON_START
#define SDL_GAMEPAD_BUTTON_LEFT_STICK SDL_CONTROLLER_BUTTON_LEFTSTICK
#define SDL_GAMEPAD_BUTTON_RIGHT_STICK SDL_CONTROLLER_BUTTON_RIGHTSTICK
#define SDL_GAMEPAD_BUTTON_LEFT_SHOULDER SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#define SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#define SDL_GAMEPAD_BUTTON_DPAD_UP SDL_CONTROLLER_BUTTON_DPAD_UP
#define SDL_GAMEPAD_BUTTON_DPAD_DOWN SDL_CONTROLLER_BUTTON_DPAD_DOWN
#define SDL_GAMEPAD_BUTTON_DPAD_LEFT SDL_CONTROLLER_BUTTON_DPAD_LEFT
#define SDL_GAMEPAD_BUTTON_DPAD_RIGHT SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#define SDL_GAMEPAD_BUTTON_MISC1 SDL_CONTROLLER_BUTTON_MISC1
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1 SDL_CONTROLLER_BUTTON_PADDLE1
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 SDL_CONTROLLER_BUTTON_PADDLE2
#define SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2 SDL_CONTROLLER_BUTTON_PADDLE3
#define SDL_GAMEPAD_BUTTON_LEFT_PADDLE2 SDL_CONTROLLER_BUTTON_PADDLE4
#define SDL_GAMEPAD_BUTTON_TOUCHPAD SDL_CONTROLLER_BUTTON_TOUCHPAD
#define SDL_GAMEPAD_BUTTON_MISC2 (SDL_CONTROLLER_BUTTON_MAX + 1)
#define SDL_GAMEPAD_BUTTON_MISC3 (SDL_CONTROLLER_BUTTON_MAX + 2)
#define SDL_GAMEPAD_BUTTON_MISC4 (SDL_CONTROLLER_BUTTON_MAX + 3)
#define SDL_GAMEPAD_BUTTON_MISC5 (SDL_CONTROLLER_BUTTON_MAX + 4)
#define SDL_GAMEPAD_BUTTON_MISC6 (SDL_CONTROLLER_BUTTON_MAX + 5)

/* --- audio ---------------------------------------------------------------- */
#define SDL_AUDIO_S16 AUDIO_S16SYS
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu
#define SDL_OpenAudioDeviceStream Shim_OpenAudioDeviceStream
#define SDL_ResumeAudioStreamDevice Shim_ResumeAudioStreamDevice
#define SDL_PutAudioStreamData Shim_PutAudioStreamData
#define SDL_GetAudioStreamQueued Shim_GetAudioStreamQueued

/* --- keyboard and mouse ------------------------------------------------------ */
#define SDL_KMOD_NONE KMOD_NONE
#define SDL_KMOD_SHIFT KMOD_SHIFT
#define SDL_KMOD_RSHIFT KMOD_RSHIFT
#define SDL_KMOD_CTRL KMOD_CTRL
#define SDL_KMOD_ALT KMOD_ALT
#define SDL_KMOD_LALT KMOD_LALT
#define SDL_KMOD_GUI KMOD_GUI
#define SDLK_M SDLK_m
#define SDLK_P SDLK_p
#define SDLK_S SDLK_s
#define SDLK_X SDLK_x
#define SDL_SCANCODE_COUNT SDL_NUM_SCANCODES
#define SDL_GetKeyboardState(count) ((const bool *)(SDL_GetKeyboardState)(count))
#define SDL_GetKeyFromScancode Shim_GetKeyFromScancode
#define SDL_GetScancodeFromKey Shim_GetScancodeFromKey

/* --- the rest ----------------------------------------------------------------- */
#define SDL_SetAtomicInt SDL_AtomicSet
#define SDL_GetAtomicInt SDL_AtomicGet
#define SDL_ShowSimpleMessageBox Shim_ShowSimpleMessageBox
#define SDL_ShowMessageBox Shim_ShowMessageBox
#define SDL_OpenURL Shim_OpenURL
#define SDL_SetClipboardText Shim_SetClipboardText
#endif
