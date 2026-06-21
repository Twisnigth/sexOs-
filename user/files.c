// =============================================================================
//  user/files.c -- EXPLORATEUR de fichiers en PROCESSUS ring 3 séparé.
// -----------------------------------------------------------------------------
//  Client du compositeur (fenêtre via libwin). Le système de fichiers est
//  manipulé UNIQUEMENT par appels système (sys_vfs_*) : aucun pointeur noyau.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"

void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
int strcmp(const char *, const char *);
char *strcat(char *, const char *);

#define W       560
#define H       420
#define ROW_H   20
#define LIST_Y  50
#define MAXENT  256

static canvas_t *cv;
static char      cwd[256];
static int       sel;
static int       count;                 // nombre d'entrées listées
static dirent_t  ents[MAXENT];
static int       mode;                   // 0=normal, 1=nouveau dossier, 2=nouveau fichier
static char      input[64];
static int       ilen;
static char      status[96];
static char      preview[512];           // aperçu du fichier sélectionné
static int       preview_len;

static const char *toolbar[] = { "Haut", "Ouvrir", "NvDossier", "NvFichier", "Suppr", "Actualiser", "CleUSB" };
#define NBTN (int)(sizeof(toolbar)/sizeof(toolbar[0]))

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

static void set_status(const char *s) { int i=0; while (s[i] && i<95) { status[i]=s[i]; i++; } status[i]=0; }

// Reconstruit la liste des entrées du dossier courant via les appels système.
static void reload(void) {
    count = 0;
    dirent_t e;
    for (int i = 0; count < MAXENT && sys_vfs_list(cwd, i, &e) == 1; i++) ents[count++] = e;
    if (sel >= count) sel = count ? count - 1 : 0;
    preview_len = 0;
}

// Construit cwd + nom -> out (chemin absolu).
static void join(const char *name, char *out) {
    strcpy(out, cwd);
    if (strcmp(cwd, "/") != 0) strcat(out, "/");
    strcat(out, name);
}

static void go_up(void) {
    if (strcmp(cwd, "/") == 0) return;
    int l = strlen(cwd);
    while (l > 1 && cwd[l-1] != '/') l--;
    if (l > 1) l--;
    cwd[l ? l : 1] = 0; if (!l) { cwd[0] = '/'; cwd[1] = 0; }
    sel = 0; reload();
}

// Ouvre l'entrée sélectionnée : dossier -> on y entre ; fichier -> aperçu.
static void open_selected(void) {
    if (sel < 0 || sel >= count) return;
    dirent_t *e = &ents[sel];
    char path[256]; join(e->name, path);
    if (e->type == 1) { strcpy(cwd, path); sel = 0; reload(); set_status(""); return; }
    vfs_io_t io = { path, 0, preview, sizeof(preview) - 1 };
    long n = sys_vfs_read(&io);
    if (n < 0) { preview_len = 0; set_status("lecture impossible"); return; }
    preview[n] = 0; preview_len = (int)n; set_status("apercu");
}

static void do_delete(void) {
    if (sel < 0 || sel >= count) return;
    char path[256]; join(ents[sel].name, path);
    if (sys_vfs_delete(path) != 0) { set_status("suppression refusee"); return; }
    set_status("supprime"); reload();
}

static void toolbar_action(int b) {
    switch (b) {
        case 0: go_up(); break;
        case 1: open_selected(); break;
        case 2: mode = 1; ilen = 0; input[0] = 0; set_status("Nom du dossier puis Entree"); break;
        case 3: mode = 2; ilen = 0; input[0] = 0; set_status("Nom du fichier puis Entree"); break;
        case 4: do_delete(); break;
        case 5: reload(); set_status("actualise"); break;
        case 6: {                                  // raccourci vers la cle USB
            dirent_t e;
            if (sys_vfs_stat("/media/usb", &e) == 0) {
                strcpy(cwd, "/media/usb"); sel = 0; reload(); set_status("cle USB");
            } else set_status("aucune cle USB montee");
            break;
        }
    }
}

// --- Disposition de la barre d'outils : dessine (hit_x<0) ou teste un clic ----
static int toolbar_layout(int hit_x, int hit_y) {
    int x = 6, y = 4, h = 22;
    for (int i = 0; i < NBTN; i++) {
        int w = (int)strlen(toolbar[i]) * 8 + 12;
        if (hit_x >= 0) {
            if (hit_x >= x && hit_x < x + w && hit_y >= y && hit_y < y + h) return i;
        } else {
            canvas_fill_rect(cv, x, y, w, h, rgb(0x3a, 0x40, 0x52));
            canvas_draw_string(cv, toolbar[i], x + 6, y + 4, rgb(0xff, 0xff, 0xff), 1);
        }
        x += w + 5;
    }
    return -1;
}

