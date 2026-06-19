// =============================================================================
//  user/compositor.c -- Compositeur / serveur d'affichage RING 3 (processus 1)
// -----------------------------------------------------------------------------
//  Possède le framebuffer (sys_fb_map) et le flux d'entrées (sys_input_poll).
//  Les applications sont des PROCESSUS SÉPARÉS : elles demandent une fenêtre via
//  IPC, dessinent dans une mémoire PARTAGÉE (shm), et reçoivent les événements
//  routés vers la fenêtre au focus. Un crash d'application ne touche pas le
//  compositeur (isolation par le matériel : kill-on-fault côté noyau).
// =============================================================================
#include "monos.h"
#include "gfx.h"
#include "wproto.h"

void *memset(void *, int, unsigned long);

#define TB    22           // hauteur de la barre de titre
#define MAXW  16

typedef struct {
    int used, id, owner, shm;
    uint32_t *px;          // tampon partagé (mappé dans le compositeur)
    int x, y, w, h;
    char title[32];
} win_t;

static win_t wins[MAXW];
static int   next_id = 1, next_x = 80, next_y = 70;
static canvas_t screen, back;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static win_t *find(int id) { for (int i = 0; i < MAXW; i++) if (wins[i].used && wins[i].id == id) return &wins[i]; return 0; }
static int top_index = -1;     // dernière fenêtre cliquée (focus / dessus)

// --- Création d'une fenêtre demandée par une application ---------------------
static void handle_create(int owner, wmsg_t *req) {
    int slot = -1; for (int i = 0; i < MAXW; i++) if (!wins[i].used) { slot = i; break; }
    if (slot < 0) return;
    int w = req->w, h = req->h;
    uint64_t va = 0;
    long shm = sys_shm_create((unsigned long)w * h * 4, &va);
    if (shm < 0) return;
    win_t *win = &wins[slot];
    win->used = 1; win->id = next_id++; win->owner = owner; win->shm = (int)shm;
    win->px = (uint32_t *)(uintptr_t)va; win->w = w; win->h = h;
    win->x = next_x; win->y = next_y; next_x += 40; next_y += 36;
    if (next_x > 600) { next_x = 80; next_y = 70; }
    int i = 0; while (req->title[i] && i < 31) { win->title[i] = req->title[i]; i++; } win->title[i] = 0;
    top_index = slot;
    // Réponse à l'application : identifiants fenêtre + mémoire partagée.
    wmsg_t r; memset(&r, 0, sizeof r);
    r.type = WMSG_CREATED; r.win = win->id; r.shm = win->shm; r.w = w; r.h = h;
    sys_ipc_send(owner, &r, sizeof r);
}

static void send_event(win_t *win, const event_t *e) {
    wmsg_t m; memset(&m, 0, sizeof m);
    m.type = WMSG_EVENT; m.win = win->id; m.ev = *e;
    m.ev.mx = e->mx - win->x;                 // coordonnées locales à la fenêtre
    m.ev.my = e->my - (win->y + TB);
    sys_ipc_send(win->owner, &m, sizeof m);
}

static void draw_window(win_t *win, int focused) {
    uint32_t tb = focused ? rgb(0x2d, 0x6c, 0xdf) : rgb(0x3a, 0x42, 0x58);
    canvas_fill_rect(&back, win->x, win->y, win->w, win->h + TB, rgb(0x20, 0x22, 0x2c));
    canvas_fill_rect(&back, win->x, win->y, win->w, TB, tb);
    canvas_draw_string(&back, win->title, win->x + 8, win->y + 4, rgb(255, 255, 255), 1);
    canvas_fill_rect(&back, win->x + win->w - 18, win->y + 4, 14, 14, rgb(0xe0, 0x4f, 0x4f));
    canvas_draw_string(&back, "x", win->x + win->w - 15, win->y + 4, rgb(255, 255, 255), 1);
    // contenu : tampon partagé de l'application
    canvas_t wc = { win->px, (uint32_t)win->w, (uint32_t)win->h, (uint32_t)win->w * 4 };
    canvas_blit(&back, &wc, win->x, win->y + TB);
    canvas_draw_rect(&back, win->x, win->y, win->w, win->h + TB, rgb(0x55, 0x5a, 0x6a));
}

