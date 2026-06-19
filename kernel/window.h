// =============================================================================
//  kernel/window.h -- Fenêtres et gestionnaire de fenêtres
// =============================================================================
#ifndef SEXOS_WINDOW_H
#define SEXOS_WINDOW_H

#include "gfx.h"
#include "input.h"

#define TITLEBAR_H 28
#define WIN_MIN_W  200
#define WIN_MIN_H  120

typedef struct window window_t;

struct window {
    int   x, y;            // position écran (coin haut-gauche, barre de titre comprise)
    int   w, h;            // taille totale (barre de titre + contenu)
    char  title[64];
    canvas_t canvas;       // surface du contenu (taille w x (h - TITLEBAR_H))
    bool  minimized;
    bool  focused;
    bool  resizable;
    bool  wants_close;     // l'application demande la fermeture
    bool  dirty;           // contenu à redessiner

    void (*on_paint)(window_t *win);
    // Événement reçu dans le repère du contenu (cx, cy = coords contenu).
    void (*on_event)(window_t *win, const event_t *e, int cx, int cy);
    void  *user;           // état propre à l'application

    window_t *next;        // liste en ordre de pile (z-order)
};

void      wm_init(void);
window_t *wm_create(const char *title, int x, int y, int w, int h);
void      wm_close(window_t *win);
void      wm_focus(window_t *win);
window_t *wm_focused(void);
void      wm_request_paint(window_t *win);

// Dessine toutes les fenêtres (du bas vers le haut) sur la surface cible.
void      wm_draw_all(canvas_t *target);
// Gère un événement souris (déplacement, focus, boutons, redimensionnement).
// Renvoie true si l'événement a été consommé par la gestion des fenêtres.
bool      wm_handle_mouse(const event_t *e);
// Route un événement clavier vers la fenêtre active.
void      wm_handle_key(const event_t *e);

int       wm_count(void);
window_t *wm_get(int index);

#endif
