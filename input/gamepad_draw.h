/*
 * gamepad_draw.h -- the few things the controller's own screens need drawn: filled
 * rectangles, ring sectors, discs and a 5x7 pixel font. Fixed-function OpenGL in
 * immediate mode, drawn over a finished frame just before it is presented.
 *
 * Coordinates are WINDOW POINTS, the space SDL's mouse events use, top-left origin; the
 * projection maps them onto the drawable whatever its pixel density.
 *
 * gpd_begin saves every piece of GL state it is about to touch and gpd_end puts it back,
 * so the program's next frame starts from exactly the state it left. That is the whole
 * contract with a renderer that does not know this layer exists.
 */

#ifndef OPENCNC3D_GAMEPAD_DRAW_H
#define OPENCNC3D_GAMEPAD_DRAW_H

#include <SDL.h>

typedef struct {
    float r, g, b, a;
} GpColor;

/* 0 when there is nothing to draw on (no current GL context for that window). */
int gpd_begin(SDL_Window *w, float *width, float *height);
void gpd_end(void);

void gpd_rect(float x, float y, float w, float h, GpColor c);
void gpd_frame(float x, float y, float w, float h, float t, GpColor c);
/* A ring sector between radii r0 and r1, from angle a0 to a1, in radians clockwise from
   straight up. */
void gpd_sector(float cx, float cy, float r0, float r1, float a0, float a1, GpColor c);
void gpd_disc(float cx, float cy, float r, GpColor c);
/* An ellipse, and a rectangle with round corners of radius r. */
void gpd_ellipse(float cx, float cy, float rx, float ry, GpColor c);
void gpd_round_rect(float x, float y, float w, float h, float r, GpColor c);
/* A triangle. */
void gpd_tri(float x0, float y0, float x1, float y1, float x2, float y2, GpColor c);
/* A straight line `t` wide. */
void gpd_line(float x0, float y0, float x1, float y1, float t, GpColor c);
/* A textured quad from a GL texture the program owns, its (0,0)..(u,v) corner. */
void gpd_image(float x, float y, float w, float h, unsigned int tex, float u, float v, GpColor tint);

/* Upper case, digits and a little punctuation; lower case is drawn as upper case. `px`
   is the size of one font pixel, so a glyph is 5px wide and 7px tall on a 6px advance. */
void gpd_text(float x, float y, float px, const char *s, GpColor c);
float gpd_text_width(float px, const char *s);
void gpd_text_centered(float cx, float y, float px, const char *s, GpColor c);

#endif
