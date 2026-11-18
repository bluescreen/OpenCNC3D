/*
 * gamepad_draw.c -- see gamepad_draw.h.
 */

#include "gamepad_draw.h"

#include <math.h>
#include <string.h>

#define GL_SILENCE_DEPRECATION 1
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

/* ---- the font ---------------------------------------------------------------------- */

/* 5x7, one byte per row, bit 4 the left column. */
static const char gpd_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,:-+/<>()%!?'$";
static const unsigned char gpd_glyphs[][7] = {
    {14, 17, 17, 31, 17, 17, 17}, /* A */
    {30, 17, 17, 30, 17, 17, 30}, /* B */
    {14, 17, 16, 16, 16, 17, 14}, /* C */
    {30, 17, 17, 17, 17, 17, 30}, /* D */
    {31, 16, 16, 30, 16, 16, 31}, /* E */
    {31, 16, 16, 30, 16, 16, 16}, /* F */
    {14, 17, 16, 23, 17, 17, 15}, /* G */
    {17, 17, 17, 31, 17, 17, 17}, /* H */
    {14, 4, 4, 4, 4, 4, 14},      /* I */
    {7, 2, 2, 2, 2, 18, 12},      /* J */
    {17, 18, 20, 24, 20, 18, 17}, /* K */
    {16, 16, 16, 16, 16, 16, 31}, /* L */
    {17, 27, 21, 21, 17, 17, 17}, /* M */
    {17, 17, 25, 21, 19, 17, 17}, /* N */
    {14, 17, 17, 17, 17, 17, 14}, /* O */
    {30, 17, 17, 30, 16, 16, 16}, /* P */
    {14, 17, 17, 17, 21, 18, 13}, /* Q */
    {30, 17, 17, 30, 20, 18, 17}, /* R */
    {15, 16, 16, 14, 1, 1, 30},   /* S */
    {31, 4, 4, 4, 4, 4, 4},       /* T */
    {17, 17, 17, 17, 17, 17, 14}, /* U */
    {17, 17, 17, 17, 17, 10, 4},  /* V */
    {17, 17, 17, 21, 21, 21, 10}, /* W */
    {17, 17, 10, 4, 10, 17, 17},  /* X */
    {17, 17, 17, 10, 4, 4, 4},    /* Y */
    {31, 1, 2, 4, 8, 16, 31},     /* Z */
    {14, 17, 19, 21, 25, 17, 14}, /* 0 */
    {4, 12, 4, 4, 4, 4, 14},      /* 1 */
    {14, 17, 1, 2, 4, 8, 31},     /* 2 */
    {31, 2, 4, 2, 1, 17, 14},     /* 3 */
    {2, 6, 10, 18, 31, 2, 2},     /* 4 */
    {31, 16, 30, 1, 1, 17, 14},   /* 5 */
    {6, 8, 16, 30, 17, 17, 14},   /* 6 */
    {31, 1, 2, 4, 8, 8, 8},       /* 7 */
    {14, 17, 17, 14, 17, 17, 14}, /* 8 */
    {14, 17, 17, 15, 1, 2, 12},   /* 9 */
    {0, 0, 0, 0, 0, 0, 0},        /*   */
    {0, 0, 0, 0, 0, 12, 12},      /* . */
    {0, 0, 0, 0, 12, 4, 8},       /* , */
    {0, 12, 12, 0, 12, 12, 0},    /* : */
    {0, 0, 0, 31, 0, 0, 0},       /* - */
    {0, 4, 4, 31, 4, 4, 0},       /* + */
    {0, 1, 2, 4, 8, 16, 0},       /* / */
    {2, 4, 8, 16, 8, 4, 2},       /* < */
    {8, 4, 2, 1, 2, 4, 8},        /* > */
    {2, 4, 8, 8, 8, 4, 2},        /* ( */
    {8, 4, 2, 2, 2, 4, 8},        /* ) */
    {24, 25, 2, 4, 8, 19, 3},     /* % */
    {4, 4, 4, 4, 4, 0, 4},        /* ! */
    {14, 17, 1, 2, 4, 0, 4},      /* ? */
    {4, 4, 8, 0, 0, 0, 0},        /* ' */
    {4, 15, 20, 14, 5, 30, 4},    /* $ */
};

/* ---- state save and restore ---------------------------------------------------------- */

static GLint gpd_prog, gpd_fbo, gpd_active;

int gpd_begin(SDL_Window *w, float *width, float *height)
{
    int ww, wh, dw, dh, i;
    if (!w || SDL_GL_GetCurrentWindow() != w || !SDL_GL_GetCurrentContext())
        return 0;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_GL_GetDrawableSize(w, &dw, &dh);
    if (ww <= 0 || wh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    glGetIntegerv(GL_CURRENT_PROGRAM, &gpd_prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &gpd_fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &gpd_active);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

    glUseProgram(0);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    /* Fixed function combines every enabled texture unit, so all of them go off, not
       just the active one. */
    for (i = 3; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_3D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_TEXTURE_RECTANGLE_ARB);
    }
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)ww, (double)wh, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glViewport(0, 0, dw, dh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_COLOR_MATERIAL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawBuffer(GL_BACK);

    *width = (float)ww;
    *height = (float)wh;
    return 1;
}

