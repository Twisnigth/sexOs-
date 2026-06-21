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
            if (idx >= 0 && idx < NMENU) { sys_spawn(g_menu[idx].app); notify(g_menu[idx].name); }
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
    // Menu déroulant (avec icônes colorées).
    if (menu_open) {
        int ph = NMENU * MENU_IH + 8, px = 6, py = dock_y - ph;
        canvas_fill_rect(&back, px, py, MENU_W, ph, rgb(0x20, 0x24, 0x30));
        canvas_draw_rect(&back, px, py, MENU_W, ph, rgb(0x3a, 0x42, 0x58));
        for (int k = 0; k < NMENU; k++) {
            int iy = py + 6 + k * MENU_IH;
            canvas_fill_rect(&back, px + 8, iy, 14, 14, g_menu[k].icon);
            canvas_draw_rect(&back, px + 8, iy, 14, 14, rgb(0x10, 0x12, 0x18));
            canvas_draw_string(&back, g_menu[k].name, px + 30, iy + 2, rgb(0xff, 0xff, 0xff), 1);
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
            compose_back();
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