static void redraw(void) {
    canvas_fill(cv, rgb(0x24, 0x27, 0x31));
    canvas_fill_rect(cv, 0, 0, cv->width, 30, rgb(0x2c, 0x30, 0x3e));
    toolbar_layout(-1, -1);

    // Barre de chemin.
    canvas_fill_rect(cv, 0, 30, cv->width, 18, rgb(0x1b, 0x1e, 0x27));
    canvas_draw_string(cv, cwd, 6, 31, rgb(0x9a, 0xd0, 0xff), 1);

    // Liste des entrées (moitié gauche).
    int list_w = preview_len ? cv->width / 2 : (int)cv->width;
    for (int i = 0; i < count; i++) {
        int y = LIST_Y + i * ROW_H;
        if (y + ROW_H > (int)cv->height - 20) break;
        if (i == sel) canvas_fill_rect(cv, 0, y, list_w, ROW_H, rgb(0x2d, 0x6c, 0xdf));
        uint32_t icon = (ents[i].type == 1) ? rgb(0xe0, 0xc4, 0x4f) : rgb(0x9a, 0xa0, 0xb4);
        canvas_fill_rect(cv, 8, y + 4, 12, 12, icon);
        canvas_draw_string(cv, ents[i].name, 28, y + 2, rgb(0xff, 0xff, 0xff), 1);
        if (ents[i].type == 1) canvas_draw_string(cv, "<dossier>", list_w - 90, y + 2, rgb(0xc8, 0xc8, 0xc8), 1);
    }

    // Panneau d'aperçu (moitié droite) si un fichier a été ouvert.
    if (preview_len) {
        int px = cv->width / 2 + 6, py = LIST_Y, cxp = px, cyp = py;
        canvas_fill_rect(cv, cv->width / 2, 48, 1, cv->height - 68, rgb(0x3a, 0x40, 0x52));
        for (int i = 0; i < preview_len; i++) {
            char ch = preview[i];
            if (ch == '\n' || cxp > (int)cv->width - 8) { cxp = px; cyp += 14; if (cyp > (int)cv->height - 24) break; if (ch == '\n') continue; }
            if (ch >= ' ') { canvas_draw_char(cv, ch, cxp, cyp, rgb(0xcf, 0xe6, 0xc8), 1); cxp += 8; }
        }
    }

    // Barre du bas : saisie ou statut.
    if (mode) {
        canvas_fill_rect(cv, 0, cv->height - 20, cv->width, 20, rgb(0x3a, 0x2c, 0x52));
        canvas_draw_string(cv, mode == 1 ? "Dossier: " : "Fichier: ", 4, cv->height - 18, rgb(0xff, 0xff, 0xff), 1);
        canvas_draw_string(cv, input, 4 + 8 * 9, cv->height - 18, rgb(0xff, 0xff, 0x99), 1);
    } else {
        canvas_fill_rect(cv, 0, cv->height - 20, cv->width, 20, rgb(0x1b, 0x1e, 0x27));
        canvas_draw_string(cv, status, 6, cv->height - 18, rgb(0x9a, 0xa0, 0xb4), 1);
    }
    win_damage();
}

static void commit_input(void) {
    input[ilen] = 0;
    if (ilen) {
        char path[256]; join(input, path);
        if (sys_vfs_create(path, mode == 1 ? 1 : 0) != 0) set_status("creation refusee");
        else { set_status("cree"); reload(); }
    }
    mode = 0;
}

static void on_key(const event_t *e) {
    if (mode) {
        if (e->key == KEY_ENTER) commit_input();
        else if (e->key == KEY_BACKSPACE) { if (ilen > 0) input[--ilen] = 0; }
        else if (e->key == KEY_ESC) { mode = 0; set_status("annule"); }
        else if (e->ch && ilen < (int)sizeof(input) - 1) { input[ilen++] = e->ch; input[ilen] = 0; }
        return;
    }
    if (e->key == KEY_UP)        { if (sel > 0) sel--; }
    else if (e->key == KEY_DOWN) { if (sel < count - 1) sel++; }
    else if (e->key == KEY_ENTER) open_selected();
    else if (e->key == KEY_DELETE) do_delete();
    else if (e->key == KEY_BACKSPACE) go_up();
}

static void on_mouse(const event_t *e) {
    if (!(e->buttons & MOUSE_LEFT)) return;
    int cx = e->mx, cy = e->my;
    if (cy < 30) { int b = toolbar_layout(cx, cy); if (b >= 0) toolbar_action(b); return; }
    if (cy >= LIST_Y) {
        int idx = (cy - LIST_Y) / ROW_H;
        if (idx >= 0 && idx < count) {
            if (idx == sel) open_selected();      // 2e clic = ouvrir
            else sel = idx;
        }
    }
}

int main(void) {
    cv = win_create(W, H, "Explorateur");
    if (!cv) return 1;
    strcpy(cwd, "/home/user");
    dirent_t e; if (sys_vfs_stat(cwd, &e) != 0) strcpy(cwd, "/");
    set_status("");
    reload();
    redraw();

    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (ev.type == EV_KEY) { if (ev.pressed) on_key(&ev); }
        else if (ev.type == EV_MOUSE) on_mouse(&ev);
        redraw();
    }
}
