// =============================================================================
//  user/compositor.c -- Compositeur / serveur d'affichage RING 3 (processus 1)
// -----------------------------------------------------------------------------
//  Possède le framebuffer (sys_fb_map) et le flux d'entrées (sys_input_poll).
//  Les applications sont des PROCESSUS SÉPARÉS : elles demandent une fenêtre via
//  IPC, dessinent dans une mémoire PARTAGÉE (shm), et reçoivent les événements
//  routés vers la fenêtre au focus. Un crash d'application ne touche pas le
//  compositeur (isolation par le matériel : kill-on-fault côté noyau).
// =============================================================================
#include "sexos.h"
#include "gfx.h"
#include "wproto.h"
#include "imgdec.h"

void *memset(void *, int, unsigned long);
void *malloc(unsigned long);
char *strcpy(char *, const char *);

void utoa(unsigned long, char *);

#define WALL_PREF "/home/user/.wallpaper"   // memorise le chemin du fond d'ecran

typedef struct { uint8_t second, minute, hour, day, month; uint16_t year; } rtct_t;

#define TB     22          // hauteur de la barre de titre
#define MAXW   16
#define DOCK_H 30          // hauteur de la barre des tâches (dock)

typedef struct {
    int used, id, owner, shm, min;   // min = fenetre reduite (cachee, presente dans le dock)
    int max, resizable;               // maximisee ; autorise le redimensionnement
    int sx, sy, sw, sh;               // geometrie memorisee (avant maximisation)
    uint32_t *px;          // tampon partagé (mappé dans le compositeur)
    int x, y, w, h;
    char title[32];
} win_t;

static win_t wins[MAXW];
static int   next_id = 1, next_x = 80, next_y = 70;
static canvas_t screen, back, wall;        // wall = fond d'ecran precalcule

// --- notifications (toast) ---------------------------------------------------
static char     toast_msg[48];
static uint64_t toast_until;
static void notify(const char *m) {
    int i = 0; while (m[i] && i < 47) { toast_msg[i] = m[i]; i++; } toast_msg[i] = 0;
    toast_until = sys_time_ms() + 2500;
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static win_t *find(int id) { for (int i = 0; i < MAXW; i++) if (wins[i].used && wins[i].id == id) return &wins[i]; return 0; }
static int top_index = -1;     // dernière fenêtre cliquée (focus / dessus)
static int resizing = -1;      // fenêtre en cours de redimensionnement (bande élastique)
static int rs_w, rs_h;         // taille en cours (pendant le glissement)

// --- Session : ecran de connexion + utilisateur courant ----------------------
#define APP_LOGOUT (-2)            // entree speciale du menu : se deconnecter
static int authed;                 // 0 = ecran de connexion, 1 = bureau
static userinfo_t g_users[8];
static int  g_nusers, login_sel, login_pwlen;
static char login_pw[64], login_err[48];
static char cur_user[32]; static int cur_admin;

// --- Dock : menu de lancement d'applications ---------------------------------
//  L'ordre/les identifiants doivent correspondre aux constantes APP_* (noyau).
static int menu_open;
static const struct { const char *name; int app; uint32_t icon; } g_menu[] = {
    { "Terminal",     APP_TERMINAL, 0x2d6cdf },
    { "Explorateur",  APP_FILES,    0xe0c44f },
    { "Horloge",      APP_CLOCK,    0x6ee79a },
    { "Moniteur",     APP_MONITOR,  0xe07f7f },
    { "Navigateur",   APP_WEB,      0x4fb0e0 },
    { "Parametres",   APP_SETTINGS, 0xb48cf0 },
    { "Calculatrice", APP_CALC,     0xf0a020 },
    { "Dessin",       APP_PAINT,    0xf060a0 },
    { "Images",       APP_IMGVIEW,  0x60c0f0 },
    { "Editeur",      APP_EDITOR,   0x9ad06a },
    { "Deconnexion",  APP_LOGOUT,   0xe07f7f },
};
#define NMENU  ((int)(sizeof g_menu / sizeof g_menu[0]))
static const char *app_name(int app) {
    for (int k = 0; k < NMENU; k++) if (g_menu[k].app == app) return g_menu[k].name;
    return "application";
}
#define MENU_W 178
#define MENU_IH 26

// Traite un clic sur le dock / le menu. Renvoie 1 si le clic est consommé
// (et ne doit donc pas être routé vers une fenêtre).
static int handle_dock_click(int cx, int cy) {
    int dock_y = back.height - DOCK_H;
    int on_menu_btn = (cy >= dock_y && cx >= 6 && cx < 70);
    if (menu_open) {
        int ph = NMENU * MENU_IH + 8, px = 6, py = dock_y - ph;
        if (cx >= px && cx < px + MENU_W && cy >= py && cy < py + ph) {
            int idx = (cy - py - 4) / MENU_IH;
            if (idx >= 0 && idx < NMENU) {
                int app = g_menu[idx].app;
                if (app == APP_LOGOUT) { authed = 0; login_pwlen = 0; login_pw[0] = 0; login_err[0] = 0; }
                else { sys_spawn(app); notify(g_menu[idx].name); }
            }
            menu_open = 0; return 1;
        }
        menu_open = 0;                       // clic hors du panneau : referme
        if (on_menu_btn) return 1;           // (bascule du bouton : ne pas rouvrir)
        if (cy < dock_y) return 0;           // au-dessus du dock : laisse passer
        return 1;
    }
    if (cy < dock_y) return 0;               // clic dans les fenêtres
    if (on_menu_btn) { menu_open = 1; return 1; }
    // Boutons des fenêtres ouvertes : focus + premier plan (restaure si réduite).
    int bx = 80;
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) continue;
        if (cx >= bx && cx < bx + 140) { top_index = i; wins[i].min = 0; return 1; }
        bx += 146;
    }
    return 1;                                // zone vide de la barre : consommé
}

