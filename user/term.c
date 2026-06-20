// =============================================================================
//  user/term.c -- TERMINAL complet en PROCESSUS ring 3 séparé (client compositeur)
// -----------------------------------------------------------------------------
//  N'utilise QUE des appels système : fenêtre via le compositeur (libwin),
//  système de fichiers / comptes / infos via syscalls. Aucun pointeur noyau.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"
#include "ascii_art.h"      // banniere « sexOs »
#include "phallus_art.h"    // art (braille decode) pour la commande « Phallus »

void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, unsigned long);
char *strcat(char *, const char *);
void utoa(unsigned long, char *);

#define COLS 80
#define ROWS 24

static char cells[ROWS][COLS];
static int  cx, cy;
static char input[256];
static int  ilen;
static char cwd[256];
static canvas_t *cv;

// --- Historique des commandes (fleche haut/bas) ------------------------------
#define HISTN 32
static char history[HISTN][256];
static int  hist_count;          // nombre de commandes memorisees
static int  hist_view;           // position de navigation (== hist_count : ligne neuve)
static int  in_col0, in_row0;    // colonne/ligne de depart de la saisie (apres l'invite)

// --- Editeur « nano » --------------------------------------------------------
#define ED_MAXLINES 256
#define ED_MAXCOL   256
static char ed_lines[ED_MAXLINES][ED_MAXCOL];
static int  ed_nlines, ed_cr, ed_cc, ed_top, ed_left, ed_dirty;
static char ed_path[256];

typedef struct { uint8_t second, minute, hour, day, month; uint16_t year; } rtct_t;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

static void scroll_up(void) {
    for (int y = 1; y < ROWS; y++) memcpy(cells[y-1], cells[y], COLS);
    memset(cells[ROWS-1], ' ', COLS);
    cy = ROWS - 1;
}
static void tputc(char ch) {
    if (ch == '\n') { cx = 0; if (++cy >= ROWS) scroll_up(); return; }
    if (cx >= COLS) { cx = 0; if (++cy >= ROWS) scroll_up(); }
    cells[cy][cx++] = ch;
}
static void tprint(const char *s) { while (*s) tputc(*s++); }

static void redraw(void) {
    canvas_fill(cv, rgb(0x0c, 0x10, 0x14));
    uint32_t fg = rgb(0xd0, 0xf0, 0xd0);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            char ch = cells[y][x];
            if (ch && ch != ' ') canvas_draw_char(cv, ch, x*8, y*16, fg, 1);
        }
    canvas_fill_rect(cv, cx*8, cy*16 + 14, 8, 2, rgb(0x6e, 0xe7, 0x9a));   // curseur
    win_damage();
}

static void p2(int v) { char b[3]; b[0] = (char)('0'+(v/10)%10); b[1] = (char)('0'+v%10); b[2] = 0; tprint(b); }

static void prompt(void) {
    userinfo_t u; sys_whoami(&u);
    tprint(u.name[0] ? u.name : "?"); tprint(":"); tprint(cwd); tprint("$ ");
}

// Construit un chemin absolu depuis cwd + argument.
static void build_abs(const char *arg, char *out) {
    if (arg[0] == '/') { strcpy(out, arg); return; }
    strcpy(out, cwd);
    if (strcmp(cwd, "/") != 0) strcat(out, "/");
    strcat(out, arg);
}

// Affiche l'invite et mémorise où commence la saisie (pour réécrire la ligne
// lors de la navigation dans l'historique).
static void show_prompt(void) {
    prompt();
    in_col0 = cx; in_row0 = cy;
    hist_view = hist_count;
}

// Remplace la ligne de saisie courante par 's' (rappel d'historique).
static void set_input(const char *s) {
    for (int x = in_col0; x < COLS; x++) cells[in_row0][x] = ' ';   // efface l'ancienne
    cx = in_col0; cy = in_row0; ilen = 0;
    for (const char *p = s; *p && ilen < 255; p++) { input[ilen++] = *p; tputc(*p); }
    input[ilen] = 0;
}

