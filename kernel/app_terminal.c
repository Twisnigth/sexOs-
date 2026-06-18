// =============================================================================
//  kernel/app_terminal.c -- Terminal graphique + shell
// -----------------------------------------------------------------------------
//  Éditeur de ligne complet (curseur déplaçable, insertion/suppression au
//  milieu) et auto-complétion par Tab (commandes intégrées + chemins du système
//  de fichiers), à la manière de bash.
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
#define INPUT_MAX 256

typedef struct {
    char  cells[TROWS][TCOLS];
    int   cx, cy;
    // --- éditeur de ligne ---
    char  input[INPUT_MAX];
    int   input_len;       // longueur de la saisie
    int   input_pos;       // position du curseur dans la saisie
    int   prompt_row;      // ligne où commence la saisie
    int   prompt_col;      // colonne où commence la saisie
    int   prev_len;        // longueur précédemment affichée (pour effacer)
    vfs_node_t *cwd;
} term_t;

// Liste des commandes intégrées (triée, sert aussi à la complétion).
static const char *BUILTINS[] = {
    "about","cat","cd","clear","date","echo","help","ls",
    "mkdir","pwd","reboot","rm","sysinfo","touch","whoami"
};
#define NBUILTINS (int)(sizeof(BUILTINS)/sizeof(BUILTINS[0]))

// --- Sortie texte (utilisée pour l'affichage des commandes) ------------------
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

// =============================================================================
//  Éditeur de ligne
// =============================================================================

// Écrit un caractère à une position linéaire (row*TCOLS+col), avec garde.
static void put_lin(term_t *t, int lin, char ch) {
    if (lin < 0 || lin >= TROWS * TCOLS) return;
    t->cells[lin / TCOLS][lin % TCOLS] = ch;
}

// Redessine la saisie depuis l'invite et replace le curseur.
static void redraw_input(term_t *t) {
    int start = t->prompt_row * TCOLS + t->prompt_col;
    int n = (t->prev_len > t->input_len) ? t->prev_len : t->input_len;
    for (int i = 0; i < n; i++)
        put_lin(t, start + i, (i < t->input_len) ? t->input[i] : ' ');
    t->prev_len = t->input_len;
    int cur = start + t->input_pos;
    t->cx = cur % TCOLS;
    t->cy = cur / TCOLS;
    if (t->cy >= TROWS) { t->cy = TROWS - 1; t->cx = TCOLS - 1; }
}

// Démarre une nouvelle saisie après l'affichage de l'invite.
static void begin_input(term_t *t) {
    t->prompt_row = t->cy;
    t->prompt_col = t->cx;
    t->prev_len = 0;
}

static void term_prompt(term_t *t) {
    if (t->cy >= TROWS - 1) term_scroll(t);   // garde une ligne pour la saisie
    char path[256];
    vfs_path(t->cwd, path, sizeof(path));
    const user_t *u = users_current();
    term_print(t, u ? u->name : "?");
    term_print(t, ":");
    term_print(t, path);
    term_print(t, "$ ");
    begin_input(t);
    redraw_input(t);
}

static void input_insert(term_t *t, char c) {
    if (t->input_len >= INPUT_MAX - 1) return;
    for (int i = t->input_len; i > t->input_pos; i--) t->input[i] = t->input[i - 1];
    t->input[t->input_pos] = c;
    t->input_len++;
    t->input_pos++;
}
static void input_insert_str(term_t *t, const char *s) {
    for (; *s; s++) input_insert(t, *s);
}
static void input_backspace(term_t *t) {
    if (t->input_pos == 0) return;
    for (int i = t->input_pos - 1; i < t->input_len - 1; i++) t->input[i] = t->input[i + 1];
    t->input_len--;
    t->input_pos--;
}
static void input_delete(term_t *t) {
    if (t->input_pos >= t->input_len) return;
    for (int i = t->input_pos; i < t->input_len - 1; i++) t->input[i] = t->input[i + 1];
    t->input_len--;
}

// =============================================================================
//  Auto-complétion
// =============================================================================

// Résout le nœud dossier désigné par un préfixe (relatif à cwd ou absolu).
static vfs_node_t *resolve_dir_prefix(term_t *t, const char *dirpart) {
    vfs_node_t *node = (dirpart[0] == '/') ? vfs_root() : t->cwd;
    char comp[VFS_NAME_MAX];
    int i = 0;
    while (dirpart[i]) {
        int j = 0;
        while (dirpart[i] && dirpart[i] != '/' && j < VFS_NAME_MAX - 1) comp[j++] = dirpart[i++];
        comp[j] = 0;
        while (dirpart[i] == '/') i++;
        if (j == 0) continue;
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) { if (node->parent) node = node->parent; continue; }
        node = vfs_lookup(node, comp);
        if (!node || node->type != VFS_DIR) return NULL;
    }
    return node;
}

// Plus long préfixe commun d'un ensemble de chaînes.
static void common_prefix(char out[64][VFS_NAME_MAX], int n, char *res) {
    if (n == 0) { res[0] = 0; return; }
    strncpy(res, out[0], VFS_NAME_MAX - 1);
    res[VFS_NAME_MAX - 1] = 0;
    for (int i = 1; i < n; i++) {
        int k = 0;
        while (res[k] && out[i][k] && res[k] == out[i][k]) k++;
        res[k] = 0;
    }
}