// Dessine le dock (barre des tâches), ses boutons et le menu déroulant.
// --- Icones d'applications (dessinees a la volee, ~16x16) ---------------------
static void ic_disc(int cx, int cy, int r, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        if (dx*dx + dy*dy <= r*r) canvas_put_pixel(&back, cx + dx, cy + dy, col);
}
static void ic_ring(int cx, int cy, int r, int r2, uint32_t col) {
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        { int d = dx*dx + dy*dy; if (d <= r*r && d >= r2*r2) canvas_put_pixel(&back, cx + dx, cy + dy, col); }
}
static void draw_app_icon(int app, int x, int y) {
    int cx = x + 8, cy = y + 8;
    uint32_t dark = rgb(0x10,0x14,0x1c), scr = rgb(0x0a,0x0e,0x14), grey = rgb(0x9a,0xa4,0xb6);
    switch (app) {
        case APP_TERMINAL:
            canvas_fill_rect(&back, x+1, y+1, 14, 14, rgb(0x18,0x1e,0x2a));
            canvas_fill_rect(&back, x+2, y+3, 12, 10, scr);
            canvas_draw_line(&back, x+4, y+5, x+7, y+8, rgb(0x6e,0xe7,0x9a));
            canvas_draw_line(&back, x+7, y+8, x+4, y+11, rgb(0x6e,0xe7,0x9a));
            canvas_fill_rect(&back, x+8, y+10, 4, 2, rgb(0x6e,0xe7,0x9a));
            break;
        case APP_FILES:
            canvas_fill_rect(&back, x+1, y+3, 6, 3, rgb(0xc8,0xa8,0x30));
            canvas_fill_rect(&back, x+1, y+5, 14, 9, rgb(0xe0,0xc4,0x4f));
            canvas_fill_rect(&back, x+1, y+5, 14, 1, rgb(0xf2,0xda,0x84));
            break;
        case APP_CLOCK:
            ic_disc(cx, cy, 7, rgb(0xeb,0xf0,0xf6)); ic_ring(cx, cy, 7, 6, rgb(0x2d,0x6c,0xdf));
            canvas_draw_line(&back, cx, cy, cx, cy-4, rgb(0x22,0x2a,0x3a));
            canvas_draw_line(&back, cx, cy, cx+3, cy+1, rgb(0x22,0x2a,0x3a));
            break;
        case APP_MONITOR:
            canvas_fill_rect(&back, x+1, y+2, 14, 11, rgb(0x18,0x1e,0x2a));
            canvas_fill_rect(&back, x+2, y+3, 12, 9, scr);
            canvas_fill_rect(&back, x+4, y+8, 2, 3, rgb(0x6e,0xe7,0x9a));
            canvas_fill_rect(&back, x+7, y+6, 2, 5, rgb(0xf0,0xc8,0x40));
            canvas_fill_rect(&back, x+10, y+5, 2, 6, rgb(0xe0,0x50,0x50));
            canvas_fill_rect(&back, x+5, y+13, 6, 1, grey);
            break;
        case APP_WEB:
            ic_disc(cx, cy, 7, rgb(0x3f,0x9c,0xd8)); ic_ring(cx, cy, 7, 6, rgb(0xe6,0xf2,0xff));
            canvas_draw_line(&back, cx-6, cy, cx+6, cy, rgb(0xe6,0xf2,0xff));
            canvas_draw_line(&back, cx, cy-7, cx, cy+7, rgb(0xe6,0xf2,0xff));
            ic_ring(cx, cy, 7, 6, rgb(0xe6,0xf2,0xff));
            canvas_draw_line(&back, cx-3, cy-6, cx-3, cy+6, rgb(0xd2,0xe8,0xf8));
            canvas_draw_line(&back, cx+3, cy-6, cx+3, cy+6, rgb(0xd2,0xe8,0xf8));
            break;
        case APP_SETTINGS: {
            ic_disc(cx, cy, 6, grey);
            canvas_fill_rect(&back, cx-1, y,    2, 3, grey); canvas_fill_rect(&back, cx-1, y+13, 2, 3, grey);
            canvas_fill_rect(&back, x,   cy-1, 3, 2, grey); canvas_fill_rect(&back, x+13, cy-1, 3, 2, grey);
            canvas_fill_rect(&back, x+2, y+2, 2, 2, grey);  canvas_fill_rect(&back, x+12, y+2, 2, 2, grey);
            canvas_fill_rect(&back, x+2, y+12, 2, 2, grey); canvas_fill_rect(&back, x+12, y+12, 2, 2, grey);
            ic_disc(cx, cy, 2, rgb(0x20,0x24,0x30));
            break; }
        case APP_CALC:
            canvas_fill_rect(&back, x+2, y+1, 12, 14, rgb(0x3a,0x40,0x52));
            canvas_fill_rect(&back, x+3, y+2, 10, 3, rgb(0xb8,0xe0,0xc0));
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
                canvas_fill_rect(&back, x+3 + c*4, y+7 + r*3, 2, 2, rgb(0xc8,0xd0,0xdc));
            break;
        case APP_PAINT:
            ic_disc(cx, cy, 7, rgb(0xe8,0xe0,0xd0));
            canvas_fill_rect(&back, x+4, y+4, 2, 2, rgb(0xe0,0x50,0x50));
            canvas_fill_rect(&back, x+9, y+4, 2, 2, rgb(0x2d,0x6c,0xdf));
            canvas_fill_rect(&back, x+11, y+8, 2, 2, rgb(0xf0,0xc8,0x40));
            canvas_fill_rect(&back, x+5, y+10, 2, 2, rgb(0x4c,0xc0,0x6a));
            ic_disc(cx+2, cy+2, 2, rgb(0x20,0x24,0x30));
            break;
        case APP_IMGVIEW:
            canvas_fill_rect(&back, x+1, y+2, 14, 12, rgb(0x9a,0xc8,0xff));
            canvas_draw_rect(&back, x+1, y+2, 14, 12, rgb(0xe6,0xec,0xf2));
            ic_disc(x+5, y+6, 2, rgb(0xf0,0xc8,0x40));
            for (int c = 2; c <= 13; c++) { int dd = c-9; if (dd<0) dd=-dd; int top = y+6+dd;
                if (top < y+13) canvas_draw_vline(&back, x+c, top, y+13-top, rgb(0x4c,0xc0,0x6a)); }
            break;
        case APP_EDITOR:
            canvas_fill_rect(&back, x+3, y+1, 10, 14, rgb(0xf2,0xf4,0xf8));
            canvas_fill_rect(&back, x+10, y+1, 3, 3, rgb(0xc8,0xd0,0xdc));
            for (int i = 0; i < 5; i++) canvas_fill_rect(&back, x+5, y+5 + i*2, 6, 1, rgb(0x8a,0x94,0xa4));
            break;
        case APP_LOGOUT:
            ic_ring(cx, cy, 6, 4, rgb(0xe0,0x60,0x60));
            canvas_fill_rect(&back, cx-3, y, 6, 4, rgb(0x20,0x24,0x30));   // ouverture en haut
            canvas_fill_rect(&back, cx-1, y+1, 2, 7, rgb(0xe0,0x60,0x60)); // barre verticale
            break;
        default:
            canvas_fill_rect(&back, x+1, y+1, 14, 14, grey);
            break;
    }
}

