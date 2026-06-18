// =============================================================================
//  kernel/app_files.c -- Explorateur de fichiers
// =============================================================================
#include "apps.h"
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"
#include "vfs.h"
#include "users.h"

typedef struct {
    vfs_node_t *cwd;
    int  sel;
    int  mode;            // 0=normal, 1=renommer, 2=nouveau dossier
    char input[64];
    int  input_len;
    char status[96];
} files_t;

// Presse-papiers partagé entre toutes les fenêtres de l'explorateur.
static vfs_node_t *clipboard;
static bool        clip_cut;

static const char *toolbar[] = {
    "Haut", "Ouvrir", "NvDossier", "Renommer", "Suppr", "Copier", "Couper", "Coller"
};
#define NBTN (int)(sizeof(toolbar)/sizeof(toolbar[0]))

#define ROW_H 20
#define LIST_Y 50

static void status(files_t *f, const char *s) {
    strncpy(f->status, s, sizeof(f->status) - 1);
    f->status[sizeof(f->status) - 1] = 0;
}

static vfs_node_t *child_at(vfs_node_t *dir, int index) {
    int n = 0;
    for (vfs_node_t *c = dir->children; c; c = c->next) if (n++ == index) return c;
    return NULL;
}

static bool can_write_here(files_t *f) {
    char path[256]; vfs_path(f->cwd, path, sizeof(path));
    return users_can_write_path(path);
}

// Copie récursive de 'src' dans le dossier 'dst'.
static void deep_copy(vfs_node_t *src, vfs_node_t *dst) {
    vfs_node_t *n = vfs_create(dst, src->name, src->type);
    if (!n) return;
    if (src->type == VFS_FILE) {
        if (src->data && src->size) vfs_write(n, 0, src->data, src->size);
    } else {
        for (vfs_node_t *c = src->children; c; c = c->next) deep_copy(c, n);
    }
}

// --- Calcul de la disposition de la barre d'outils ---------------------------
// Si 'hit_x' >= 0, renvoie l'index du bouton touché (ou -1) ; sinon dessine.
static int toolbar_layout(canvas_t *c, int hit_x, int hit_y) {
    int x = 6, y = 4, h = 22;
    for (int i = 0; i < NBTN; i++) {
        int w = (int)strlen(toolbar[i]) * 8 + 12;
        if (hit_x >= 0) {
            if (hit_x >= x && hit_x < x + w && hit_y >= y && hit_y < y + h) return i;
        } else if (c) {
            canvas_fill_rect(c, x, y, w, h, fb_rgb(0x3a, 0x40, 0x52));
            canvas_draw_rect(c, x, y, w, h, fb_rgb(0x10,0x12,0x1a));
            canvas_draw_string(c, toolbar[i], x + 6, y + 3, fb_rgb(0xff,0xff,0xff), 1);
        }
        x += w + 5;
    }
    return -1;
}

static void files_open_selected(window_t *win, files_t *f) {
    vfs_node_t *n = child_at(f->cwd, f->sel);
    if (!n) return;
    if (n->type == VFS_DIR) { f->cwd = n; f->sel = 0; status(f, ""); }
    else app_editor_open(n);
    win->dirty = true;
}

static void files_action(window_t *win, files_t *f, int btn) {
    vfs_node_t *sel = child_at(f->cwd, f->sel);
    switch (btn) {
        case 0: if (f->cwd->parent) { f->cwd = f->cwd->parent; f->sel = 0; } break;  // Haut
        case 1: files_open_selected(win, f); break;                                  // Ouvrir
        case 2: if (!can_write_here(f)) { status(f, "permission refusee"); break; }   // Nouveau dossier
                f->mode = 2; f->input_len = 0; f->input[0] = 0; status(f, "Nom du dossier puis Entree"); break;
        case 3: if (!sel) break;                                                      // Renommer
                if (!can_write_here(f)) { status(f, "permission refusee"); break; }
                f->mode = 1; strncpy(f->input, sel->name, sizeof(f->input)-1);
                f->input_len = strlen(f->input); status(f, "Nouveau nom puis Entree"); break;
        case 4: if (!sel) break;                                                      // Supprimer
                if (!can_write_here(f)) { status(f, "permission refusee"); break; }
                vfs_delete(sel); if (f->sel > 0) f->sel--; status(f, "supprime"); break;
        case 5: clipboard = sel; clip_cut = false; status(f, sel ? "copie" : ""); break;   // Copier
        case 6: clipboard = sel; clip_cut = true;  status(f, sel ? "coupe" : ""); break;    // Couper
        case 7: if (!clipboard) break;                                                // Coller
                if (!can_write_here(f)) { status(f, "permission refusee"); break; }
                if (clip_cut) { if (vfs_move(clipboard, f->cwd)) { clipboard = NULL; status(f, "deplace"); } else status(f, "echec"); }
                else { deep_copy(clipboard, f->cwd); status(f, "colle"); }
                break;
    }
    win->dirty = true;
}

