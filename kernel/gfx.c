// =============================================================================
//  kernel/gfx.c -- Primitives de dessin sur canvas
// =============================================================================
#include "gfx.h"
#include "font8x16.h"

static inline uint32_t *pixel_at(canvas_t *c, int32_t x, int32_t y) {
    return (uint32_t *)((uint8_t *)c->pixels + (uint32_t)y * c->pitch + (uint32_t)x * 4);
}

void canvas_put_pixel(canvas_t *c, int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || x >= (int32_t)c->width || y >= (int32_t)c->height) return;
    *pixel_at(c, x, y) = color;
}

void canvas_fill(canvas_t *c, uint32_t color) {
    for (uint32_t y = 0; y < c->height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)c->pixels + y * c->pitch);
        for (uint32_t x = 0; x < c->width; x++) row[x] = color;
    }
}

void canvas_fill_rect(canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)c->width)  w = (int32_t)c->width - x;
    if (y + h > (int32_t)c->height) h = (int32_t)c->height - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t yy = 0; yy < h; yy++) {
        uint32_t *row = pixel_at(c, x, y + yy);
        for (int32_t xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void canvas_draw_hline(canvas_t *c, int32_t x, int32_t y, int32_t w, uint32_t color) {
    canvas_fill_rect(c, x, y, w, 1, color);
}
void canvas_draw_vline(canvas_t *c, int32_t x, int32_t y, int32_t h, uint32_t color) {
    canvas_fill_rect(c, x, y, 1, h, color);
}

void canvas_draw_rect(canvas_t *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    canvas_draw_hline(c, x, y, w, color);
    canvas_draw_hline(c, x, y + h - 1, w, color);
    canvas_draw_vline(c, x, y, h, color);
    canvas_draw_vline(c, x + w - 1, y, h, color);
}

void canvas_draw_line(canvas_t *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    // Algorithme de Bresenham.
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int32_t err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        canvas_put_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void canvas_draw_char(canvas_t *c, char ch, int32_t x, int32_t y, uint32_t fg, int scale) {
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col))
                canvas_fill_rect(c, x + col * scale, y + row * scale, scale, scale, fg);
        }
    }
}

void canvas_draw_char_bg(canvas_t *c, char ch, int32_t x, int32_t y,
                         uint32_t fg, uint32_t bg, int scale) {
    const uint8_t *glyph = font8x16[(uint8_t)ch];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            canvas_fill_rect(c, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void canvas_draw_string(canvas_t *c, const char *s, int32_t x, int32_t y, uint32_t fg, int scale) {
    int32_t cx = x;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n') { cx = x; y += FONT_HEIGHT * scale; continue; }
        canvas_draw_char(c, s[i], cx, y, fg, scale);
        cx += FONT_WIDTH * scale;
    }
}

int canvas_text_width(const char *s, int scale) {
    int w = 0, max = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\n') { if (w > max) max = w; w = 0; }
        else w += FONT_WIDTH * scale;
    }
    return w > max ? w : max;
}

void canvas_blit_clipped(canvas_t *dst, const canvas_t *src, int32_t dx, int32_t dy,
                         const rect_t *clip) {
    int32_t cx0 = 0, cy0 = 0, cx1 = (int32_t)dst->width, cy1 = (int32_t)dst->height;
    if (clip) {
        if (clip->x > cx0) cx0 = clip->x;
        if (clip->y > cy0) cy0 = clip->y;
        if (clip->x + clip->w < cx1) cx1 = clip->x + clip->w;
        if (clip->y + clip->h < cy1) cy1 = clip->y + clip->h;
    }
    for (int32_t sy = 0; sy < (int32_t)src->height; sy++) {
        int32_t ty = dy + sy;
        if (ty < cy0 || ty >= cy1) continue;
        const uint32_t *srow = (const uint32_t *)((const uint8_t *)src->pixels + sy * src->pitch);
        uint32_t *drow = (uint32_t *)((uint8_t *)dst->pixels + ty * dst->pitch);
        for (int32_t sx = 0; sx < (int32_t)src->width; sx++) {
            int32_t tx = dx + sx;
            if (tx < cx0 || tx >= cx1) continue;
            drow[tx] = srow[sx];
        }
    }
}

void canvas_blit(canvas_t *dst, const canvas_t *src, int32_t dx, int32_t dy) {
    canvas_blit_clipped(dst, src, dx, dy, NULL);
}
