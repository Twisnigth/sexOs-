// =============================================================================
//  kernel/app_terminal.c -- Terminal graphique (shell réutilisant l'esprit
//  de l'OS legacy, avec commandes liées au système de fichiers).
// =============================================================================
#include "apps.h"
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"
#include "vfs.h"
#include "users.h"
#include "pmm.h"
#include "rtc.h"
#include "io.h"

#define TCOLS 80
#define TROWS 25

typedef struct {
    char  cells[TROWS][TCOLS];
    int   cx, cy;
    char  input[256];
    int   input_len;
    vfs_node_t *cwd;
} term_t;

static term_t *T;   // terminal courant (un seul à la fois lors de l'exécution d'une commande)

// --- Sortie texte ------------------------------------------------------------
static void term_scroll(term_t *t) {
    for (int y = 1; y < TROWS; y++)
        memcpy(t->cells[y - 1], t->cells[y], TCOLS);
    memset(t->cells[TROWS - 1], ' ', TCOLS);
    t->cy = TROWS - 1;
}
static void term_newline(term_t *t) {
    t->cx = 0;
    if (++t->cy >= TROWS) term_scroll(t);
}
static void term_putc(term_t *t, char c) {
    if (c == '\n') { term_newline(t); return; }
    if (c == '\b') { if (t->cx > 0) { t->cx--; t->cells[t->cy][t->cx] = ' '; } return; }
    t->cells[t->cy][t->cx] = c;
    if (++t->cx >= TCOLS) term_newline(t);
}
static void term_print(term_t *t, const char *s) { for (; *s; s++) term_putc(t, *s); }

static void term_prompt(term_t *t) {
    char path[256];
    vfs_path(t->cwd, path, sizeof(path));
    const user_t *u = users_current();
    term_print(t, u ? u->name : "?");
    term_print(t, ":");
    term_print(t, path);
    term_print(t, "$ ");
}

// --- Commandes ---------------------------------------------------------------
static void cmd_help(term_t *t) {
    term_print(t,
        "Commandes :\n"
        " help            cette aide\n"
        " clear           efface l'ecran\n"
        " echo <texte>    affiche du texte\n"
        " ls [chemin]     liste un dossier\n"
        " cd <dossier>    change de dossier\n"
        " pwd             dossier courant\n"
        " cat <fichier>   affiche un fichier\n"
        " mkdir <nom>     cree un dossier\n"
        " touch <nom>     cree un fichier\n"
        " rm <nom>        supprime\n"
        " whoami          utilisateur courant\n"
        " date            date et heure\n"
        " sysinfo         infos systeme\n"
        " about           a propos + mascotte\n"
        " reboot          redemarre\n");
}
static void cmd_about(term_t *t) {
    term_print(t,
        "\n  MonOS version 2.0\n"
        "  Systeme x86_64, demarrage UEFI/BIOS via Limine.\n"
        "  Interface graphique, multi-fenetres, comptes utilisateurs.\n\n"
        "  Mascotte :\n    D\n    |\n    |\n    8\n\n");
}
static void cmd_sysinfo(term_t *t) {
    char num[24];
    term_print(t, "Memoire totale : ");
    utoa(pmm_total_bytes() / (1024*1024), num, 10); term_print(t, num); term_print(t, " Mio\n");
    term_print(t, "Memoire utilisee : ");
    utoa(pmm_used_bytes() / (1024*1024), num, 10); term_print(t, num); term_print(t, " Mio\n");
    term_print(t, "Utilisateur : ");
    const user_t *u = users_current();
    term_print(t, u ? u->name : "?");
    term_print(t, users_is_admin() ? " (admin)\n" : " (standard)\n");
}

// Renvoie le nœud désigné par 'arg' relatif à cwd ('arg' peut être absolu).
static vfs_node_t *resolve_arg(term_t *t, const char *arg) {
    if (!arg || !arg[0]) return t->cwd;
    if (arg[0] == '/') return vfs_resolve(arg);
    if (strcmp(arg, "..") == 0) return t->cwd->parent ? t->cwd->parent : t->cwd;
    if (strcmp(arg, ".") == 0) return t->cwd;
    return vfs_lookup(t->cwd, arg);
}

static void cmd_ls(term_t *t, const char *arg) {
    vfs_node_t *d = resolve_arg(t, arg);
    if (!d) { term_print(t, "ls: introuvable\n"); return; }
    if (d->type != VFS_DIR) { term_print(t, d->name); term_print(t, "\n"); return; }
    for (vfs_node_t *c = d->children; c; c = c->next) {
        term_print(t, c->name);
        if (c->type == VFS_DIR) term_print(t, "/");
        term_print(t, "\n");
    }
}
static void cmd_cd(term_t *t, const char *arg) {
    vfs_node_t *d = resolve_arg(t, arg);
    if (!d || d->type != VFS_DIR) { term_print(t, "cd: dossier invalide\n"); return; }
    t->cwd = d;
}
static void cmd_cat(term_t *t, const char *arg) {
    vfs_node_t *f = resolve_arg(t, arg);
    if (!f || f->type != VFS_FILE) { term_print(t, "cat: fichier introuvable\n"); return; }
    for (size_t i = 0; i < f->size; i++) term_putc(t, (char)f->data[i]);
    term_putc(t, '\n');
}
static void cmd_mkdir(term_t *t, const char *arg) {
    char path[256]; vfs_path(t->cwd, path, sizeof(path));
    if (!users_can_write_path(path)) { term_print(t, "permission refusee\n"); return; }
    if (!vfs_create(t->cwd, arg, VFS_DIR)) term_print(t, "mkdir: echec\n");
}
static void cmd_touch(term_t *t, const char *arg) {
    char path[256]; vfs_path(t->cwd, path, sizeof(path));
    if (!users_can_write_path(path)) { term_print(t, "permission refusee\n"); return; }
    if (!vfs_create(t->cwd, arg, VFS_FILE)) term_print(t, "touch: echec\n");
}
static void cmd_rm(term_t *t, const char *arg) {
    char path[256]; vfs_path(t->cwd, path, sizeof(path));
    if (!users_can_write_path(path)) { term_print(t, "permission refusee\n"); return; }
    vfs_node_t *n = vfs_lookup(t->cwd, arg);
    if (!n || !vfs_delete(n)) term_print(t, "rm: echec\n");
}