// Mémorise une commande dans l'historique (sans doublon consécutif).
static void hist_push(const char *s) {
    if (hist_count > 0 && strcmp(history[hist_count-1], s) == 0) return;
    if (hist_count < HISTN) strcpy(history[hist_count++], s);
    else { for (int i = 1; i < HISTN; i++) strcpy(history[i-1], history[i]); strcpy(history[HISTN-1], s); }
}

static void cmd_ls(const char *arg) {
    char path[256];
    if (arg[0]) build_abs(arg, path); else strcpy(path, cwd);
    dirent_t e; int i = 0;
    while (sys_vfs_list(path, i++, &e) == 1) {
        tprint(e.name); if (e.type == 1) tprint("/"); tprint("\n");
    }
}
static void cmd_cd(const char *arg) {
    if (!arg[0] || strcmp(arg, "/") == 0) { strcpy(cwd, "/"); return; }
    if (strcmp(arg, "..") == 0) {
        int l = strlen(cwd);
        while (l > 1 && cwd[l-1] != '/') l--;
        if (l > 1) l--;
        cwd[l ? l : 1] = 0; if (!l) { cwd[0] = '/'; cwd[1] = 0; }
        return;
    }
    char path[256]; build_abs(arg, path);
    dirent_t e;
    if (sys_vfs_stat(path, &e) == 0 && e.type == 1) strcpy(cwd, path);
    else tprint("cd: dossier introuvable\n");
}
static void cmd_cat(const char *arg) {
    if (!arg[0]) { tprint("usage: cat <fichier>\n"); return; }
    char path[256]; build_abs(arg, path);
    static char buf[4096];
    vfs_io_t io = { path, 0, buf, sizeof(buf) - 1 };
    long n = sys_vfs_read(&io);
    if (n < 0) { tprint("cat: fichier introuvable\n"); return; }
    buf[n] = 0; tprint(buf);
    if (n > 0 && buf[n-1] != '\n') tprint("\n");
}
static void cmd_make(const char *arg, int dir) {
    if (!arg[0]) { tprint("usage: <nom>\n"); return; }
    char path[256]; build_abs(arg, path);
    if (sys_vfs_create(path, dir) != 0) tprint("echec (permission refusee ?)\n");
}
static void cmd_rm(const char *arg) {
    if (!arg[0]) { tprint("usage: rm <nom>\n"); return; }
    char path[256]; build_abs(arg, path);
    if (sys_vfs_delete(path) != 0) tprint("rm: echec (permission refusee ?)\n");
}
static void cmd_date(void) {
    rtct_t t; sys_rtc(&t);
    char y[8]; utoa(t.year, y);
    tprint(y); tprint("-"); p2(t.month); tprint("-"); p2(t.day);
    tprint(" "); p2(t.hour); tprint(":"); p2(t.minute); tprint(":"); p2(t.second); tprint("\n");
}
static void cmd_sysinfo(void) {
    sysinfo_t s; sys_sysinfo(&s); char n[24];
    tprint("sexOs v2 x86_64 (bureau multi-processus, ring 3)\n");
    utoa(s.mem_used_mb, n); tprint("memoire : "); tprint(n); tprint(" / ");
    utoa(s.mem_total_mb, n); tprint(n); tprint(" Mio\n");
    utoa(s.uptime_s, n); tprint("uptime  : "); tprint(n); tprint(" s\n");
    utoa(s.pci_count, n); tprint("PCI     : "); tprint(n); tprint(" peripheriques\n");
}
static void cmd_help(void) {
    tprint("commandes : help clear echo ls cd pwd cat mkdir touch rm nano\n");
    tprint("            whoami date sysinfo Phallus\n");
    tprint("    ssh   : hostkey pubkey pubkey-add ssh-keygen\n");
    tprint("  fleche haut/bas : historique   ^C copier la ligne   ^V coller\n");
}