static void draw_dock(void) {
    int dock_y = back.height - DOCK_H;
    canvas_fill_rect(&back, 0, dock_y, back.width, DOCK_H, rgb(0x18, 0x1b, 0x26));
    canvas_fill_rect(&back, 0, dock_y, back.width, 1, rgb(0x2d, 0x34, 0x46));
    // Bouton Menu.
    canvas_fill_rect(&back, 6, dock_y + 5, 64, DOCK_H - 10, rgb(0x2d, 0x6c, 0xdf));
    canvas_draw_string(&back, "Menu", 22, dock_y + 9, rgb(255, 255, 255), 1);
    // Un bouton par fenêtre ouverte.
    int bx = 80;
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].used) continue;
        if (bx + 140 > (int)back.width - 90) break;
        uint32_t col = (i == top_index) ? rgb(0x3a, 0x42, 0x58) : rgb(0x24, 0x28, 0x34);
        canvas_fill_rect(&back, bx, dock_y + 5, 140, DOCK_H - 10, col);
        canvas_draw_string(&back, wins[i].title, bx + 8, dock_y + 9, rgb(0xe6, 0xec, 0xf2), 1);
        bx += 146;
    }
    // Horloge (heure réelle HH:MM:SS) à droite.
    rtct_t t; sys_rtc(&t);
    char hh[10];
    hh[0]='0'+t.hour/10;   hh[1]='0'+t.hour%10;   hh[2]=':';
    hh[3]='0'+t.minute/10; hh[4]='0'+t.minute%10; hh[5]=':';
    hh[6]='0'+t.second/10; hh[7]='0'+t.second%10; hh[8]=0;
    canvas_draw_string(&back, hh, back.width - 72, dock_y + 9, rgb(0xd0, 0xdc, 0xe8), 1);
    // Utilisateur connecté (nom + pastille admin/standard) à gauche de l'horloge.
    if (cur_user[0]) {
        char ub[48]; int p = 0;
        for (int i = 0; cur_user[i] && p < 31; i++) ub[p++] = cur_user[i];
        if (cur_admin) { const char *a = " (admin)"; for (int i = 0; a[i]; i++) ub[p++] = a[i]; }
        ub[p] = 0;
        int uw = canvas_text_width(ub, 1);
        canvas_fill_rect(&back, back.width - 86 - uw - 14, dock_y + 8, 8, 8,
                         cur_admin ? rgb(0xe0, 0x8a, 0x40) : rgb(0x4c, 0xc0, 0x6a));
        canvas_draw_string(&back, ub, back.width - 84 - uw, dock_y + 9, rgb(0xc8, 0xd0, 0xdc), 1);
    }
    // Menu déroulant (avec icônes colorées).
    if (menu_open) {
        int ph = NMENU * MENU_IH + 8, px = 6, py = dock_y - ph;
        canvas_fill_rect(&back, px, py, MENU_W, ph, rgb(0x20, 0x24, 0x30));
        canvas_draw_rect(&back, px, py, MENU_W, ph, rgb(0x3a, 0x42, 0x58));
        for (int k = 0; k < NMENU; k++) {
            int iy = py + 5 + k * MENU_IH;
            draw_app_icon(g_menu[k].app, px + 8, iy);
            canvas_draw_string(&back, g_menu[k].name, px + 30, iy + 4, rgb(0xff, 0xff, 0xff), 1);
        }
    }
}

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
    win->min = 0; win->max = 0; win->resizable = (req->flags & WIN_RESIZABLE) ? 1 : 0;
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

