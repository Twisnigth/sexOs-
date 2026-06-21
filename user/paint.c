// =============================================================================
//  user/paint.c -- Application de dessin (processus ring 3, client compositeur)
// -----------------------------------------------------------------------------
//  Dessin a la souris, palette de couleurs, taille de pinceau, effacer.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

#define W 420
#define H 320
#define TOOL_H 28
#define NCOL 9

static canvas_t *cv;
static uint32_t palette[NCOL] = {
    0x000000, 0xffffff, 0xe04040, 0x40c040, 0x4070e0,
    0xf0d030, 0xf08020, 0xb060e0, 0x40d0d0,
};
static uint32_t cur = 0x000000;
static int brush = 4;
static int last_x, last_y, drawing;

static void fill_canvas_area(uint32_t col) {
    canvas_fill_rect(cv, 0, TOOL_H, W, H - TOOL_H, col);
}

static void draw_toolbar(void) {
    canvas_fill_rect(cv, 0, 0, W, TOOL_H, rgb(0x22, 0x26, 0x32));
    for (int i = 0; i < NCOL; i++) {
        int x = 4 + i * 24;
        canvas_fill_rect(cv, x, 4, 20, 20, palette[i]);
        uint32_t bord = (palette[i] == cur) ? rgb(0xff,0xff,0xff) : rgb(0x10,0x12,0x18);
        canvas_draw_rect(cv, x, 4, 20, 20, bord);
        if (palette[i] == cur) canvas_draw_rect(cv, x-1, 3, 22, 22, rgb(0xff,0xff,0xff));
    }
    // pinceau - / + et indicateur
    int bx = 4 + NCOL * 24 + 8;
    canvas_fill_rect(cv, bx, 4, 20, 20, rgb(0x3a,0x42,0x58));
    canvas_draw_string(cv, "-", bx + 7, 8, rgb(0xff,0xff,0xff), 2);
    canvas_fill_rect(cv, bx+24, 4, 20, 20, rgb(0x3a,0x42,0x58));
    canvas_draw_string(cv, "+", bx+24 + 5, 8, rgb(0xff,0xff,0xff), 2);
    char s[4]; s[0]='0'+ (brush/10)%10; s[1]='0'+brush%10; s[2]=0;
    canvas_draw_string(cv, (brush>=10?s:s+0), bx+50, 10, rgb(0xc8,0xd0,0xdc), 1);
    // bouton Effacer
    canvas_fill_rect(cv, W - 70, 4, 64, 20, rgb(0xc0,0x50,0x50));
    canvas_draw_string(cv, "Effacer", W - 66, 9, rgb(0xff,0xff,0xff), 1);
}

static void brush_at(int x, int y) {
    int s = brush;
    canvas_fill_rect(cv, x - s/2, y - s/2, s, s, cur);
}

// trace un segment epais entre deux points (interpolation lineaire)
static void stroke(int x0, int y0, int x1, int y1) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int n = adx > ady ? adx : ady; if (n < 1) n = 1;
    for (int i = 0; i <= n; i++) {
        int x = x0 + dx * i / n, y = y0 + dy * i / n;
        if (y >= TOOL_H) brush_at(x, y);
    }
}

static void on_mouse(const event_t *e) {
    int held = e->buttons & MOUSE_LEFT;
    if (held && e->my < TOOL_H && !drawing) {       // clic dans la barre d'outils
        int x = e->mx;
        for (int i = 0; i < NCOL; i++) if (x >= 4+i*24 && x < 4+i*24+20) { cur = palette[i]; draw_toolbar(); win_damage(); return; }
        int bx = 4 + NCOL*24 + 8;
        if (x >= bx && x < bx+20)        { if (brush > 1) brush--; draw_toolbar(); win_damage(); return; }
        if (x >= bx+24 && x < bx+44)     { if (brush < 40) brush++; draw_toolbar(); win_damage(); return; }
        if (x >= W-70 && x < W-6)        { fill_canvas_area(0xffffff); win_damage(); return; }
        return;
    }
    if (held) {
        if (drawing) stroke(last_x, last_y, e->mx, e->my);
        else if (e->my >= TOOL_H) brush_at(e->mx, e->my);
        last_x = e->mx; last_y = e->my; drawing = 1;
        win_damage();
    } else {
        drawing = 0;
    }
}

int main(void) {
    cv = win_create(W, H, "Dessin");
    if (!cv) return 1;
    fill_canvas_area(0xffffff);
    draw_toolbar();
    win_damage();
    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (e.type == EV_MOUSE) on_mouse(&e);
    }
}
