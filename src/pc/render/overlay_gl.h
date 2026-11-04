/* The mods' overlays drawn by the GPU: their pictures kept as textures,
 * their calls kept as quads, drawn over the game's picture every frame. On
 * the CPU the same calls are blended into a canvas the window's size and
 * sent to a texture, which takes longer the larger the window is and the
 * more of it a call changes; here a fade over the whole window costs one
 * quad. Desktop OpenGL's fixed pipeline, on Linux and Windows: elsewhere
 * OverlayGl_Sink is NULL and the canvas draws them. */
#ifndef PC_RENDER_OVERLAY_GL_H
#define PC_RENDER_OVERLAY_GL_H

struct ModsOverlaySink;
/* With a context current. NULL where it cannot be used. */
const struct ModsOverlaySink *OverlayGl_Sink(void);
/* The quads, over what the window holds, in the projection it is drawn in
 * (win_w x win_h, y down), `output` pixels wide and high. Blending is left on
 * with the premultiplied function and the color white. */
void OverlayGl_Draw(int win_w, int win_h, int output_w, int output_h);
/* The textures and quads forgotten, the context they were in gone. */
void OverlayGl_Lost(void);

#endif