#define WIN_MINW 160
#define WIN_MINH 90

// Redimensionne une fenêtre : alloue un nouveau tampon partagé et prévient
// l'application (qui le mappe et se redessine).
static void do_resize(win_t *win, int nw, int nh) {
    if (nw < WIN_MINW) nw = WIN_MINW;
    if (nh < WIN_MINH) nh = WIN_MINH;
    if (nw > (int)back.width)  nw = (int)back.width;
    if (nh > (int)back.height - TB) nh = (int)back.height - TB;
    if (nw == win->w && nh == win->h) return;
    uint64_t va = 0;
    long shm = sys_shm_create((unsigned long)nw * nh * 4, &va);
    if (shm < 0) return;
    win->shm = (int)shm; win->px = (uint32_t *)(uintptr_t)va;
    win->w = nw; win->h = nh;
    for (int i = 0; i < nw * nh; i++) win->px[i] = rgb(0x20, 0x22, 0x2c);  // fond neutre
    wmsg_t m; memset(&m, 0, sizeof m);
    m.type = WMSG_RESIZE; m.win = win->id; m.shm = win->shm; m.w = nw; m.h = nh;
    sys_ipc_send(win->owner, &m, sizeof m);
}

// Bascule maximisé / restauré.
static void toggle_max(win_t *win) {
    if (!win->max) {
        win->sx = win->x; win->sy = win->y; win->sw = win->w; win->sh = win->h;
        win->x = 0; win->y = 0; win->max = 1;
        do_resize(win, (int)back.width, (int)back.height - TB - DOCK_H);
    } else {
        win->x = win->sx; win->y = win->sy; win->max = 0;
        do_resize(win, win->sw, win->sh);
    }
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
    // bouton reduire (orange) + glyphe barre
    canvas_fill_rect(&back, win->x + win->w - 54, win->y + 4, 14, 14, rgb(0xe0, 0xb0, 0x4f));
    canvas_fill_rect(&back, win->x + win->w - 51, win->y + 13, 8, 2, rgb(0x20, 0x20, 0x20));
    // bouton maximiser (vert, seulement si redimensionnable)
    if (win->resizable) {
        canvas_fill_rect(&back, win->x + win->w - 36, win->y + 4, 14, 14, rgb(0x4f, 0xc0, 0x6a));
        canvas_draw_rect(&back, win->x + win->w - 33, win->y + 7, 8, 8, rgb(0x18, 0x18, 0x18));
    }
    // bouton fermer (rouge)
    canvas_fill_rect(&back, win->x + win->w - 18, win->y + 4, 14, 14, rgb(0xe0, 0x4f, 0x4f));
    canvas_draw_string(&back, "x", win->x + win->w - 15, win->y + 4, rgb(255, 255, 255), 1);
    // contenu : tampon partagé de l'application
    canvas_t wc = { win->px, (uint32_t)win->w, (uint32_t)win->h, (uint32_t)win->w * 4 };
    canvas_blit(&back, &wc, win->x, win->y + TB);
    canvas_draw_rect(&back, win->x, win->y, win->w, win->h + TB, rgb(0x55, 0x5a, 0x6a));
    // poignee de redimensionnement (coin bas-droit) si redimensionnable
    if (win->resizable) {
        int gy = win->y + TB + win->h - 14;
        for (int k = 2; k <= 12; k += 4)
            canvas_draw_line(&back, win->x + win->w - 14 + k, win->y + TB + win->h - 2,
                             win->x + win->w - 2, gy + k, rgb(0x88, 0x90, 0xa0));
    }
}

// --- Composition optimisée (rectangles modifiés + curseur « save-under ») ----
//  Le framebuffer est lent en écriture (write-through). Plutôt que de recopier
//  tout l'écran à chaque trame, on ne pousse vers le framebuffer que les zones
//  réellement modifiées : déplacement du curseur (8x8), fenêtre déplacée/fermée,
//  fenêtre redessinée (WMSG_DAMAGE), horloge du dock. Le curseur est tracé
//  directement sur l'écran ; 'back' contient le bureau SANS curseur.
static int d_x0, d_y0, d_x1, d_y1, d_any, d_full;
static void dirty_reset(void) { d_any = 0; d_full = 0; d_x0 = d_y0 = 1<<29; d_x1 = d_y1 = -(1<<29); }
static void dirty_add(int x, int y, int w, int h) {
    if (x < d_x0) d_x0 = x; if (y < d_y0) d_y0 = y;
    if (x + w > d_x1) d_x1 = x + w; if (y + h > d_y1) d_y1 = y + h; d_any = 1;
}
static void dirty_full(void) { d_full = 1; d_any = 1; }