// --- Gestion des cles SSH (memes fichiers que le serveur sshd) ----------------
static void authk_path(char *out) {
    userinfo_t u; sys_whoami(&u);
    strcpy(out, u.home[0] ? u.home : "/home/user");
    strcat(out, "/.ssh/authorized_keys");
}
// hostkey : cle publique d'hote + empreinte (pour verifier le serveur).
static void cmd_hostkey(void) {
    static char b[256]; sys_ssh_hostkey(b, sizeof b); tprint(b);
}
// pubkey : liste les cles autorisees de l'utilisateur courant.
static void cmd_pubkey(void) {
    char path[256]; authk_path(path);
    static char buf[4096];
    vfs_io_t io = { path, 0, buf, sizeof(buf) - 1 };
    long n = sys_vfs_read(&io);
    if (n <= 0) { tprint("aucune cle autorisee\n"); return; }
    buf[n] = 0; tprint(buf);
    if (buf[n-1] != '\n') tprint("\n");
}
// pubkey-add : enregistre une cle publique "ssh-ed25519 <base64> [commentaire]".
static void cmd_pubkey_add(const char *arg) {
    if (strncmp(arg, "ssh-ed25519 ", 12) != 0) {
        tprint("usage: pubkey-add ssh-ed25519 <base64> [commentaire]\n"); return;
    }
    const char *b = arg + 12; while (*b == ' ') b++;
    if (!*b) { tprint("cle invalide\n"); return; }
    userinfo_t u; sys_whoami(&u);
    char dir[256]; strcpy(dir, u.home[0] ? u.home : "/home/user"); strcat(dir, "/.ssh");
    dirent_t e;
    if (sys_vfs_stat(dir, &e) != 0) sys_vfs_create(dir, 1);     // cree .ssh au besoin
    char path[256]; strcpy(path, dir); strcat(path, "/authorized_keys");
    if (sys_vfs_stat(path, &e) != 0) sys_vfs_create(path, 0);
    long off = (sys_vfs_stat(path, &e) == 0) ? (long)e.size : 0;
    char line[300]; int lo = 0;
    for (const char *q = arg; *q && lo < 298; q++) line[lo++] = *q;
    line[lo++] = '\n'; line[lo] = 0;
    vfs_io_t io = { path, (uint64_t)off, line, (uint64_t)lo };
    long w = sys_vfs_write(&io);
    tprint(w > 0 ? "cle ajoutee\n" : "echec (permission ?)\n");
}
// ssh-keygen : genere une paire, ajoute la publique, imprime la cle privee.
static void cmd_keygen(void) {
    static char b[1024]; sys_ssh_keygen(b, sizeof b); tprint(b);
}

// =============================================================================
//  EDITEUR « nano » : edition plein-ecran d'un fichier (ex: authorized_keys)
// =============================================================================
static int ed_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ed_load(const char *path) {
    ed_nlines = 0; ed_cr = ed_cc = ed_top = ed_left = ed_dirty = 0;
    static char buf[16384];
    vfs_io_t io = { path, 0, buf, sizeof(buf) - 1 };
    long n = sys_vfs_read(&io); if (n < 0) n = 0;       // fichier neuf
    buf[n] = 0;
    int li = 0, c = 0;
    for (long i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') { ed_lines[li][c] = 0; if (li < ED_MAXLINES-1) li++; c = 0; }
        else if (ch == '\r') continue;
        else if (c < ED_MAXCOL-1) ed_lines[li][c++] = ch;
    }
    ed_lines[li][c] = 0;
    ed_nlines = li + 1;
}

