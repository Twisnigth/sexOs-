// =============================================================================
//  user/wmserver.c -- Compositeur / gestionnaire de fenêtres RING 3 (Phase 3)
// -----------------------------------------------------------------------------
//  Démontre l'architecture cible : un processus EN RING 3 (CPL 3) possède le
//  framebuffer (via sys_fb_map), reçoit le flux d'entrées (sys_input_poll) et
//  compose des fenêtres déplaçables / fermables. Tout le dessin réutilise le
//  vrai code gfx.c du noyau, recompilé pour l'espace utilisateur — aucun accès
//  matériel direct. (Le portage complet des 5 applications, couplées au VFS et
//  aux comptes, reste à faire : il nécessite la couche de syscalls VFS/users.)
// =============================================================================
#include "sexos.h"
#include "gfx.h"          // primitives de dessin (via -Ikernel), recompilées ring 3

#define TB   24           // hauteur de la barre de titre
#define NWIN 3

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;   // framebuffer 0xRRGGBB (QEMU)
}

typedef struct { int x, y, w, h; const char *title; int visible; } win_t;

static void itoa_u(uint64_t v, char *out) {
    char tmp[24]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

static void draw_window(canvas_t *c, win_t *w, int focused) {
    if (!w->visible) return;
    uint32_t tb = focused ? rgb(0x2d, 0x6c, 0xdf) : rgb(0x3a, 0x42, 0x58);
    canvas_fill_rect(c, w->x, w->y, w->w, w->h, rgb(0x20, 0x22, 0x2c));   // corps
    canvas_fill_rect(c, w->x, w->y, w->w, TB, tb);                        // barre
    canvas_draw_string(c, w->title, w->x + 8, w->y + 5, rgb(255, 255, 255), 1);
    // bouton fermer (carré rouge en haut à droite)
    canvas_fill_rect(c, w->x + w->w - 18, w->y + 5, 14, 14, rgb(0xe0, 0x4f, 0x4f));
    canvas_draw_string(c, "x", w->x + w->w - 15, w->y + 5, rgb(255, 255, 255), 1);
    canvas_draw_rect(c, w->x, w->y, w->w, w->h, rgb(0x55, 0x5a, 0x6a));   // bordure
}

int main(void) {
    fbinfo_t fb;
    if (sys_fb_map(&fb)) return 1;
    canvas_t screen = { (uint32_t *)(uintptr_t)fb.addr, fb.width, fb.height, fb.pitch };
    canvas_t back;
    back.width = fb.width; back.height = fb.height; back.pitch = fb.width * 4;
    back.pixels = (uint32_t *)sys_alloc((unsigned long)back.pitch * back.height);
    if (!back.pixels) return 2;

    win_t win[NWIN] = {
        { 120,  90, 360, 200, "Bienvenue (ring 3)", 1 },
        { 300, 180, 300, 150, "Horloge",            1 },
        { 200, 360, 340, 160, "Compositeur ring 3", 1 },
    };
    int cx = fb.width / 2, cy = fb.height / 2, buttons = 0, prevb = 0;
    int drag = -1, ddx = 0, ddy = 0, top = 0;

    for (int frame = 0; frame < 700; frame++) {
        event_t e;
        while (sys_input_poll(&e)) {
            if (e.type == EV_MOUSE) { cx = e.mx; cy = e.my; buttons = e.buttons; }
            else if (e.type == EV_KEY && e.pressed && e.key == KEY_ESC) return 0;
        }
        int pressed = (buttons & MOUSE_LEFT) && !(prevb & MOUSE_LEFT);
        int released = !(buttons & MOUSE_LEFT) && (prevb & MOUSE_LEFT);
        prevb = buttons;

        if (pressed) {
            for (int i = NWIN - 1; i >= 0; i--) {            // du dessus vers le dessous
                win_t *w = &win[i];
                if (!w->visible) continue;
                if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + TB) {
                    if (cx >= w->x + w->w - 18 && cx < w->x + w->w - 4) { w->visible = 0; break; }
                    drag = i; ddx = cx - w->x; ddy = cy - w->y; top = i; break;
                }
            }
        }
        if (released) drag = -1;
        if (drag >= 0 && (buttons & MOUSE_LEFT)) { win[drag].x = cx - ddx; win[drag].y = cy - ddy; }

        // --- Composition (dans le back-buffer) ---
        canvas_fill(&back, rgb(0x16, 0x18, 0x28));          // fond uni (rapide)
        for (int i = 0; i < NWIN; i++) draw_window(&back, &win[i], i == top);
        // curseur (petit losange blanc)
        canvas_fill_rect(&back, cx, cy, 8, 8, rgb(255, 255, 255));
        // bandeau d'info
        char up[24]; itoa_u(sys_time_ms() / 1000, up);
        canvas_draw_string(&back, "sexOs -- bureau en RING 3 (CPL 3)  uptime=", 12, 10,
                           rgb(0x9a, 0xc8, 0xff), 1);
        canvas_draw_string(&back, up, 12 + 42 * 8, 10, rgb(0x9a, 0xc8, 0xff), 1);

        canvas_blit(&screen, &back, 0, 0);
    }
    return 0;
}