// --- Curseur fleche (plus lisible) : contour noir + remplissage blanc --------
//  'X' = contour (noir), '.' = remplissage (blanc), ' ' = transparent.
#define CUR_W 12
#define CUR_H 19
static const char *cursor_bmp[CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X.....XXXXXX",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "       XX   ",
};

// Peint screen[rect] = back[rect] avec le CURSEUR (fleche) superposé là où il se
// trouve (curx,cury). Chaque pixel reçoit DIRECTEMENT sa valeur finale en un seul
// passage : il n'existe jamais d'état intermédiaire « effacé » à l'écran, donc
// pas de clignotement (contrairement à un effacer-puis-redessiner en deux temps).
static void paint(int x, int y, int w, int h, int curx, int cury) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)screen.width)  w = (int)screen.width  - x;
    if (y + h > (int)screen.height) h = (int)screen.height - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        const uint32_t *s = (const uint32_t *)((const uint8_t *)back.pixels + yy * back.pitch);
        uint32_t *dd = (uint32_t *)((uint8_t *)screen.pixels + yy * screen.pitch);
        int cr = yy - cury;
        int in_cur_row = (cr >= 0 && cr < CUR_H);
        for (int xx = x; xx < x + w; xx++) {
            uint32_t v = s[xx];
            if (in_cur_row) {
                int cc = xx - curx;
                if (cc >= 0 && cc < CUR_W) {
                    char p = cursor_bmp[cr][cc];
                    if (p == 'X') v = 0x000000;
                    else if (p == '.') v = 0xFFFFFF;
                }
            }
            dd[xx] = v;
        }
    }
}
// Recompose tout le bureau (sans curseur) dans 'back' (mémoire cache : rapide).
static void compose_back(void) {
    canvas_blit(&back, &wall, 0, 0);          // fond d'ecran (degrade precalcule)
    canvas_draw_string(&back, "sexOs -- bureau ring 3 : cliquez sur \"Menu\" (en bas) pour lancer une application",
                       12, 8, rgb(0x9a, 0xc8, 0xff), 1);
    for (int i = 0; i < MAXW; i++) if (wins[i].used && !wins[i].min && i != top_index) draw_window(&wins[i], 0);
    if (top_index >= 0 && wins[top_index].used && !wins[top_index].min) draw_window(&wins[top_index], 1);
    // bande élastique pendant un redimensionnement
    if (resizing >= 0 && wins[resizing].used) {
        win_t *w = &wins[resizing];
        canvas_draw_rect(&back, w->x, w->y, rs_w, rs_h + TB, rgb(0x9a, 0xc8, 0xff));
        canvas_draw_rect(&back, w->x + 1, w->y + 1, rs_w - 2, rs_h + TB - 2, rgb(0x9a, 0xc8, 0xff));
    }
    draw_dock();
    // notification (toast) au-dessus du dock
    if (sys_time_ms() < toast_until && toast_msg[0]) {
        int tw = canvas_text_width(toast_msg, 1) + 24;
        int tx = ((int)back.width - tw) / 2, ty = (int)back.height - DOCK_H - 40;
        canvas_fill_rect(&back, tx, ty, tw, 26, rgb(0x2d, 0x34, 0x46));
        canvas_draw_rect(&back, tx, ty, tw, 26, rgb(0x4a, 0x90, 0xe0));
        canvas_draw_string(&back, toast_msg, tx + 12, ty + 9, rgb(0xe6, 0xec, 0xf2), 1);
    }
}

// Construit le fond d'ecran (degrade vertical bleu nuit -> violet) une fois.
static void build_wallpaper(void) {
    for (int y = 0; y < (int)wall.height; y++) {
        int t = (y * 255) / (int)wall.height;
        uint8_t r = 0x12 + (uint8_t)((t * 0x1c) / 255);
        uint8_t g = 0x14 + (uint8_t)((t * 0x10) / 255);
        uint8_t b = 0x2a + (uint8_t)((t * 0x34) / 255);
        canvas_fill_rect(&wall, 0, y, (int)wall.width, 1, rgb(r, g, b));
    }
}

// Décode 'path' et le met à l'échelle « cover » (remplit l'écran, recadre) dans
// le tampon du fond d'écran. Renvoie 0 si OK, -1 sinon (le dégradé est conservé).
#define WP_MAXW 1280
#define WP_MAXH 1024
static int set_wallpaper_image(const char *path) {
    static uint8_t *fbuf; static uint32_t *ibuf;
    if (!fbuf) fbuf = malloc(6u << 20);
    if (!ibuf) ibuf = malloc((unsigned long)WP_MAXW * WP_MAXH * 4);
    if (!fbuf || !ibuf) return -1;
    vfs_io_t io = { path, 0, fbuf, 6u << 20 };
    long n = sys_vfs_read(&io);
    if (n <= 0) return -1;
    int iw, ih;
    if (!img_decode(fbuf, (int)n, ibuf, WP_MAXW, WP_MAXH, &iw, &ih)) return -1;
    int W = (int)wall.width, H = (int)wall.height;
    long scale = ((long)W << 16) / iw, s2 = ((long)H << 16) / ih;
    if (s2 > scale) scale = s2;                       // « cover » : le plus grand facteur
    if (scale < 1) scale = 1;
    int sw = (int)(((long)iw * scale) >> 16), sh = (int)(((long)ih * scale) >> 16);
    int ox = (W - sw) / 2, oy = (H - sh) / 2;
    for (int y = 0; y < H; y++) {
        int syy = (int)((((long)(y - oy)) << 16) / scale);
        if (syy < 0) syy = 0; if (syy >= ih) syy = ih - 1;
        for (int x = 0; x < W; x++) {
            int sxx = (int)((((long)(x - ox)) << 16) / scale);
            if (sxx < 0) sxx = 0; if (sxx >= iw) sxx = iw - 1;
            wall.pixels[y * wall.width + x] = ibuf[syy * iw + sxx];
        }
    }
    return 0;
}