static int ed_save(void) {
    static char buf[16384]; int o = 0;
    for (int i = 0; i < ed_nlines; i++) {
        for (int c = 0; ed_lines[i][c] && o < (int)sizeof(buf)-2; c++) buf[o++] = ed_lines[i][c];
        if (i < ed_nlines-1 && o < (int)sizeof(buf)-1) buf[o++] = '\n';
    }
    dirent_t e; if (sys_vfs_stat(ed_path, &e) != 0) sys_vfs_create(ed_path, 0);   // cree si absent
    vfs_io_t io = { ed_path, 0, buf, (uint64_t)o };
    long w = sys_vfs_save(&io);
    if (w >= 0) ed_dirty = 0;
    return (int)w;
}

static void ed_draw(const char *msg) {
    int rows = ROWS - 3;                              // lignes de texte (1..ROWS-3)
    if (ed_cr < ed_top) ed_top = ed_cr;
    if (ed_cr >= ed_top + rows) ed_top = ed_cr - rows + 1;
    if (ed_cc < ed_left) ed_left = ed_cc;
    if (ed_cc >= ed_left + COLS) ed_left = ed_cc - COLS + 1;
    memset(cells, ' ', sizeof cells);
    // Barre de titre.
    int x = 0; for (const char *p = "sexOs nano: "; *p && x < COLS; p++) cells[0][x++] = *p;
    for (const char *p = ed_path; *p && x < COLS-2; p++) cells[0][x++] = *p;
    if (ed_dirty && x < COLS-1) cells[0][x++] = '*';
    // Texte (avec defilement horizontal).
    for (int i = 0; i < rows; i++) {
        int li = ed_top + i; if (li >= ed_nlines) break;
        const char *L = ed_lines[li]; int ln = ed_strlen(L);
        for (int c = 0; c < COLS; c++) { int sc = ed_left + c; if (sc < ln) cells[1+i][c] = L[sc]; }
    }
    // Ligne de message + raccourcis.
    if (msg) { int mx = 0; for (const char *p = msg; *p && mx < COLS; p++) cells[ROWS-2][mx++] = *p; }
    int sx = 0; for (const char *p = "^O Enreg  ^X Quitter  ^K Couper  ^U Coller"; *p && sx < COLS; p++) cells[ROWS-1][sx++] = *p;
    cx = ed_cc - ed_left; cy = 1 + (ed_cr - ed_top);
    redraw();
}

static void ed_clampcol(void) { int ll = ed_strlen(ed_lines[ed_cr]); if (ed_cc > ll) ed_cc = ll; }