static void term_complete(term_t *t) {
    // Token courant (sous le curseur).
    int s = t->input_pos;
    while (s > 0 && t->input[s - 1] != ' ') s--;
    int i = 0; while (i < s && t->input[i] == ' ') i++;
    bool first = (i == s);
    int tlen = t->input_pos - s;
    char token[INPUT_MAX];
    memcpy(token, t->input + s, tlen); token[tlen] = 0;

    // Base à compléter + dossier de recherche (pour les chemins).
    const char *base;
    vfs_node_t *dir = NULL;
    char dirpart[INPUT_MAX];
    if (first) {
        base = token;
    } else {
        int sl = -1;
        for (int k = 0; k < tlen; k++) if (token[k] == '/') sl = k;
        if (sl < 0) { dirpart[0] = 0; base = token; }
        else { memcpy(dirpart, token, sl + 1); dirpart[sl + 1] = 0; base = token + sl + 1; }
        dir = resolve_dir_prefix(t, dirpart);
        if (!dir) return;
    }
    int blen = strlen(base);

    // Collecte des candidats.
    char names[64][VFS_NAME_MAX];
    bool is_dir[64];
    int ncand = 0;
    if (first) {
        for (int k = 0; k < NBUILTINS && ncand < 64; k++)
            if (strncmp(BUILTINS[k], base, blen) == 0) {
                strncpy(names[ncand], BUILTINS[k], VFS_NAME_MAX - 1);
                is_dir[ncand] = false; ncand++;
            }
    } else {
        for (vfs_node_t *c = dir->children; c && ncand < 64; c = c->next)
            if (strncmp(c->name, base, blen) == 0) {
                strncpy(names[ncand], c->name, VFS_NAME_MAX - 1);
                is_dir[ncand] = (c->type == VFS_DIR); ncand++;
            }
    }

    if (ncand == 0) return;                       // rien : aucun bip visuel

    if (ncand == 1) {
        input_insert_str(t, names[0] + blen);     // complète le reste
        input_insert(t, first ? ' ' : (is_dir[0] ? '/' : ' '));
        redraw_input(t);
        return;
    }

    // Plusieurs candidats : étend au plus long préfixe commun.
    char lcp[VFS_NAME_MAX];
    common_prefix(names, ncand, lcp);
    if ((int)strlen(lcp) > blen) input_insert_str(t, lcp + blen);

    // Affiche la liste des possibilités sous l'invite, puis redessine.
    int saved_len = t->input_len, saved_pos = t->input_pos;
    t->input_pos = t->input_len;
    redraw_input(t);                              // place le curseur en fin de ligne
    term_newline(t);
    for (int k = 0; k < ncand; k++) {
        term_print(t, names[k]);
        if (is_dir[k]) term_putc(t, '/');
        term_putc(t, ' ');
    }
    term_newline(t);
    term_prompt(t);                               // nouvelle invite
    // Restaure la saisie en cours sous la nouvelle invite (input inchangé).
    t->input_len = saved_len;
    t->input_pos = saved_pos;
    redraw_input(t);
}

// =============================================================================
//  Commandes
// =============================================================================
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
        " reboot          redemarre\n"
        "(Tab : auto-completion des commandes et des chemins)\n");
}
static void cmd_about(term_t *t) {
    term_print(t,
        "\n  MonOS version 2.0\n"
        "  Systeme x86_64, demarrage UEFI/BIOS via Limine.\n\n"
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

// =============================================================================
//  Rappels fenêtre
// =============================================================================
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
    canvas_fill_rect(c, t->cx * 8, t->cy * 16 + 14, 8, 2, fb_rgb(0x6e,0xe7,0x9a));
}

static void term_event(window_t *win, const event_t *e, int cx, int cy) {
    (void)cx; (void)cy;
    if (e->type != EV_KEY || !e->pressed) return;
    term_t *t = (term_t *)win->user;

    switch (e->key) {
        case KEY_ENTER: {
            // Place le curseur en fin de saisie puis passe à la ligne.
            t->input_pos = t->input_len;
            redraw_input(t);
            int endlin = t->prompt_row * TCOLS + t->prompt_col + t->input_len;
            t->cx = endlin % TCOLS; t->cy = endlin / TCOLS;
            if (t->cy >= TROWS) t->cy = TROWS - 1;
            term_newline(t);
            t->input[t->input_len] = 0;
            char line[INPUT_MAX]; strcpy(line, t->input);
            t->input_len = t->input_pos = 0;
            term_run(t, line);
            term_prompt(t);
            break;
        }
        case KEY_BACKSPACE: input_backspace(t); redraw_input(t); break;
        case KEY_DELETE:    input_delete(t);    redraw_input(t); break;
        case KEY_LEFT:  if (t->input_pos > 0) t->input_pos--; redraw_input(t); break;
        case KEY_RIGHT: if (t->input_pos < t->input_len) t->input_pos++; redraw_input(t); break;
        case KEY_HOME:  t->input_pos = 0; redraw_input(t); break;
        case KEY_END:   t->input_pos = t->input_len; redraw_input(t); break;
        case KEY_TAB:   term_complete(t); break;
        default:
            if (e->ch) { input_insert(t, e->ch); redraw_input(t); }
            break;
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
    term_print(t, "MonOS Terminal v2 -- 'help', Tab pour completer.\n");
    term_prompt(t);
    win->dirty = true;
}