// Mémorise / restaure le choix de fond d'écran (persiste si un disque est présent).
static void save_wallpaper_pref(const char *path) {
    sys_vfs_create(WALL_PREF, 0);
    vfs_io_t io = { WALL_PREF, 0, (void *)path, (uint64_t)0 };
    unsigned long l = 0; while (path[l]) l++; io.len = l;
    sys_vfs_save(&io);
}
static void load_wallpaper_pref(void) {
    static char path[256];
    vfs_io_t io = { WALL_PREF, 0, path, sizeof(path) - 1 };
    long n = sys_vfs_read(&io);
    if (n > 0) { path[n] = 0; set_wallpaper_image(path); }
}

// --- Ecran de connexion ------------------------------------------------------
#define LG_ROW 30
static void login_geom(int *px, int *py, int *pw, int *ph, int *listy) {
    int W = back.width, H = back.height;
    int pwid = 380, phgt = 160 + g_nusers * LG_ROW;
    *px = (W - pwid) / 2; *py = (H - phgt) / 2; *pw = pwid; *ph = phgt;
    *listy = *py + 70;
}
static int login_user_at(int cx, int cy) {
    int px, py, pw, ph, ly; login_geom(&px, &py, &pw, &ph, &ly);
    if (cx < px + 12 || cx > px + pw - 12) return -1;
    for (int i = 0; i < g_nusers; i++) { int ry = ly + i * LG_ROW; if (cy >= ry - 2 && cy < ry + LG_ROW - 2) return i; }
    return -1;
}
static void compose_login(void) {
    canvas_blit(&back, &wall, 0, 0);
    int px, py, pw, ph, ly; login_geom(&px, &py, &pw, &ph, &ly);
    canvas_fill_rect(&back, px, py, pw, ph, rgb(0x1a, 0x1f, 0x2b));
    canvas_draw_rect(&back, px, py, pw, ph, rgb(0x3a, 0x44, 0x58));
    canvas_draw_string(&back, "sexOs", px + 20, py + 14, rgb(0x9a, 0xc8, 0xff), 2);
    canvas_draw_string(&back, "Connexion -- choisissez un compte", px + 20, py + 44, rgb(0x9a, 0xa0, 0xb4), 1);
    for (int i = 0; i < g_nusers; i++) {
        int ry = ly + i * LG_ROW;
        if (i == login_sel) canvas_fill_rect(&back, px + 12, ry - 2, pw - 24, LG_ROW - 2, rgb(0x2d, 0x6c, 0xdf));
        canvas_fill_rect(&back, px + 20, ry + 3, 16, 16, g_users[i].is_admin ? rgb(0xe0, 0x8a, 0x40) : rgb(0x4c, 0xc0, 0x6a));
        canvas_draw_string(&back, g_users[i].name, px + 44, ry + 4, rgb(0xff, 0xff, 0xff), 1);
        if (g_users[i].is_admin)
            canvas_draw_string(&back, "(admin)", px + 44 + canvas_text_width(g_users[i].name, 1) + 10, ry + 4, rgb(0xe0, 0xb0, 0x60), 1);
    }
    int fy = ly + g_nusers * LG_ROW + 14;
    canvas_draw_string(&back, "Mot de passe :", px + 20, fy, rgb(0xc8, 0xd0, 0xdc), 1);
    int bx = px + 20, by = fy + 18, bw = pw - 40, bh = 22;
    canvas_fill_rect(&back, bx, by, bw, bh, rgb(0x0e, 0x12, 0x18));
    canvas_draw_rect(&back, bx, by, bw, bh, rgb(0x3a, 0x44, 0x58));
    char dots[64]; int n = login_pwlen < 60 ? login_pwlen : 60; for (int i = 0; i < n; i++) dots[i] = '*'; dots[n] = 0;
    canvas_draw_string(&back, dots, bx + 6, by + 6, rgb(0xff, 0xff, 0x99), 1);
    canvas_draw_string(&back, "[Entree] se connecter   [Haut/Bas] changer de compte", px + 20, by + bh + 9, rgb(0x6a, 0x76, 0x86), 1);
    if (login_err[0]) canvas_draw_string(&back, login_err, px + 20, by + bh + 25, rgb(0xff, 0x80, 0x80), 1);
}
static void do_login(void) {
    login_pw[login_pwlen] = 0;
    if (login_sel < 0 || login_sel >= g_nusers) return;
    if (sys_login(g_users[login_sel].name, login_pw) == 0) {
        authed = 1;
        int i = 0; while (g_users[login_sel].name[i] && i < 31) { cur_user[i] = g_users[login_sel].name[i]; i++; }
        cur_user[i] = 0; cur_admin = g_users[login_sel].is_admin;
        login_pwlen = 0; login_pw[0] = 0; login_err[0] = 0;
    } else { strcpy(login_err, "mot de passe incorrect"); login_pwlen = 0; login_pw[0] = 0; }
}
static int login_event(const event_t *e) {
    if (e->type == EV_MOUSE) {
        if (e->buttons & MOUSE_LEFT) { int u = login_user_at(e->mx, e->my); if (u >= 0) { login_sel = u; return 1; } }
        return 0;
    }
    if (e->type == EV_KEY && e->pressed) {
        if (e->key == KEY_ENTER) { do_login(); return 1; }
        if (e->key == KEY_BACKSPACE) { if (login_pwlen > 0) login_pw[--login_pwlen] = 0; return 1; }
        if (e->key == KEY_UP)   { if (login_sel > 0) login_sel--; return 1; }
        if (e->key == KEY_DOWN || e->key == KEY_TAB) { if (g_nusers) login_sel = (login_sel + 1) % g_nusers; return 1; }
        if (e->ch >= ' ' && login_pwlen < (int)sizeof(login_pw) - 1) { login_pw[login_pwlen++] = e->ch; login_pw[login_pwlen] = 0; return 1; }
    }
    return 0;
}