static void files_paint(window_t *win) {
    files_t *f = (files_t *)win->user;
    canvas_t *c = &win->canvas;
    canvas_fill(c, fb_rgb(0x24, 0x27, 0x31));

    // Barre d'outils.
    canvas_fill_rect(c, 0, 0, c->width, 30, fb_rgb(0x2c, 0x30, 0x3e));
    toolbar_layout(c, -1, -1);

    // Barre de chemin.
    char path[256]; vfs_path(f->cwd, path, sizeof(path));
    canvas_fill_rect(c, 0, 30, c->width, 18, fb_rgb(0x1b, 0x1e, 0x27));
    canvas_draw_string(c, path, 6, 31, fb_rgb(0x9a, 0xd0, 0xff), 1);

    // Liste des entrées.
    int i = 0;
    for (vfs_node_t *e = f->cwd->children; e; e = e->next, i++) {
        int y = LIST_Y + i * ROW_H;
        if (y + ROW_H > (int)c->height - 20) break;
        if (i == f->sel) canvas_fill_rect(c, 0, y, c->width, ROW_H, fb_rgb(0x2d, 0x6c, 0xdf));
        uint32_t icon = (e->type == VFS_DIR) ? fb_rgb(0xe0,0xc4,0x4f) : fb_rgb(0x9a,0xa0,0xb4);
        canvas_fill_rect(c, 8, y + 4, 12, 12, icon);
        canvas_draw_string(c, e->name, 28, y + 2, fb_rgb(0xff,0xff,0xff), 1);
        if (e->type == VFS_DIR) canvas_draw_string(c, "<dossier>", c->width - 90, y + 2, fb_rgb(0xc8,0xc8,0xc8), 1);
    }

    // Barre de saisie (mode renommer / nouveau dossier).
    if (f->mode) {
        canvas_fill_rect(c, 0, c->height - 20, c->width, 20, fb_rgb(0x3a,0x2c,0x52));
        canvas_draw_string(c, f->mode == 1 ? "Renommer: " : "Dossier: ", 4, c->height - 18, fb_rgb(0xff,0xff,0xff), 1);
        canvas_draw_string(c, f->input, 4 + 8 * 10, c->height - 18, fb_rgb(0xff,0xff,0x99), 1);
    } else {
        canvas_fill_rect(c, 0, c->height - 20, c->width, 20, fb_rgb(0x1b,0x1e,0x27));
        canvas_draw_string(c, f->status, 6, c->height - 18, fb_rgb(0x9a,0xa0,0xb4), 1);
    }
}

static void files_event(window_t *win, const event_t *e, int cx, int cy) {
    files_t *f = (files_t *)win->user;

    if (e->type == EV_MOUSE && (e->buttons & MOUSE_LEFT)) {
        if (cy < 30) {                       // barre d'outils
            int b = toolbar_layout(NULL, cx, cy);
            if (b >= 0) files_action(win, f, b);
            return;
        }
        if (cy >= LIST_Y) {                  // liste
            int idx = (cy - LIST_Y) / ROW_H;
            int count = vfs_count_children(f->cwd);
            if (idx < count) {
                if (idx == f->sel) files_open_selected(win, f);   // 2e clic = ouvrir
                else { f->sel = idx; win->dirty = true; }
            }
        }
        return;
    }

    if (e->type == EV_KEY && e->pressed) {
        if (f->mode) {                       // saisie de nom
            if (e->key == KEY_ENTER) {
                f->input[f->input_len] = 0;
                vfs_node_t *sel = child_at(f->cwd, f->sel);
                if (f->mode == 1 && sel) vfs_rename(sel, f->input);
                else if (f->mode == 2) vfs_create(f->cwd, f->input, VFS_DIR);
                f->mode = 0; status(f, "ok");
            } else if (e->key == KEY_BACKSPACE) {
                if (f->input_len > 0) f->input[--f->input_len] = 0;
            } else if (e->key == KEY_ESC) {
                f->mode = 0; status(f, "annule");
            } else if (e->ch && f->input_len < (int)sizeof(f->input) - 1) {
                f->input[f->input_len++] = e->ch;
            }
            win->dirty = true;
            return;
        }
        int count = vfs_count_children(f->cwd);
        if (e->key == KEY_UP)   { if (f->sel > 0) f->sel--; }
        else if (e->key == KEY_DOWN) { if (f->sel < count - 1) f->sel++; }
        else if (e->key == KEY_ENTER) files_open_selected(win, f);
        else if (e->key == KEY_DELETE) files_action(win, f, 4);
        else if (e->key == KEY_BACKSPACE) files_action(win, f, 0);
        win->dirty = true;
    }
}

void app_files_open(void) {
    window_t *win = wm_create("Explorateur de fichiers", 160, 110, 560, 420);
    if (!win) return;
    files_t *f = (files_t *)kcalloc(1, sizeof(files_t));
    const user_t *u = users_current();
    f->cwd = vfs_resolve(u ? u->home : "/");
    if (!f->cwd) f->cwd = vfs_root();
    status(f, "");
    win->user = f;
    win->on_paint = files_paint;
    win->on_event = files_event;
    win->dirty = true;
}