static void ed_insert(char ch) {
    char *L = ed_lines[ed_cr]; int ll = ed_strlen(L);
    if (ll >= ED_MAXCOL-1) return;
    for (int i = ll; i >= ed_cc; i--) L[i+1] = L[i];
    L[ed_cc++] = ch; ed_dirty = 1;
}
static void ed_split(void) {
    if (ed_nlines >= ED_MAXLINES) return;
    for (int i = ed_nlines; i > ed_cr+1; i--) strcpy(ed_lines[i], ed_lines[i-1]);
    strcpy(ed_lines[ed_cr+1], ed_lines[ed_cr] + ed_cc);
    ed_lines[ed_cr][ed_cc] = 0;
    ed_nlines++; ed_cr++; ed_cc = 0; ed_dirty = 1;
}
static void ed_backspace(void) {
    char *L = ed_lines[ed_cr];
    if (ed_cc > 0) { int ll = ed_strlen(L); for (int i = ed_cc-1; i <= ll; i++) L[i] = L[i+1]; ed_cc--; ed_dirty = 1; }
    else if (ed_cr > 0) {
        int pl = ed_strlen(ed_lines[ed_cr-1]);
        if (pl + ed_strlen(L) < ED_MAXCOL-1) {
            strcat(ed_lines[ed_cr-1], L);
            for (int i = ed_cr; i < ed_nlines-1; i++) strcpy(ed_lines[i], ed_lines[i+1]);
            ed_nlines--; ed_cr--; ed_cc = pl; ed_dirty = 1;
        }
    }
}
static void ed_delete(void) {
    char *L = ed_lines[ed_cr]; int ll = ed_strlen(L);
    if (ed_cc < ll) { for (int i = ed_cc; i <= ll; i++) L[i] = L[i+1]; ed_dirty = 1; }
    else if (ed_cr < ed_nlines-1) {
        if (ll + ed_strlen(ed_lines[ed_cr+1]) < ED_MAXCOL-1) {
            strcat(L, ed_lines[ed_cr+1]);
            for (int i = ed_cr+1; i < ed_nlines-1; i++) strcpy(ed_lines[i], ed_lines[i+1]);
            ed_nlines--; ed_dirty = 1;
        }
    }
}
static void ed_cut_line(void) {
    char tmp[ED_MAXCOL+2]; strcpy(tmp, ed_lines[ed_cr]);
    int tl = ed_strlen(tmp); tmp[tl] = '\n'; tmp[tl+1] = 0;
    sys_clip_set(tmp, tl+1);                          // ligne copiee au presse-papiers
    if (ed_nlines > 1) {
        for (int i = ed_cr; i < ed_nlines-1; i++) strcpy(ed_lines[i], ed_lines[i+1]);
        ed_nlines--; if (ed_cr >= ed_nlines) ed_cr = ed_nlines-1;
    } else ed_lines[0][0] = 0;
    ed_cc = 0; ed_dirty = 1;
}
static void ed_paste(void) {
    static char cb[4096]; int n = sys_clip_get(cb, sizeof cb - 1); cb[n] = 0;
    for (int i = 0; i < n; i++) { if (cb[i] == '\n') ed_split(); else if (cb[i] != '\r') ed_insert(cb[i]); }
    ed_dirty = 1;
}

// nano <fichier> : ouvre l'editeur plein-ecran (Ctrl+O enregistre, Ctrl+X quitte).
static void cmd_nano(const char *arg) {
    if (!arg[0]) { tprint("usage: nano <fichier>\n"); return; }
    build_abs(arg, ed_path);
    ed_load(ed_path);
    ed_draw("Edition  (^O enregistrer, ^X quitter, ^K couper, ^U coller)");
    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (ev.type != EV_KEY || !ev.pressed) continue;
        if (ev.mods & MOD_CTRL) {
            char c = ev.ch;
            if (c == 'x' || c == 'X') break;
            else if (c == 'o' || c == 'O') { int w = ed_save(); ed_draw(w >= 0 ? "Enregistre." : "Echec (permission ?)"); }
            else if (c == 'k' || c == 'K') { ed_cut_line(); ed_draw("Ligne coupee (^U pour coller)."); }
            else if (c == 'u' || c == 'U') { ed_paste(); ed_draw("Colle."); }
            continue;
        }
        switch (ev.key) {
            case KEY_UP:    if (ed_cr > 0) ed_cr--; ed_clampcol(); break;
            case KEY_DOWN:  if (ed_cr < ed_nlines-1) ed_cr++; ed_clampcol(); break;
            case KEY_LEFT:  if (ed_cc > 0) ed_cc--; else if (ed_cr > 0) { ed_cr--; ed_cc = ed_strlen(ed_lines[ed_cr]); } break;
            case KEY_RIGHT: { int ll = ed_strlen(ed_lines[ed_cr]); if (ed_cc < ll) ed_cc++; else if (ed_cr < ed_nlines-1) { ed_cr++; ed_cc = 0; } } break;
            case KEY_HOME:  ed_cc = 0; break;
            case KEY_END:   ed_cc = ed_strlen(ed_lines[ed_cr]); break;
            case KEY_ENTER: ed_split(); break;
            case KEY_BACKSPACE: ed_backspace(); break;
            case KEY_DELETE: ed_delete(); break;
            default: if (ev.ch) ed_insert(ev.ch); break;
        }
        ed_draw(0);
    }
    memset(cells, ' ', sizeof cells); cx = cy = 0;     // ecran propre au retour
}