int main(void) {
    sys_comp_register();
    fbinfo_t fb; if (sys_fb_map(&fb)) return 1;
    screen.pixels = (uint32_t *)(uintptr_t)fb.addr;
    screen.width = fb.width; screen.height = fb.height; screen.pitch = fb.pitch;
    back.width = fb.width; back.height = fb.height; back.pitch = fb.width * 4;
    back.pixels = (uint32_t *)sys_alloc((unsigned long)back.pitch * back.height);
    if (!back.pixels) return 2;

    int cx = fb.width / 2, cy = fb.height / 2, prevb = 0;
    int drag = -1, ddx = 0, ddy = 0, reap = 0;

    for (;;) {
        // (1) Messages des applications.
        wmsg_t msg; int sender;
        while (sys_ipc_recv(&msg, sizeof msg, &sender) > 0) {
            if (msg.type == WMSG_CREATE) handle_create(sender, &msg);
            else if (msg.type == WMSG_DESTROY) { win_t *w = find(msg.win); if (w) w->used = 0; }
            // WMSG_DAMAGE : on recompose chaque trame, rien à faire ici.
        }

        // (2) Entrées : focus / déplacement / fermeture, sinon routage au focus.
        event_t e; int buttons = prevb;
        while (sys_input_poll(&e)) {
            if (e.type == EV_MOUSE) {
                cx = e.mx; cy = e.my; buttons = e.buttons;
                int pressed = (buttons & MOUSE_LEFT) && !(prevb & MOUSE_LEFT);
                int released = !(buttons & MOUSE_LEFT) && (prevb & MOUSE_LEFT);
                prevb = buttons;
                if (pressed) {
                    for (int i = MAXW - 1; i >= 0; i--) {
                        // parcourt dans l'ordre de la pile (le focus en dernier)
                        int idx = (top_index >= 0) ? (top_index - i + 2 * MAXW) % MAXW : i;
                        win_t *w = &wins[idx];
                        if (!w->used) continue;
                        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h + TB) {
                            top_index = idx;
                            if (cy < w->y + TB) {       // barre de titre
                                if (cx >= w->x + w->w - 18 && cx < w->x + w->w - 4) {
                                    wmsg_t c; memset(&c, 0, sizeof c); c.type = WMSG_CLOSE; c.win = w->id;
                                    sys_ipc_send(w->owner, &c, sizeof c);
                                    w->used = 0;
                                } else { drag = idx; ddx = cx - w->x; ddy = cy - w->y; }
                            } else {
                                send_event(w, &e);      // clic dans le contenu
                            }
                            break;
                        }
                    }
                } else if (released) {
                    prevb = buttons; drag = -1;
                } else {
                    if (drag >= 0) { wins[drag].x = cx - ddx; wins[drag].y = cy - ddy; }
                    else if (top_index >= 0 && wins[top_index].used) send_event(&wins[top_index], &e);
                }
            } else if (e.type == EV_KEY) {
                if (top_index >= 0 && wins[top_index].used) send_event(&wins[top_index], &e);
            }
        }

        // (2b) Récupération des fenêtres orphelines : si le PROCESSUS
        //  propriétaire est mort (crash tué par le noyau, ou sortie sans fermer
        //  sa fenêtre), on libère le slot pour que la fenêtre disparaisse.
        if (++reap == 30) {
            reap = 0;
            for (int i = 0; i < MAXW; i++)
                if (wins[i].used && !sys_pid_alive(wins[i].owner)) {
                    wins[i].used = 0;
                    if (top_index == i) top_index = -1;
                }
        }

        // (3) Composition.
        canvas_fill(&back, rgb(0x16, 0x18, 0x28));
        canvas_draw_string(&back, "MonOS -- compositeur ring 3 : chaque fenetre est un PROCESSUS separe",
                           12, 8, rgb(0x9a, 0xc8, 0xff), 1);
        for (int i = 0; i < MAXW; i++) if (wins[i].used && i != top_index) draw_window(&wins[i], 0);
        if (top_index >= 0 && wins[top_index].used) draw_window(&wins[top_index], 1);
        canvas_fill_rect(&back, cx, cy, 8, 8, rgb(255, 255, 255));   // curseur
        canvas_blit(&screen, &back, 0, 0);
        sys_yield();                 // commutation coopérative (pas de busy-poll)
    }
}
