// =============================================================================
//  kernel/app_editor.c -- Éditeur de texte simple
// =============================================================================
#include "apps.h"
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"
#include "vfs.h"
#include "users.h"

#define EDIT_MAX 8192

typedef struct {
    vfs_node_t *file;
    char   buf[EDIT_MAX];
    int    len;
    int    cursor;
    int    scroll;        // première ligne affichée
    bool   readonly;
    char   status[64];
} editor_t;

static void ed_status(editor_t *ed, const char *s) {
    strncpy(ed->status, s, sizeof(ed->status) - 1);
    ed->status[sizeof(ed->status) - 1] = 0;
}

static void ed_save(window_t *win, editor_t *ed) {
    char path[256];
    vfs_path(ed->file, path, sizeof(path));
    if (!users_can_write_path(path)) { ed_status(ed, "Lecture seule (permission refusee)"); return; }
    if (vfs_write(ed->file, 0, ed->buf, ed->len) >= 0) {
        ed->file->size = ed->len;
        ed_status(ed, "Enregistre.");
    } else ed_status(ed, "Echec de l'enregistrement.");
    (void)win;
}

static void ed_paint(window_t *win) {
    editor_t *ed = (editor_t *)win->user;
    canvas_t *c = &win->canvas;
    uint32_t bg = fb_rgb(0x1b, 0x1e, 0x27);
    uint32_t fg = fb_rgb(0xe6, 0xe6, 0xe6);
    uint32_t cur = fb_rgb(0xff, 0xcc, 0x33);
    canvas_fill(c, bg);

    int cols = c->width / 8;
    int rows = (c->height - 18) / 16;     // on réserve une ligne de statut

    // Calcule la position (ligne/colonne) du curseur.
    int line = 0, col = 0, cl = 0, cc = 0;
    for (int i = 0; i < ed->len; i++) {
        if (i == ed->cursor) { cl = line; cc = col; }
        if (ed->buf[i] == '\n') { line++; col = 0; } else col++;
    }
    if (ed->cursor >= ed->len) { cl = line; cc = col; }

    if (cl < ed->scroll) ed->scroll = cl;
    if (cl >= ed->scroll + rows) ed->scroll = cl - rows + 1;

    // Affiche le texte ligne par ligne.
    int x = 0, y = 0, curline = 0;
    for (int i = 0; i <= ed->len; i++) {
        char ch = (i < ed->len) ? ed->buf[i] : 0;
        if (curline >= ed->scroll && curline < ed->scroll + rows) {
            int sy = (curline - ed->scroll) * 16;
            if (i == ed->cursor) canvas_fill_rect(c, x, sy, 8, 16, cur);
            if (ch && ch != '\n' && x < (cols * 8))
                canvas_draw_char(c, ch, x, sy, (i == ed->cursor) ? bg : fg, 1);
        }
        if (ch == '\n') { curline++; x = 0; }
        else x += 8;
    }

    // Ligne de statut.
    canvas_fill_rect(c, 0, c->height - 18, c->width, 18, fb_rgb(0x2d, 0x6c, 0xdf));
    canvas_draw_string(c, ed->readonly ? "[lecture seule]  Ctrl+S: enregistrer"
                                        : "Ctrl+S: enregistrer", 4, c->height - 16,
                       fb_rgb(0xff,0xff,0xff), 1);
    canvas_draw_string(c, ed->status, c->width - 8 * (int)strlen(ed->status) - 6,
                       c->height - 16, fb_rgb(0xff,0xff,0xff), 1);
    (void)y;
}

static void ed_event(window_t *win, const event_t *e, int cx, int cy) {
    (void)cx; (void)cy;
    if (e->type != EV_KEY || !e->pressed) return;
    editor_t *ed = (editor_t *)win->user;

    if ((e->mods & MOD_CTRL) && (e->ch == 's' || e->ch == 'S')) { ed_save(win, ed); win->dirty = true; return; }

    if (e->key == KEY_LEFT)  { if (ed->cursor > 0) ed->cursor--; }
    else if (e->key == KEY_RIGHT) { if (ed->cursor < ed->len) ed->cursor++; }
    else if (e->key == KEY_UP || e->key == KEY_DOWN) {
        // Déplacement vertical : on recalcule colonne et on se déplace d'une ligne.
        int col = 0, i = ed->cursor;
        while (i > 0 && ed->buf[i-1] != '\n') { i--; col++; }
        if (e->key == KEY_UP) {
            if (i > 0) { int j = i - 1; int ls = j; while (ls > 0 && ed->buf[ls-1] != '\n') ls--;
                         int k = ls; int cc = 0; while (k < j && cc < col) { k++; cc++; } ed->cursor = k; }
        } else {
            int j = ed->cursor; while (j < ed->len && ed->buf[j] != '\n') j++;
            if (j < ed->len) { int k = j + 1; int cc = 0; while (k < ed->len && ed->buf[k] != '\n' && cc < col) { k++; cc++; } ed->cursor = k; }
        }
    }
    else if (e->key == KEY_BACKSPACE) {
        if (ed->cursor > 0 && !ed->readonly) {
            memmove(&ed->buf[ed->cursor-1], &ed->buf[ed->cursor], ed->len - ed->cursor);
            ed->cursor--; ed->len--;
            ed_status(ed, "modifie");
        }
    }
    else if (e->key == KEY_ENTER) {
        if (ed->len < EDIT_MAX - 1 && !ed->readonly) {
            memmove(&ed->buf[ed->cursor+1], &ed->buf[ed->cursor], ed->len - ed->cursor);
            ed->buf[ed->cursor++] = '\n'; ed->len++;
        }
    }
    else if (e->ch) {
        if (ed->len < EDIT_MAX - 1 && !ed->readonly) {
            memmove(&ed->buf[ed->cursor+1], &ed->buf[ed->cursor], ed->len - ed->cursor);
            ed->buf[ed->cursor++] = e->ch; ed->len++;
            ed_status(ed, "modifie");
        }
    }
    win->dirty = true;
}

void app_editor_open(vfs_node_t *file) {
    if (!file || file->type != VFS_FILE) return;
    char title[96];
    strcpy(title, "Editeur - ");
    strcat(title, file->name);
    window_t *win = wm_create(title, 200, 140, 560, 380);
    if (!win) return;
    editor_t *ed = (editor_t *)kcalloc(1, sizeof(editor_t));
    ed->file = file;
    ed->len = file->size < EDIT_MAX ? (int)file->size : EDIT_MAX - 1;
    if (file->data) memcpy(ed->buf, file->data, ed->len);
    char path[256]; vfs_path(file, path, sizeof(path));
    ed->readonly = !users_can_write_path(path);
    ed_status(ed, ed->readonly ? "lecture seule" : "");
    win->user = ed;
    win->on_paint = ed_paint;
    win->on_event = ed_event;
    win->dirty = true;
}