// --- « Phallus » : fastfetch (art en points a gauche + infos a droite) -------
//  Rendu DIRECT sur le canvas (hors grille texte), puis attente d'une touche.
static void draw_dot_art(int x0, int y0, int sc, uint32_t col) {
    for (int y = 0; y < PHALLUS_H; y++) {
        const char *row = phallus_art[y];
        for (int x = 0; row[x]; x++)
            if (row[x] == '#') canvas_fill_rect(cv, x0 + x*sc, y0 + y*sc, sc, sc, col);
    }
}
static void info(int x, int y, const char *label, const char *val, uint32_t lc, uint32_t vc) {
    canvas_draw_string(cv, label, x, y, lc, 1);
    canvas_draw_string(cv, val, x + (int)strlen(label)*8, y, vc, 1);
}
static void cmd_phallus(void) {
    uint32_t bg = rgb(0x0c,0x10,0x14), pink = rgb(0xff,0x7a,0xb0),
             acc = rgb(0x6e,0xe7,0x9a), w = rgb(0xe6,0xec,0xf2), dim = rgb(0x8a,0x98,0xa6);
    canvas_fill(cv, bg);
    draw_dot_art(8, 28, 2, pink);                       // art a gauche (echelle 2)
    int rx = 132, ry = 8;
    for (int i = 0; i < SEXOS_BANNER_LINES; i++)
        canvas_draw_string(cv, sexos_banner[i], rx, ry + i*16, pink, 1);
    sysinfo_t s; sys_sysinfo(&s);
    userinfo_t u; sys_whoami(&u);
    netinfo_t ni; sys_net_info(&ni);
    char b[80], n[24];
    int y = ry + SEXOS_BANNER_LINES*16 + 12;
    strcpy(b, u.name[0] ? u.name : "user"); strcat(b, "@sexos");
    canvas_draw_string(cv, b, rx, y, acc, 1); y += 16;
    canvas_draw_string(cv, "-----------------------", rx, y, dim, 1); y += 16;
    info(rx,y,"OS:      ","sexOs v2 (x86_64)", acc, w); y += 16;
    info(rx,y,"Noyau:   ","sexos 2.0", acc, w); y += 16;
    info(rx,y,"Shell:   ","terminal (ring 3)", acc, w); y += 16;
    info(rx,y,"Bureau:  ","compositeur ring 3", acc, w); y += 16;
    utoa(s.uptime_s, n); strcpy(b, n); strcat(b, " s"); info(rx,y,"Uptime:  ", b, acc, w); y += 16;
    utoa(s.mem_used_mb, n); strcpy(b, n); strcat(b, " / "); utoa(s.mem_total_mb, n); strcat(b, n); strcat(b, " Mio");
    info(rx,y,"Memoire: ", b, acc, w); y += 16;
    utoa(s.pci_count, n); info(rx,y,"PCI:     ", n, acc, w); y += 16;
    if (ni.up && ni.ip) {
        char ip[20]; ip[0] = 0;
        for (int i = 3; i >= 0; i--) { utoa((ni.ip >> (i*8)) & 0xff, n); strcat(ip, n); if (i) strcat(ip, "."); }
        info(rx,y,"IP:      ", ip, acc, w); y += 16;
    }
    y += 12;
    canvas_draw_string(cv, "(appuyez sur une touche pour revenir)", rx, y, dim, 1);
    win_damage();
    for (;;) { event_t e; int r = win_wait(&e); if (r < 0) sys_exit(0); if (e.type == EV_KEY && e.pressed) break; }
}