void gpd_end(void)
{
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib(); /* matrix mode, viewport, enables, blend, colour */
    glActiveTexture((GLenum)gpd_active);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)gpd_fbo);
    glUseProgram((GLuint)gpd_prog);
}

/* ---- shapes ---------------------------------------------------------------------------- */

static void gpd_color(GpColor c)
{
    glColor4f(c.r, c.g, c.b, c.a);
}

void gpd_rect(float x, float y, float w, float h, GpColor c)
{
    gpd_color(c);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void gpd_frame(float x, float y, float w, float h, float t, GpColor c)
{
    gpd_rect(x, y, w, t, c);
    gpd_rect(x, y + h - t, w, t, c);
    gpd_rect(x, y + t, t, h - 2 * t, c);
    gpd_rect(x + w - t, y + t, t, h - 2 * t, c);
}

void gpd_sector(float cx, float cy, float r0, float r1, float a0, float a1, GpColor c)
{
    int i, n = (int)((a1 - a0) * 24.0f) + 2;
    gpd_color(c);
    glBegin(GL_TRIANGLE_STRIP);
    for (i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        float sx = sinf(a), sy = -cosf(a);
        glVertex2f(cx + sx * r1, cy + sy * r1);
        glVertex2f(cx + sx * r0, cy + sy * r0);
    }
    glEnd();
}

void gpd_disc(float cx, float cy, float r, GpColor c)
{
    int i, n = 48;
    gpd_color(c);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (i = 0; i <= n; i++) {
        float a = 6.2831853f * (float)i / (float)n;
        glVertex2f(cx + sinf(a) * r, cy - cosf(a) * r);
    }
    glEnd();
}

void gpd_ellipse(float cx, float cy, float rx, float ry, GpColor c)
{
    int i, n = 56;
    gpd_color(c);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (i = 0; i <= n; i++) {
        float a = 6.2831853f * (float)i / (float)n;
        glVertex2f(cx + sinf(a) * rx, cy - cosf(a) * ry);
    }
    glEnd();
}

void gpd_round_rect(float x, float y, float w, float h, float r, GpColor c)
{
    int i, k, n = 10;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    gpd_color(c);
    /* one convex fan around the outline: four quarter circles joined by straight edges */
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(x + w * 0.5f, y + h * 0.5f);
    for (k = 0; k < 4; k++) {
        float cx = (k == 0 || k == 3) ? x + w - r : x + r, cy = (k < 2) ? y + h - r : y + r;
        for (i = 0; i <= n; i++) {
            float a = 1.5707963f * ((float)k + (float)i / (float)n);
            glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
        }
    }
    glVertex2f(x + w, y + h - r);
    glEnd();
}

void gpd_tri(float x0, float y0, float x1, float y1, float x2, float y2, GpColor c)
{
    gpd_color(c);
    glBegin(GL_TRIANGLES);
    glVertex2f(x0, y0);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
}

void gpd_line(float x0, float y0, float x1, float y1, float t, GpColor c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy), nx, ny;
    if (len <= 0.0f)
        return;
    nx = -dy / len * t * 0.5f;
    ny = dx / len * t * 0.5f;
    gpd_color(c);
    glBegin(GL_QUADS);
    glVertex2f(x0 + nx, y0 + ny);
    glVertex2f(x1 + nx, y1 + ny);
    glVertex2f(x1 - nx, y1 - ny);
    glVertex2f(x0 - nx, y0 - ny);
    glEnd();
}

void gpd_image(float x, float y, float w, float h, unsigned int tex, float u, float v, GpColor tint)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gpd_color(tint);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(u, 0.0f);    glVertex2f(x + w, y);
    glTexCoord2f(u, v);       glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, v);    glVertex2f(x, y + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* ---- text ------------------------------------------------------------------------------ */

static const unsigned char *gpd_glyph(char ch)
{
    const char *p;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    p = strchr(gpd_chars, ch);
    return (p && ch) ? gpd_glyphs[p - gpd_chars] : gpd_glyphs[36] /* space */;
}

float gpd_text_width(float px, const char *s)
{
    size_t n = strlen(s);
    return n ? (float)(6 * n - 1) * px : 0.0f;
}

void gpd_text(float x, float y, float px, const char *s, GpColor c)
{
    gpd_color(c);
    glBegin(GL_QUADS);
    for (; *s; s++, x += 6.0f * px) {
        const unsigned char *g = gpd_glyph(*s);
        int row, col;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (g[row] & (16 >> col)) {
                    float gx = x + (float)col * px, gy = y + (float)row * px;
                    glVertex2f(gx, gy);
                    glVertex2f(gx + px, gy);
                    glVertex2f(gx + px, gy + px);
                    glVertex2f(gx, gy + px);
                }
    }
    glEnd();
}

void gpd_text_centered(float cx, float y, float px, const char *s, GpColor c)
{
    gpd_text(cx - gpd_text_width(px, s) * 0.5f, y, px, s, c);
}
