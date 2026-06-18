// =============================================================================
//  kernel/wm.c -- Gestionnaire de fenêtres (pile z, focus, déplacement, etc.)
// =============================================================================
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"

#define BTN_SIZE   18
#define BTN_MARGIN 5
#define GRIP       16

static window_t *windows;      // liste du bas (tête) vers le haut (queue)
static int prev_buttons;

// État d'interaction.
static window_t *drag_win;  static int drag_dx, drag_dy;
static window_t *resize_win;

// --- Couleurs ----------------------------------------------------------------
static uint32_t col_title_active(void)   { return fb_rgb(0x2d, 0x6c, 0xdf); }
static uint32_t col_title_inactive(void) { return fb_rgb(0x55, 0x5b, 0x6e); }
static uint32_t col_border(void)         { return fb_rgb(0x10, 0x12, 0x1a); }
static uint32_t col_title_text(void)     { return fb_rgb(0xff, 0xff, 0xff); }

void wm_init(void) {
    windows = NULL;
    prev_buttons = 0;
    drag_win = resize_win = NULL;
}

static int content_h(window_t *w) { return w->h - TITLEBAR_H; }

static bool alloc_canvas(window_t *w) {
    int ch = content_h(w);
    if (ch < 1) ch = 1;
    size_t bytes = (size_t)w->w * ch * 4;
    uint32_t *px = (uint32_t *)krealloc(w->canvas.pixels, bytes);
    if (!px) return false;
    w->canvas.pixels = px;
    w->canvas.width  = w->w;
    w->canvas.height = ch;
    w->canvas.pitch  = w->w * 4;
    return true;
}

window_t *wm_create(const char *title, int x, int y, int w, int h) {
    window_t *win = (window_t *)kcalloc(1, sizeof(window_t));
    if (!win) return NULL;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->resizable = true;
    win->dirty = true;
    strncpy(win->title, title, sizeof(win->title) - 1);
    if (!alloc_canvas(win)) { kfree(win); return NULL; }
    canvas_fill(&win->canvas, fb_rgb(0x20, 0x22, 0x2c));

    // Ajoute en queue (au sommet) et donne le focus.
    win->next = NULL;
    if (!windows) windows = win;
    else { window_t *c = windows; while (c->next) c = c->next; c->next = win; }
    wm_focus(win);
    return win;
}

void wm_close(window_t *win) {
    if (!win) return;
    if (windows == win) windows = win->next;
    else for (window_t *c = windows; c; c = c->next)
            if (c->next == win) { c->next = win->next; break; }
    if (win->canvas.pixels) kfree(win->canvas.pixels);
    if (drag_win == win) drag_win = NULL;
    if (resize_win == win) resize_win = NULL;
    kfree(win);
}

void wm_focus(window_t *win) {
    if (!win) return;
    // Déplace 'win' en queue de liste (sommet de la pile).
    if (windows == win) {
        if (!win->next) { goto setfocus; }
        windows = win->next;
    } else {
        for (window_t *c = windows; c; c = c->next)
            if (c->next == win) { c->next = win->next; break; }
    }
    win->next = NULL;
    if (!windows) windows = win;
    else { window_t *c = windows; while (c->next) c = c->next; c->next = win; }
setfocus:
    for (window_t *c = windows; c; c = c->next) c->focused = (c == win);
    win->minimized = false;
}

window_t *wm_focused(void) {
    window_t *f = NULL;
    for (window_t *c = windows; c; c = c->next) if (!c->minimized) f = c;
    return f;   // la dernière non minimisée = sommet
}

void wm_request_paint(window_t *win) { if (win) win->dirty = true; }

// --- Dessin ------------------------------------------------------------------
static void draw_window(canvas_t *t, window_t *win) {
    if (win->minimized) return;

    // Contenu (peint par l'application si nécessaire).
    if (win->dirty && win->on_paint) { win->on_paint(win); win->dirty = false; }

    // Barre de titre.
    uint32_t tc = win->focused ? col_title_active() : col_title_inactive();
    canvas_fill_rect(t, win->x, win->y, win->w, TITLEBAR_H, tc);
    canvas_draw_string(t, win->title, win->x + 8, win->y + 6, col_title_text(), 1);

    // Boutons fermer (rouge) et réduire (jaune).
    int bx = win->x + win->w - BTN_SIZE - BTN_MARGIN;
    int by = win->y + BTN_MARGIN;
    canvas_fill_rect(t, bx, by, BTN_SIZE, BTN_SIZE, fb_rgb(0xe0, 0x4f, 0x4f));
    canvas_draw_string(t, "x", bx + 5, by + 1, fb_rgb(0xff,0xff,0xff), 1);
    int mx = bx - BTN_SIZE - 4;
    canvas_fill_rect(t, mx, by, BTN_SIZE, BTN_SIZE, fb_rgb(0xe0, 0xc4, 0x4f));
    canvas_fill_rect(t, mx + 4, by + BTN_SIZE - 6, BTN_SIZE - 8, 3, fb_rgb(0x30,0x30,0x30));

    // Contenu.
    canvas_blit(t, &win->canvas, win->x, win->y + TITLEBAR_H);

    // Bordure + poignée de redimensionnement.
    canvas_draw_rect(t, win->x, win->y, win->w, win->h, col_border());
    if (win->resizable) {
        int gx = win->x + win->w - GRIP, gy = win->y + win->h - GRIP;
        for (int i = 4; i < GRIP; i += 4)
            canvas_draw_line(t, gx + i, win->y + win->h - 2, win->x + win->w - 2, gy + i,
                             fb_rgb(0xaa,0xaa,0xaa));
    }
}