static void run(char *line) {
    char *cmd = line, *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }
    if (!cmd[0]) return;
    else if (!strcmp(cmd, "help")) cmd_help();
    else if (!strcmp(cmd, "clear")) { memset(cells, ' ', sizeof cells); cx = cy = 0; }
    else if (!strcmp(cmd, "echo")) { tprint(arg); tprint("\n"); }
    else if (!strcmp(cmd, "ls")) cmd_ls(arg);
    else if (!strcmp(cmd, "cd")) cmd_cd(arg);
    else if (!strcmp(cmd, "pwd")) { tprint(cwd); tprint("\n"); }
    else if (!strcmp(cmd, "cat")) cmd_cat(arg);
    else if (!strcmp(cmd, "mkdir")) cmd_make(arg, 1);
    else if (!strcmp(cmd, "touch")) cmd_make(arg, 0);
    else if (!strcmp(cmd, "rm")) cmd_rm(arg);
    else if (!strcmp(cmd, "whoami")) { userinfo_t u; sys_whoami(&u); tprint(u.name); if (u.is_admin) tprint(" (admin)"); tprint("\n"); }
    else if (!strcmp(cmd, "date")) cmd_date();
    else if (!strcmp(cmd, "sysinfo")) cmd_sysinfo();
    else if (!strcmp(cmd, "Phallus") || !strcmp(cmd, "phallus")) cmd_phallus();
    else if (!strcmp(cmd, "hostkey")) cmd_hostkey();
    else if (!strcmp(cmd, "pubkey")) cmd_pubkey();
    else if (!strcmp(cmd, "pubkey-add")) cmd_pubkey_add(arg);
    else if (!strcmp(cmd, "ssh-keygen")) cmd_keygen();
    else if (!strcmp(cmd, "nano") || !strcmp(cmd, "edit")) cmd_nano(arg);
    else { tprint(cmd); tprint(": commande inconnue\n"); }
}

int main(void) {
    cv = win_create(COLS * 8, ROWS * 16, "Terminal");
    if (!cv) return 1;
    memset(cells, ' ', sizeof cells);
    strcpy(cwd, "/home/user");
    dirent_t e; if (sys_vfs_stat(cwd, &e) != 0) strcpy(cwd, "/");
    for (int i = 0; i < SEXOS_BANNER_LINES; i++) { tprint(sexos_banner[i]); tprint("\n"); }
    tprint("Terminal ring 3. Tapez 'help', ou 'Phallus' pour le fastfetch.\n");
    show_prompt(); redraw();

    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (ev.type != EV_KEY || !ev.pressed) continue;

        // Raccourcis Ctrl : copier (la ligne de saisie) / coller.
        if ((ev.mods & MOD_CTRL) && ev.ch) {
            if (ev.ch == 'v' || ev.ch == 'V') {
                static char cb[4096]; int n = sys_clip_get(cb, sizeof cb - 1); cb[n] = 0;
                for (int i = 0; i < n && ilen < 255; i++) {
                    char c = cb[i]; if (c == '\n' || c == '\r') continue;
                    input[ilen++] = c; tputc(c);
                }
                input[ilen] = 0;
            } else if (ev.ch == 'c' || ev.ch == 'C') {
                sys_clip_set(input, ilen);              // copie la ligne courante
            }
            redraw(); continue;
        }

        if (ev.key == KEY_ENTER) {
            tputc('\n'); input[ilen] = 0;
            if (input[0]) hist_push(input);
            run(input);
            ilen = 0; input[0] = 0;
            show_prompt();
        } else if (ev.key == KEY_BACKSPACE) {
            if (ilen > 0) { ilen--; input[ilen] = 0; if (cx > 0) { cx--; cells[cy][cx] = ' '; } }
        } else if (ev.key == KEY_UP) {
            if (hist_view > 0) { hist_view--; set_input(history[hist_view]); }
        } else if (ev.key == KEY_DOWN) {
            if (hist_view < hist_count) { hist_view++; set_input(hist_view < hist_count ? history[hist_view] : ""); }
        } else if (ev.ch) {
            if (ilen < 255) { input[ilen++] = ev.ch; input[ilen] = 0; tputc(ev.ch); }
        }
        redraw();
    }
}
