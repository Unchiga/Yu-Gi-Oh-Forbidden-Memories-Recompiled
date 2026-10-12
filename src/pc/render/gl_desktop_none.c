/* Where the system's OpenGL is GLES (Android, the Switch), in place of present_pass.c:
 * the present pass and sdl.c's GL presenter call desktop GL's fixed
 * function (glBegin, glOrtho, the matrix stack), which GLES does not have.
 * That path is never taken there, since Platform_HasDesktopGL() is 0 and
 * sdl.c shows the picture through the SDL renderer instead; these only
 * complete the link. tools/pc/build_game32.py picks this file for Android
 * builds (ANDROID_BACKEND), tools/pc/build_config.py for the Switch. */
#if defined(__ANDROID__) || defined(__SWITCH__)
#include "present_pass.h"
#include <SDL3/SDL_opengl.h>

int PresentPass_InitCore(void) { return 0; }

void PresentPass_Quad(float x, float y, float w, float h, float s0, float t0, float s1, float t1)
{
    (void)x; (void)y; (void)w; (void)h; (void)s0; (void)t0; (void)s1; (void)t1;
}

int PresentPass_Wanted(void)
{
    return 0;
}

int PresentPass_Begin(unsigned texture, int source_h, float s0, float t0, float s1, float t1, int textures_smoothed)
{
    (void)texture; (void)source_h; (void)s0; (void)t0; (void)s1; (void)t1; (void)textures_smoothed;
    return 0;
}

void PresentPass_End(void)
{
}

void APIENTRY glBegin(GLenum mode) { (void)mode; }
void APIENTRY glEnd(void) {}
void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
void APIENTRY glTexCoord2f(GLfloat s, GLfloat t) { (void)s; (void)t; }
void APIENTRY glVertex2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
void APIENTRY glMatrixMode(GLenum mode) { (void)mode; }
void APIENTRY glLoadIdentity(void) {}
void APIENTRY glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    (void)l; (void)r; (void)b; (void)t; (void)n; (void)f;
}
#endif