int main(void) {
    sys_comp_register();
    fbinfo_t fb; if (sys_fb_map(&fb)) return 1;
    screen.pixels = (uint32_t *)(uintptr_t)fb.addr;
    screen.width = fb.width; screen.height = fb.height; screen.pitch = fb.pitch;
    back.width = fb.width; back.height = fb.height; back.pitch = fb.width * 4;
    back.pixels = (uint32_t *)sys_alloc((unsigned long)back.pitch * back.height);
    if (!back.pixels) return 2;
    wall.width = fb.width; wall.height = fb.height; wall.pitch = fb.width * 4;
    wall.pixels = (uint32_t *)sys_alloc((unsigned long)wall.pitch * wall.height);
    if (!wall.pixels) return 2;
    build_wallpaper();
    load_wallpaper_pref();                 // restaure un fond d'ecran personnalise s'il existe

    // Comptes pour l'ecran de connexion (au demarrage : verrouille).
    g_nusers = 0;
    for (int i = 0; i < 8 && sys_users_list(i, &g_users[i]) == 1; i++) g_nusers++;
    authed = 0;

    int cx = fb.width / 2, cy = fb.height / 2, prevb = 0;
    int drag = -1, ddx = 0, ddy = 0;
    int pcx = cx, pcy = cy;                  // position précédente du curseur (écran)
    int need_recompose = 1;                  // première trame : tout dessiner
    uint64_t last_reap = 0, last_sec = (uint64_t)-1;

    for (;;) {
        dirty_reset();

        // (1) Messages des applications.
        wmsg_t msg; int sender;
        while (sys_ipc_recv(&msg, sizeof msg, &sender) > 0) {
            if (msg.type == WMSG_CREATE) { handle_create(sender, &msg); need_recompose = 1; dirty_full(); }
            else if (msg.type == WMSG_DESTROY) {
                win_t *w = find(msg.win);
                if (w) { dirty_add(w->x, w->y, w->w, w->h + TB); w->used = 0; need_recompose = 1; }
            } else if (msg.type == WMSG_DAMAGE) {           // le tampon de l'appli a changé
                win_t *w = find(msg.win);
                if (w) { need_recompose = 1; dirty_add(w->x, w->y, w->w, w->h + TB); }
            } else if (msg.type == WMSG_LAUNCH) {           // une appli demande d'en lancer une autre
                if (msg.w >= 0 && msg.w < APP_COUNT) { sys_spawn(msg.w); notify(app_name(msg.w)); }
            } else if (msg.type == WMSG_WALLPAPER) {        // changer le fond d'ecran
                static char wp[256];
                if (sys_arg_get(wp, sizeof wp) > 0 && wp[0] && set_wallpaper_image(wp) == 0) {
                    save_wallpaper_pref(wp); notify("fond d'ecran applique");
                } else notify("image de fond illisible");
                need_recompose = 1; dirty_full();
            }
        }

        // (2) Entrées : focus / déplacement / fermeture, sinon routage au focus.
        event_t e; int buttons = prevb;
        while (sys_input_poll(&e)) {
            if (e.type == EV_MOUSE) { cx = e.mx; cy = e.my; }   // suivre le curseur meme verrouille
            if (!authed) { if (login_event(&e)) { need_recompose = 1; dirty_full(); } continue; }
            if (e.type == EV_MOUSE) {
                cx = e.mx; cy = e.my; buttons = e.buttons;
                int pressed = (buttons & MOUSE_LEFT) && !(prevb & MOUSE_LEFT);
                int released = !(buttons & MOUSE_LEFT) && (prevb & MOUSE_LEFT);
                prevb = buttons;
                if (pressed) {
                    if (handle_dock_click(cx, cy)) { need_recompose = 1; dirty_full(); continue; }
                    for (int i = MAXW - 1; i >= 0; i--) {
                        // parcourt dans l'ordre de la pile (le focus en dernier)
                        int idx = (top_index >= 0) ? (top_index - i + 2 * MAXW) % MAXW : i;
                        win_t *w = &wins[idx];
                        if (!w->used || w->min) continue;
                        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h + TB) {
                            top_index = idx; need_recompose = 1; dirty_full();
                            if (w->resizable && cx >= w->x + w->w - 16 && cy >= w->y + TB + w->h - 16) {
                                resizing = idx; rs_w = w->w; rs_h = w->h;   // poignee de redim.
                            } else if (cy < w->y + TB) {                    // barre de titre
                                if (cx >= w->x + w->w - 18 && cx < w->x + w->w - 4) {
                                    wmsg_t c; memset(&c, 0, sizeof c); c.type = WMSG_CLOSE; c.win = w->id;
                                    sys_ipc_send(w->owner, &c, sizeof c);
                                    w->used = 0;
                                } else if (w->resizable && cx >= w->x + w->w - 36 && cx < w->x + w->w - 22) {
                                    toggle_max(w);                          // maximiser/restaurer
                                } else if (cx >= w->x + w->w - 54 && cx < w->x + w->w - 40) {
                                    w->min = 1; top_index = -1;             // reduire
                                } else { drag = idx; ddx = cx - w->x; ddy = cy - w->y; }
                            } else {
                                send_event(w, &e);      // clic dans le contenu
                            }
                            break;
                        }
                    }
                } else if (released) {
                    prevb = buttons; drag = -1;
                    if (resizing >= 0) {                                    // applique le redim.
                        do_resize(&wins[resizing], rs_w, rs_h);
                        resizing = -1; need_recompose = 1; dirty_full();
                    }
                } else {
                    if (resizing >= 0) {
                        win_t *w = &wins[resizing];
                        int ow = rs_w > w->w ? rs_w : w->w, oh = rs_h > w->h ? rs_h : w->h;
                        dirty_add(w->x, w->y, ow + 4, oh + TB + 4);
                        rs_w = cx - w->x; rs_h = cy - (w->y + TB);
                        if (rs_w < WIN_MINW) rs_w = WIN_MINW;
                        if (rs_h < WIN_MINH) rs_h = WIN_MINH;
                        dirty_add(w->x, w->y, rs_w + 4, rs_h + TB + 4);
                        need_recompose = 1;
                    } else if (drag >= 0) {
                        dirty_add(wins[drag].x, wins[drag].y, wins[drag].w, wins[drag].h + TB);  // ancienne position
                        wins[drag].x = cx - ddx; wins[drag].y = cy - ddy;
                        dirty_add(wins[drag].x, wins[drag].y, wins[drag].w, wins[drag].h + TB);  // nouvelle position
                        need_recompose = 1;
                    } else if (top_index >= 0 && wins[top_index].used) send_event(&wins[top_index], &e);
                }
            } else if (e.type == EV_KEY) {
                if (top_index >= 0 && wins[top_index].used) send_event(&wins[top_index], &e);
            }
        }

        // (2b) Horloge du dock : rafraîchie chaque seconde (zone du dock seulement).
        uint64_t now = sys_time_ms();
        if (now / 1000 != last_sec) {
            last_sec = now / 1000; need_recompose = 1;
            dirty_add(0, (int)back.height - DOCK_H, (int)back.width, DOCK_H);
            dirty_add(0, (int)back.height - DOCK_H - 44, (int)back.width, 44);  // bande notifications
        }

        // (2c) Récupération des fenêtres orphelines (~toutes les 500 ms) : si le
        //  PROCESSUS propriétaire est mort, on libère le slot (la fenêtre disparaît).
        if (now - last_reap > 500) {
            last_reap = now;
            for (int i = 0; i < MAXW; i++)
                if (wins[i].used && !sys_pid_alive(wins[i].owner)) {
                    wins[i].used = 0; if (top_index == i) top_index = -1;
                    need_recompose = 1; dirty_full();
                }
        }

        // (3) Mise à jour de l'écran : on ne pousse au framebuffer (lent) que les
        //  zones modifiées, et le curseur est composé EN UN SEUL PASSAGE avec le
        //  fond (jamais d'état « effacé » visible -> pas de clignotement). Quand le
        //  curseur se déplace, on dessine la NOUVELLE position AVANT d'effacer
        //  l'ancienne : le curseur reste donc visible en permanence.
        if (need_recompose) {
            if (authed) compose_back(); else compose_login();
            if (d_full || !d_any) paint(0, 0, (int)screen.width, (int)screen.height, cx, cy);
            else paint(d_x0, d_y0, d_x1 - d_x0, d_y1 - d_y0, cx, cy);
        }
        if (cx != pcx || cy != pcy) {
            paint(cx, cy, CUR_W, CUR_H, cx, cy);          // nouvelle position D'ABORD (curseur dessiné)
            paint(pcx, pcy, CUR_W, CUR_H, cx, cy);        // efface l'ancienne (curseur déjà ailleurs)
            pcx = cx; pcy = cy;
        } else if (!need_recompose) {
            paint(cx, cy, CUR_W, CUR_H, cx, cy);          // au repos : confirme le curseur (idempotent)
        }
        // Framebuffer en Write-Combining : SFENCE vide le tampon WC du CPU pour que
        // tout (curseur compris) atteigne la VRAM immédiatement.
        __asm__ volatile ("sfence" ::: "memory");
        // Dort jusqu'au prochain événement (entrée/IPC) ou ~100 ms (horloge,
        // récupération des fenêtres orphelines). Ne consomme plus le CPU au repos
        // et réagit instantanément aux entrées -> mouvements fluides.
        sys_wait_event(100);
    }
}