void wm_draw_all(canvas_t *target) {
    for (window_t *c = windows; c; c = c->next) draw_window(target, c);
}

// --- Test de présence sous le curseur ----------------------------------------
static window_t *window_at(int x, int y) {
    window_t *found = NULL;
    for (window_t *c = windows; c; c = c->next) {
        if (c->minimized) continue;
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) found = c;
    }
    return found;   // le dernier correspondant = sommet
}

static void forward_content_event(window_t *win, const event_t *e) {
    if (!win || !win->on_event) return;
    int cx = e->mx - win->x;
    int cy = e->my - (win->y + TITLEBAR_H);
    win->on_event(win, e, cx, cy);
}

bool wm_handle_mouse(const event_t *e) {
    int left = e->buttons & MOUSE_LEFT;
    int left_pressed  = left && !(prev_buttons & MOUSE_LEFT);
    int left_released = !left && (prev_buttons & MOUSE_LEFT);
    prev_buttons = e->buttons;

    // Déplacement / redimensionnement en cours.
    if (drag_win) {
        if (left) {
            drag_win->x = e->mx - drag_dx;
            drag_win->y = e->my - drag_dy;
            if (drag_win->y < 0) drag_win->y = 0;
            if (drag_win->x < -(drag_win->w - 60)) drag_win->x = -(drag_win->w - 60);
            if (drag_win->x > (int)fb_width() - 60) drag_win->x = fb_width() - 60;
            if (drag_win->y > (int)fb_height() - TITLEBAR_H) drag_win->y = fb_height() - TITLEBAR_H;
            return true;
        }
        drag_win = NULL;
    }
    if (resize_win) {
        if (left) {
            int nw = e->mx - resize_win->x;
            int nh = e->my - resize_win->y;
            if (nw < WIN_MIN_W) nw = WIN_MIN_W;
            if (nh < WIN_MIN_H) nh = WIN_MIN_H;
            if (nw != resize_win->w || nh != resize_win->h) {
                resize_win->w = nw; resize_win->h = nh;
                alloc_canvas(resize_win);
                canvas_fill(&resize_win->canvas, fb_rgb(0x20,0x22,0x2c));
                resize_win->dirty = true;
            }
            return true;
        }
        resize_win = NULL;
    }

    if (left_pressed) {
        window_t *win = window_at(e->mx, e->my);
        if (!win) return false;            // clic sur le bureau : non consommé
        wm_focus(win);

        // Boutons de la barre de titre ?
        if (e->my >= win->y && e->my < win->y + TITLEBAR_H) {
            int bx = win->x + win->w - BTN_SIZE - BTN_MARGIN;
            int by = win->y + BTN_MARGIN;
            if (e->mx >= bx && e->mx < bx + BTN_SIZE && e->my >= by && e->my < by + BTN_SIZE) {
                win->wants_close = true; return true;        // fermer
            }
            int mxb = bx - BTN_SIZE - 4;
            if (e->mx >= mxb && e->mx < mxb + BTN_SIZE && e->my >= by && e->my < by + BTN_SIZE) {
                win->minimized = true; return true;          // réduire
            }
            // Sinon : début du déplacement.
            drag_win = win; drag_dx = e->mx - win->x; drag_dy = e->my - win->y;
            return true;
        }
        // Poignée de redimensionnement ?
        if (win->resizable &&
            e->mx >= win->x + win->w - GRIP && e->my >= win->y + win->h - GRIP) {
            resize_win = win; return true;
        }
        // Sinon : événement de contenu.
        forward_content_event(win, e);
        return true;
    }

    if (left_released) {
        window_t *win = wm_focused();
        if (win) forward_content_event(win, e);
        return false;
    }

    // Simple déplacement : transmis à la fenêtre survolée.
    window_t *win = window_at(e->mx, e->my);
    if (win && e->my >= win->y + TITLEBAR_H) { forward_content_event(win, e); }
    return win != NULL;
}

void wm_handle_key(const event_t *e) {
    window_t *win = wm_focused();
    if (win && win->on_event) win->on_event(win, e, 0, 0);
}

int wm_count(void) {
    int n = 0;
    for (window_t *c = windows; c; c = c->next) n++;
    return n;
}
window_t *wm_get(int index) {
    int n = 0;
    for (window_t *c = windows; c; c = c->next) if (n++ == index) return c;
    return NULL;
}
