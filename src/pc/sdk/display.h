#ifndef MEMORIES_PC_SDK_DISPLAY_H
#define MEMORIES_PC_SDK_DISPLAY_H
/* Show the area selected by the last PutDispEnv. Main thread only. */
void Memories_PresentDisplay(void);
/* The display area shown as it stands, menu and notices over it, no frame
 * counted (the control channel's wait). Main thread only. */
void Memories_ShowStill(void);
unsigned Memories_PresentedFrames(void);
/* Frames that reached the window: presentation is paced apart from the game. */
unsigned Memories_ShownFrames(void);
typedef struct FrameStats {
    unsigned fps_tenths;       /* game frames per second */
    unsigned shown_tenths;     /* presented frames per second */
    unsigned game_us, present_us, game_max_us, present_max_us;
    unsigned missed_vblanks;
    unsigned draw_words, draw_us;
    unsigned windows;          /* how many times these were counted (every 120 frames) */
} FrameStats;
const FrameStats *Memories_FrameStats(void);
void Memories_SetDrawStats(unsigned words, unsigned us);
/* Write the current display rectangle, or the entire 1024x512 VRAM, as PPM. */
void Memories_DumpFrame(const char *path, int full_vram);
/* FNV-1a of all of VRAM, the hash MEMORIES_FRAME_HASHES writes per frame. */
unsigned long long Memories_VramHash(void);
/* The internal resolution (SoftGpu_SetScale) from the main thread while
 * the game runs; 3 and 5 to 7 round down to 2 and 4. */
int Memories_SetInternalScale(int scale);
/* For game code that reads part of the picture back (StoreImage) and loads
 * it elsewhere to draw with (LoadImage): after the load, so the OpenGL
 * picture draws that copy from its scaled picture (SoftGpu_Capture)
 * instead of from the console-resolution VRAM. */
void Memories_PictureCapture(int sx, int sy, int dx, int dy, int w, int h);
#endif
