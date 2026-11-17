#ifndef MEMORIES_PC_GL_PICTURE_H
#define MEMORIES_PC_GL_PICTURE_H
#include <stdint.h>

/* The internal resolution drawn by OpenGL (SDL backend only). The software
 * GPU records what it did to VRAM (soft_gpu.h, SoftGpuRecorder); at present
 * the record is replayed into a picture of the whole of VRAM at scale x
 * scale pixels per word, kept in a framebuffer. VRAM itself stays the
 * console's. Needs a context of OpenGL 3.0 or OpenGL ES 3.0 or later,
 * current on the calling thread; every call below is made with that context
 * current. */

/* Once the window's context exists. Returns 1 when the pass is available,
 * and from then on the software GPU records for it instead of drawing its
 * own picture. 0 (GL too old, MEMORIES_GL_PICTURE=0) leaves the software
 * picture in place, and nothing made in the context. */
int GlPicture_Init(void);
/* Replays what was recorded since the last call; the framebuffer's texture
 * is then the picture. Returns 0 when the pass is off or the scale is 1. */
int GlPicture_Replay(void);
/* The picture's texture (RGBA, picture_w x picture_h texels; texel row 0
 * is the top of VRAM), after Replay. With anti-aliasing this resolves the
 * whole picture: to show part of it, GlPicture_ShownTexture resolves that
 * part alone. */
unsigned GlPicture_Texture(int *picture_w, int *picture_h);
/* Pixels x,y,w,h of that texture copied into one of their own (w x h, row
 * 0 their top), after Replay, for the presenter: in the whole picture the
 * rows and columns past them are the rest of VRAM (the other buffer,
 * textures), which bilinear would blend into the window's edges, and the
 * shown area's place in VRAM alternates between the buffers, which nearest
 * at a scale that is not whole would sample a little differently each
 * frame. 0 when the pass is off. */
unsigned GlPicture_ShownTexture(int x, int y, int w, int h);
int GlPicture_Scale(void);
/* The scale the pass could not make its picture at (the targets did not fit
 * in video memory, or the scale is beyond the largest texture, or on OpenGL
 * ES beyond 4x), once: 0 when there was none since the last call. *why is
 * then MEMORIES_REFUSED_ROOM, _SIZE or _ES (display.h). The pass has no
 * picture meanwhile; the platform asks the game for a lower scale
 * (Memories_ScaleRefused). */
int GlPicture_TakeRefusedScale(int *why);
/* The record is half the arena: frames went unshown (a raised game speed)
 * and it should be replayed before it overflows, which would draw the next
 * frame from VRAM at 1x. */
int GlPicture_Behind(void);
/* Pixels x,y,w,h of the picture as 0x00RRGGBB, after a Replay. Returns 0
 * when the pass is off. */
int GlPicture_Read(int x, int y, int w, int h, uint32_t *out);
/* Pixels x,y,w,h of what the presenter showed, as 0x00RRGGBB, row 0 the top
 * (the screenshot's read-back; nothing is read per frame): of the picture
 * (GlPicture_Texture's) with `wide_texture` 0, else of that widescreen
 * target's (GlPicture_WideTexture). Past the edges the edge pixels, as the
 * presenter's clamp shows them. 0 when the pass is off or has no such
 * texture (the picture was replaced since). */
int GlPicture_ReadShown(unsigned wide_texture, int x, int y, int w, int h, uint32_t *out);
/* Widescreen: the widened picture of the display area x,y,w,h (words), when
 * the pass drew a target for it (soft_gpu.h, SoftGpu_WideFrame), after a
 * Replay: its texture, picture_w x picture_h texels, row 0 the area's top,
 * the widened area filling it across. Sides nothing drew since the last
 * call are made black, as the software GPU makes them. 0 when there is none. */
unsigned GlPicture_WideTexture(int x, int y, int w, int h, int *picture_w, int *picture_h);
/* The same picture's first h rows as 0x00RRGGBB into `out` (picture_w x
 * h * scale), nothing changed. Returns 0 when there is none, or when it is
 * not wide_w words across at `want_scale` (the size `out` holds). */
int GlPicture_ReadWide(int x, int y, int w, int h, int wide_w, int want_scale, uint32_t *out);

/* OpenGL ES 3 (Android, and MEMORIES_GLES=1 on a desktop): the context is
 * the SDL renderer's (opengles2, in an ES 3.0 context), which shows the
 * picture through a texture of its own. Pixels x,y,w,h of `from` (the
 * picture's texture, GlPicture_Texture, or a widescreen target's,
 * GlPicture_WideTexture) into texture `to` (RGBA, w x h texels, row 0 their
 * top), after a Replay. Returns 0 when `from` is neither or `to` cannot be
 * drawn into. */
int GlPicture_CopyInto(unsigned from, int x, int y, int w, int h, unsigned to);
/* The context was lost (Android, the app sent to the background): every
 * name the pass held is forgotten, not deleted, and the pass is off until
 * GlPicture_Init in the new context, whose first replay draws the picture
 * again from VRAM. */
void GlPicture_Lost(void);
/* The pass given up for good (OpenGL ES: a failed start after a lost
 * context, or the presenter's texture cannot be made or drawn into): every
 * name it holds deleted, so the context must be current, the pass off, and
 * the software GPU draws the picture again, from VRAM, as without the pass.
 * sdl.c does not start it again at a later device reset: the recorder's
 * return (SoftGpu_SetRecorder) would free the software picture that the
 * present handling the reset is reading. */
void GlPicture_Stop(void);
#endif