static void term_run(term_t *t, char *line) {
    // Découpe en commande + argument (reste de la ligne).
    while (*line == ' ') line++;
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }
    char *cmd = line;

    if (cmd[0] == 0) return;
    else if (strcmp(cmd, "help") == 0) cmd_help(t);
    else if (strcmp(cmd, "clear") == 0) { for (int y=0;y<TROWS;y++) memset(t->cells[y],' ',TCOLS); t->cx=t->cy=0; }
    else if (strcmp(cmd, "echo") == 0) { term_print(t, arg); term_putc(t, '\n'); }
    else if (strcmp(cmd, "ls") == 0) cmd_ls(t, arg);
    else if (strcmp(cmd, "cd") == 0) cmd_cd(t, arg);
    else if (strcmp(cmd, "pwd") == 0) { char p[256]; vfs_path(t->cwd,p,sizeof(p)); term_print(t,p); term_putc(t,'\n'); }
    else if (strcmp(cmd, "cat") == 0) cmd_cat(t, arg);
    else if (strcmp(cmd, "mkdir") == 0) cmd_mkdir(t, arg);
    else if (strcmp(cmd, "touch") == 0) cmd_touch(t, arg);
    else if (strcmp(cmd, "rm") == 0) cmd_rm(t, arg);
    else if (strcmp(cmd, "whoami") == 0) { const user_t *u=users_current(); term_print(t, u?u->name:"?"); term_print(t, users_is_admin()?" (admin)\n":" (standard)\n"); }
    else if (strcmp(cmd, "date") == 0) { rtc_time_t tm; rtc_now(&tm); char b[24]; rtc_format(&tm,b); term_print(t,b); term_putc(t,'\n'); }
    else if (strcmp(cmd, "sysinfo") == 0) cmd_sysinfo(t);
    else if (strcmp(cmd, "about") == 0) cmd_about(t);
    else if (strcmp(cmd, "reboot") == 0) { term_print(t, "redemarrage...\n"); outb(0x64, 0xFE); }
    else { term_print(t, cmd); term_print(t, ": commande inconnue\n"); }
}

// --- Rappels fenêtre ---------------------------------------------------------
static void term_paint(window_t *win) {
    term_t *t = (term_t *)win->user;
    canvas_t *c = &win->canvas;
    uint32_t bg = fb_rgb(0x0c, 0x10, 0x14);
    uint32_t fg = fb_rgb(0xd0, 0xf0, 0xd0);
    canvas_fill(c, bg);
    for (int y = 0; y < TROWS; y++)
        for (int x = 0; x < TCOLS; x++) {
            char ch = t->cells[y][x];
            if (ch && ch != ' ') canvas_draw_char(c, ch, x * 8, y * 16, fg, 1);
        }
    // Curseur de saisie.
    canvas_fill_rect(c, t->cx * 8, t->cy * 16 + 14, 8, 2, fb_rgb(0x6e,0xe7,0x9a));
}

static void term_event(window_t *win, const event_t *e, int cx, int cy) {
    (void)cx; (void)cy;
    if (e->type != EV_KEY || !e->pressed) return;
    term_t *t = (term_t *)win->user;
    T = t;
    if (e->key == KEY_ENTER) {
        t->input[t->input_len] = 0;
        term_putc(t, '\n');
        term_run(t, t->input);
        t->input_len = 0;
        term_prompt(t);
    } else if (e->key == KEY_BACKSPACE) {
        if (t->input_len > 0) { t->input_len--; term_putc(t, '\b'); }
    } else if (e->ch) {
        if (t->input_len < (int)sizeof(t->input) - 1) {
            t->input[t->input_len++] = e->ch;
            term_putc(t, e->ch);
        }
    }
    win->dirty = true;
}

void app_terminal_open(void) {
    window_t *win = wm_create("Terminal", 120, 90, TCOLS * 8, TROWS * 16 + TITLEBAR_H);
    if (!win) return;
    win->resizable = false;
    term_t *t = (term_t *)kcalloc(1, sizeof(term_t));
    for (int y = 0; y < TROWS; y++) memset(t->cells[y], ' ', TCOLS);
    const user_t *u = users_current();
    t->cwd = vfs_resolve(u ? u->home : "/");
    if (!t->cwd) t->cwd = vfs_root();
    win->user = t;
    win->on_paint = term_paint;
    win->on_event = term_event;
    term_print(t, "MonOS Terminal v2 -- tapez 'help'.\n");
    term_prompt(t);
    win->dirty = true;
}
