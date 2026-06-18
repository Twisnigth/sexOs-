// =============================================================================
//  kernel/gfx.h -- Primitives de dessin sur un "canvas" (surface 32 bpp)
// -----------------------------------------------------------------------------
//  Un canvas est une surface mémoire de pixels 32 bits. Le framebuffer matériel
//  et le back-buffer du compositeur sont tous deux des canvas, ce qui permet de
//  dessiner partout avec le même code (clé du double buffering anti-scintillement).
// =============================================================================
#ifndef MONOS_GFX_H
#define MONOS_GFX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t *pixels;   // base de la surface
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    // octets par ligne (>= width*4)
} canvas_t;

// Rectangle de découpe (clip) — utilisé pour limiter le rendu à une zone.
typedef struct { int32_t x, y, w, h; } rect_t;

void canvas_put_pixel(canvas_t *c, int32_t x, int32_t y, uint32_t color);
void canvas_fill(canvas_t *c, uint32_t color);
void canvas_fill_rect(canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void canvas_draw_rect(canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void canvas_draw_hline(canvas_t *c, int32_t x, int32_t y, int32_t w, uint32_t color);
void canvas_draw_vline(canvas_t *c, int32_t x, int32_t y, int32_t h, uint32_t color);
void canvas_draw_line(canvas_t *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);

// Texte (police bitmap 8x16), avec facteur d'échelle entier.
void canvas_draw_char(canvas_t *c, char ch, int32_t x, int32_t y, uint32_t fg, int scale);
void canvas_draw_char_bg(canvas_t *c, char ch, int32_t x, int32_t y, uint32_t fg, uint32_t bg, int scale);
void canvas_draw_string(canvas_t *c, const char *s, int32_t x, int32_t y, uint32_t fg, int scale);
int  canvas_text_width(const char *s, int scale);   // largeur en pixels

// Recopie (blit) d'un canvas source dans un canvas destination à (dx,dy).
void canvas_blit(canvas_t *dst, const canvas_t *src, int32_t dx, int32_t dy);
// Blit d'une sous-zone du source, avec découpe sur le rectangle clip (peut être NULL).
void canvas_blit_clipped(canvas_t *dst, const canvas_t *src, int32_t dx, int32_t dy,
                         const rect_t *clip);

#endif
