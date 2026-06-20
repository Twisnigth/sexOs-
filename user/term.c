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
#include "http.h"           // curl/wget : client HTTP/HTTPS ring 3
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

static char *g_cap; static int g_caplen, g_capmax;   // redirection « > » : capture de la sortie

static void scroll_up(void) {
    for (int y = 1; y < ROWS; y++) memcpy(cells[y-1], cells[y], COLS);
    memset(cells[ROWS-1], ' ', COLS);
    cy = ROWS - 1;
}
static void tputc(char ch) {
    if (g_cap) { if (g_caplen < g_capmax - 1) g_cap[g_caplen++] = ch; return; }  // capture (redirection)
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
    tprint("fichiers : ls cd pwd cat head tail wc grep find tree stat du hexdump\n");
    tprint("           cp mv rm mkdir touch nano sort uniq rev   (echo ... > fichier)\n");
    tprint("systeme  : help clear echo whoami id users su passwd uname about date\n");
    tprint("           sysinfo uptime free df sync ps sleep cal seq calc base64 history reboot\n");
    tprint("reseau   : ip resolve ping curl wget   ssh user@hote [cmd]\n");
    tprint("cles ssh : hostkey pubkey pubkey-add ssh-keygen\n");
    tprint("fun      : cowsay cmatrix sex figlet fortune snake Phallus beep play\n");
    tprint("  fleche haut/bas : historique   ^C copier   ^V coller\n");
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

// --- Commandes utilitaires supplementaires -----------------------------------
static char fscratch[16384];                 // tampon partage (cp/mv/head/wc)

// Extrait le prochain mot de 'p' dans 'out' ; renvoie le pointeur apres le mot.
static const char *next_tok(const char *p, char *out, int max) {
    while (*p == ' ') p++;
    int i = 0; while (*p && *p != ' ' && i < max-1) out[i++] = *p++;
    out[i] = 0; return p;
}
// Formate une IP (ordre hote) "a.b.c.d" dans out.
static void ip_str(uint32_t ip, char *out) {
    out[0] = 0; char n[8];
    for (int i = 3; i >= 0; i--) { utoa((ip >> (i*8)) & 0xff, n); strcat(out, n); if (i) strcat(out, "."); }
}
// Copie le contenu d'un fichier (chemins absolus). Renvoie 0/-1.
static int copy_file(const char *srcabs, const char *dstabs) {
    vfs_io_t in = { srcabs, 0, fscratch, sizeof fscratch };
    long n = sys_vfs_read(&in); if (n < 0) return -1;
    dirent_t e; if (sys_vfs_stat(dstabs, &e) != 0) sys_vfs_create(dstabs, 0);
    vfs_io_t out = { dstabs, 0, fscratch, (uint64_t)n };
    return sys_vfs_save(&out) < 0 ? -1 : 0;
}

static void cmd_uname(void) { tprint("sexOs 2.0 x86_64\n"); }
static void cmd_id(void) {
    userinfo_t u; sys_whoami(&u);
    tprint("user="); tprint(u.name); tprint(u.is_admin ? " (admin)\n" : " (standard)\n");
}
static void cmd_about(void) {
    tprint("sexOs v2 -- systeme d'exploitation x86_64 (noyau C/asm + bureau ring 3)\n");
    tprint("  D\n  |\n  |\n  8\n");
}
static void cmd_uptime(void) {
    sysinfo_t s; sys_sysinfo(&s); char b[16];
    utoa(s.uptime_s, b); tprint("actif depuis "); tprint(b); tprint(" secondes\n");
}
static void cmd_free(void) {
    sysinfo_t s; sys_sysinfo(&s); char b[16];
    utoa(s.mem_used_mb, b); tprint("memoire utilisee : "); tprint(b); tprint(" Mio\n");
    utoa(s.mem_total_mb, b); tprint("memoire totale   : "); tprint(b); tprint(" Mio\n");
}
static void cmd_ip(void) {
    netinfo_t ni; sys_net_info(&ni);
    if (!ni.up) { tprint("reseau hors ligne\n"); return; }
    char b[20];
    ip_str(ni.ip, b);      tprint("IP        : "); tprint(b); tprint("\n");
    ip_str(ni.mask, b);    tprint("Masque    : "); tprint(b); tprint("\n");
    ip_str(ni.gateway, b); tprint("Passerelle: "); tprint(b); tprint("\n");
    ip_str(ni.dns, b);     tprint("DNS       : "); tprint(b); tprint("\n");
}
static void cmd_resolve(const char *arg) {
    if (!arg[0]) { tprint("usage: resolve <hote>\n"); return; }
    uint32_t ip = 0; int r = 0; uint64_t end = sys_time_ms() + 5000;
    do { r = sys_dns_resolve(arg, &ip); if (r == 0) sys_yield(); } while (r == 0 && sys_time_ms() < end);
    if (r == 1) { char b[20]; ip_str(ip, b); tprint(arg); tprint(" -> "); tprint(b); tprint("\n"); }
    else tprint("resolve: echec\n");
}
static void cmd_history(void) {
    for (int i = 0; i < hist_count; i++) { char n[8]; utoa(i+1, n); tprint(n); tprint("  "); tprint(history[i]); tprint("\n"); }
}
static void cmd_head(const char *arg) {
    if (!arg[0]) { tprint("usage: head <fichier>\n"); return; }
    char path[256]; build_abs(arg, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) { tprint("head: introuvable\n"); return; }
    fscratch[n] = 0; int lines = 0;
    for (long i = 0; i < n && lines < 10; i++) { tputc(fscratch[i]); if (fscratch[i] == '\n') lines++; }
    if (n > 0 && fscratch[n-1] != '\n') tprint("\n");
}
static void cmd_wc(const char *arg) {
    if (!arg[0]) { tprint("usage: wc <fichier>\n"); return; }
    char path[256]; build_abs(arg, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) { tprint("wc: introuvable\n"); return; }
    int lc = 0, wc = 0, inw = 0;
    for (long i = 0; i < n; i++) { char c = fscratch[i];
        if (c == '\n') lc++;
        if (c == ' ' || c == '\n' || c == '\t') inw = 0; else if (!inw) { inw = 1; wc++; } }
    char b[12];
    utoa(lc, b); tprint(b); tprint(" lignes  ");
    utoa(wc, b); tprint(b); tprint(" mots  ");
    utoa((unsigned long)n, b); tprint(b); tprint(" octets\n");
}
static void cmd_cp(const char *arg) {
    char s[128], d[128]; const char *p = next_tok(arg, s, sizeof s); next_tok(p, d, sizeof d);
    if (!s[0] || !d[0]) { tprint("usage: cp <source> <destination>\n"); return; }
    char ps[256], pd[256]; build_abs(s, ps); build_abs(d, pd);
    if (copy_file(ps, pd) != 0) tprint("cp: echec (source introuvable / permission ?)\n");
}
static void cmd_mv(const char *arg) {
    char s[128], d[128]; const char *p = next_tok(arg, s, sizeof s); next_tok(p, d, sizeof d);
    if (!s[0] || !d[0]) { tprint("usage: mv <source> <destination>\n"); return; }
    char ps[256], pd[256]; build_abs(s, ps); build_abs(d, pd);
    if (copy_file(ps, pd) != 0) { tprint("mv: echec\n"); return; }
    sys_vfs_delete(ps);
}
static void cmd_reboot(void) { tprint("redemarrage...\n"); redraw(); sys_reboot(); }
// beep [freq] [ms] : bip via le haut-parleur PC.
static void cmd_beep(const char *arg) {
    char t1[16], t2[16]; const char *p = next_tok(arg, t1, sizeof t1); next_tok(p, t2, sizeof t2);
    int f = 0, d = 0;
    for (const char *q = t1; *q >= '0' && *q <= '9'; q++) f = f*10 + (*q-'0');
    for (const char *q = t2; *q >= '0' && *q <= '9'; q++) d = d*10 + (*q-'0');
    if (f <= 0) f = 800;
    if (d <= 0) d = 200;
    if (f < 20) f = 20;
    if (f > 12000) f = 12000;
    if (d > 3000) d = 3000;
    sys_beep(f, d);
}
// play : joue une petite melodie.
static void cmd_play(void) {
    static const int mel[][2] = { {523,160},{587,160},{659,160},{698,160},{784,320},{698,160},{659,160},{587,160},{523,360} };
    for (int i = 0; i < 9; i++) sys_beep(mel[i][0], mel[i][1]);
}

// Analyse "a.b.c.d" -> ip (ordre hote). Renvoie 1 si valide.
static int parse_ip(const char *s, uint32_t *ip) {
    uint32_t part[4]; int pi = 0, v = 0, dig = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); dig = 1; if (v > 255) return 0; }
        else if (*p == '.' || *p == 0) { if (!dig || pi > 3) return 0; part[pi++] = v; v = 0; dig = 0; if (!*p) break; }
        else return 0;
    }
    if (pi != 4) return 0;
    *ip = (part[0]<<24) | (part[1]<<16) | (part[2]<<8) | part[3];
    return 1;
}

// ping <hote> : 4 echos ICMP (resout le nom au besoin).
static void cmd_ping(const char *arg) {
    if (!arg[0]) { tprint("usage: ping <hote>\n"); return; }
    uint32_t ip;
    if (!parse_ip(arg, &ip)) {
        int r = 0; uint64_t end = sys_time_ms() + 5000;
        do { r = sys_dns_resolve(arg, &ip); if (r == 0) sys_yield(); } while (r == 0 && sys_time_ms() < end);
        if (r != 1) { tprint("ping: hote introuvable\n"); return; }
    }
    char ips[20]; ip_str(ip, ips);
    tprint("ping "); tprint(ips); tprint("\n");
    for (int i = 0; i < 4; i++) {
        sys_ping_send(ip);
        uint64_t t0 = sys_time_ms(); int got = 0;
        while (sys_time_ms() - t0 < 1000) { sys_yield(); if (sys_ping_got()) { got = 1; break; } }
        if (got) { char b[12]; utoa((unsigned long)(sys_time_ms() - t0), b); tprint("reponse en "); tprint(b); tprint(" ms\n"); }
        else tprint("delai depasse\n");
        uint64_t p = sys_time_ms(); while (sys_time_ms() - p < 300) sys_yield();
    }
}

// curl <url> : recupere une URL (http/https) et affiche le corps.
static char http_body[65536];
static void cmd_curl(const char *arg) {
    if (!arg[0]) { tprint("usage: curl <url>\n"); return; }
    int status = 0; const char *err = 0;
    int n = http_fetch(arg, http_body, sizeof http_body - 1, &status, &err);
    if (n < 0) { tprint("curl: echec"); if (err) { tprint(" ("); tprint(err); tprint(")"); } tprint("\n"); return; }
    http_body[n] = 0;
    char sb[8]; utoa(status, sb);
    tprint("HTTP "); tprint(sb);
    if (http_last_secure) tprint(http_last_verified ? "  [TLS verifie]" : "  [TLS non verifie]");
    tprint("\n"); tprint(http_body);
    if (n > 0 && http_body[n-1] != '\n') tprint("\n");
}
// wget <url> [fichier] : telecharge une URL dans un fichier.
static void cmd_wget(const char *arg) {
    char url[256], file[128]; const char *p = next_tok(arg, url, sizeof url); next_tok(p, file, sizeof file);
    if (!url[0]) { tprint("usage: wget <url> [fichier]\n"); return; }
    int status = 0; const char *err = 0;
    int n = http_fetch(url, http_body, sizeof http_body - 1, &status, &err);
    if (n < 0) { tprint("wget: echec\n"); return; }
    if (!file[0]) strcpy(file, "index.sex");
    char path[256]; build_abs(file, path);
    dirent_t e; if (sys_vfs_stat(path, &e) != 0) sys_vfs_create(path, 0);
    vfs_io_t io = { path, 0, http_body, (uint64_t)n }; sys_vfs_save(&io);
    char b[12]; utoa((unsigned long)n, b); tprint(b); tprint(" octets -> "); tprint(file); tprint("\n");
}

// Sous-chaine presente dans 'hay' ?
static int str_contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

// --- grep / tail / stat / hexdump (un fichier dans fscratch) ------------------
static void cmd_grep(const char *arg) {
    char pat[128]; const char *p = next_tok(arg, pat, sizeof pat); while (*p == ' ') p++;
    if (!pat[0] || !*p) { tprint("usage: grep <motif> <fichier>\n"); return; }
    char path[256]; build_abs(p, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) { tprint("grep: introuvable\n"); return; }
    fscratch[n] = 0; int ls = 0;
    for (int i = 0; i <= (int)n; i++)
        if (i == (int)n || fscratch[i] == '\n') {
            fscratch[i] = 0;
            if (str_contains(fscratch + ls, pat)) { tprint(fscratch + ls); tprint("\n"); }
            ls = i + 1;
        }
}
static void cmd_tail(const char *arg) {
    if (!arg[0]) { tprint("usage: tail <fichier>\n"); return; }
    char path[256]; build_abs(arg, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) { tprint("tail: introuvable\n"); return; }
    fscratch[n] = 0; int lines = 0, start = 0;
    for (int i = (int)n - 1; i >= 0; i--) if (fscratch[i] == '\n') { lines++; if (lines > 10) { start = i + 1; break; } }
    tprint(fscratch + start);
    if (n > 0 && fscratch[n-1] != '\n') tprint("\n");
}
static void cmd_stat(const char *arg) {
    if (!arg[0]) { tprint("usage: stat <nom>\n"); return; }
    char path[256]; build_abs(arg, path);
    dirent_t e; if (sys_vfs_stat(path, &e) != 0) { tprint("stat: introuvable\n"); return; }
    char b[16];
    tprint("nom    : "); tprint(e.name); tprint("\n");
    tprint("type   : "); tprint(e.type == 1 ? "dossier\n" : "fichier\n");
    utoa((unsigned long)e.size, b); tprint("taille : "); tprint(b); tprint(" octets\n");
}
static void cmd_hexdump(const char *arg) {
    if (!arg[0]) { tprint("usage: hexdump <fichier>\n"); return; }
    char path[256]; build_abs(arg, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) { tprint("hexdump: introuvable\n"); return; }
    if (n > 256) n = 256;                              // limite l'affichage
    const char *hx = "0123456789abcdef";
    for (int i = 0; i < (int)n; i += 16) {
        char off[6]; for (int k = 3; k >= 0; k--) off[3-k] = hx[(i >> (k*4)) & 0xf]; off[4] = 0;
        tprint(off); tprint("  ");
        for (int j = 0; j < 16; j++) {
            if (i+j < (int)n) { char h[4]; h[0]=hx[(fscratch[i+j]>>4)&0xf]; h[1]=hx[fscratch[i+j]&0xf]; h[2]=' '; h[3]=0; tprint(h); }
            else tprint("   ");
        }
        tprint(" ");
        for (int j = 0; j < 16 && i+j < (int)n; j++) { char c = fscratch[i+j]; char s[2]; s[0]=(c>=32&&c<127)?c:'.'; s[1]=0; tprint(s); }
        tprint("\n");
    }
}

// --- find / tree / du (parcours recursif du VFS) -----------------------------
static void path_join(char *out, const char *dir, const char *name) {
    strcpy(out, dir); if (strcmp(dir, "/") != 0) strcat(out, "/"); strcat(out, name);
}
static void find_rec(const char *dir, const char *name) {
    dirent_t e; int i = 0;
    while (sys_vfs_list(dir, i++, &e) == 1) {
        char child[256]; path_join(child, dir, e.name);
        if (str_contains(e.name, name)) { tprint(child); if (e.type == 1) tprint("/"); tprint("\n"); }
        if (e.type == 1) find_rec(child, name);
    }
}
static void cmd_find(const char *arg) {
    if (!arg[0]) { tprint("usage: find <nom>\n"); return; }
    find_rec(cwd, arg);
}
static void tree_rec(const char *dir, int depth) {
    dirent_t e; int i = 0;
    while (sys_vfs_list(dir, i++, &e) == 1) {
        for (int d = 0; d < depth; d++) tprint("  ");
        tprint(e.name); if (e.type == 1) tprint("/"); tprint("\n");
        if (e.type == 1) { char child[256]; path_join(child, dir, e.name); tree_rec(child, depth + 1); }
    }
}
static void cmd_tree(const char *arg) {
    char path[256]; if (arg[0]) build_abs(arg, path); else strcpy(path, cwd);
    tprint(path); tprint("\n"); tree_rec(path, 1);
}
static unsigned long du_rec(const char *dir) {
    unsigned long total = 0; dirent_t e; int i = 0;
    while (sys_vfs_list(dir, i++, &e) == 1) {
        if (e.type == 1) { char child[256]; path_join(child, dir, e.name); total += du_rec(child); }
        else total += e.size;
    }
    return total;
}
static void cmd_du(const char *arg) {
    char path[256]; if (arg[0]) build_abs(arg, path); else strcpy(path, cwd);
    dirent_t e; if (sys_vfs_stat(path, &e) != 0) { tprint("du: introuvable\n"); return; }
    unsigned long t = (e.type == 1) ? du_rec(path) : e.size;
    char b[16]; utoa(t, b); tprint(b); tprint(" octets  "); tprint(path); tprint("\n");
}

// --- ps / sleep / cal --------------------------------------------------------
static void cmd_ps(void) {
    tprint("PID  ETAT    CPU    MEM(Kio)  NOM\n");
    procinfo_t p; int i = 0; char b[16];
    while (sys_proc_list(i++, &p) == 1) {
        utoa(p.pid, b); tprint(b); tprint("    ");
        const char *st = p.state==1?"pret  ":p.state==2?"actif ":p.state==3?"bloque":p.state==4?"zombie":"?     ";
        tprint(st); tprint("  ");
        utoa((unsigned long)p.cpu_ticks, b); tprint(b); tprint("    ");
        utoa((unsigned long)p.mem_kb, b); tprint(b); tprint("      ");
        tprint(p.name); tprint("\n");
    }
}
static void cmd_sleep(const char *arg) {
    int s = 0; for (const char *p = arg; *p >= '0' && *p <= '9'; p++) s = s*10 + (*p - '0');
    if (s <= 0) { tprint("usage: sleep <secondes>\n"); return; }
    uint64_t end = sys_time_ms() + (uint64_t)s * 1000;
    while (sys_time_ms() < end) sys_yield();
}
static void cmd_cal(void) {
    rtct_t t; sys_rtc(&t);
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y = t.year, mo = t.month;
    int days = dim[(mo-1) % 12];
    if (mo == 2 && ((y%4==0 && y%100!=0) || y%400==0)) days = 29;
    // Jour de la semaine du 1er (Zeller : h 0=samedi).
    int m = mo, Y = y; if (m < 3) { m += 12; Y--; }
    int K = Y % 100, J = Y / 100;
    int h = (1 + (13*(m+1))/5 + K + K/4 + J/4 + 5*J) % 7;
    int col = (h + 6) % 7;                              // 0=dimanche
    char b[8];
    utoa(mo, b); tprint("  mois "); tprint(b); tprint("/"); utoa(y, b); tprint(b); tprint("\n");
    tprint("Di Lu Ma Me Je Ve Sa\n");
    for (int c = 0; c < col; c++) tprint("   ");
    for (int d = 1; d <= days; d++) {
        if (d < 10) tprint(" ");
        utoa(d, b); tprint(b); tprint(" ");
        if (++col == 7) { col = 0; tprint("\n"); }
    }
    if (col != 0) tprint("\n");
}

// --- cowsay / cmatrix (fun) --------------------------------------------------
static void cmd_cowsay(const char *arg) {
    const char *msg = arg[0] ? arg : "Meuh !";
    int len = (int)strlen(msg);
    tprint(" "); for (int i = 0; i < len + 2; i++) tprint("_"); tprint("\n");
    tprint("< "); tprint(msg); tprint(" >\n");
    tprint(" "); for (int i = 0; i < len + 2; i++) tprint("-"); tprint("\n");
    tprint("        \\   ^__^\n");
    tprint("         \\  (oo)\\_______\n");
    tprint("            (__)\\       )\\/\\\n");
    tprint("                ||----w |\n");
    tprint("                ||     ||\n");
}
static unsigned g_rng;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 16) & 0x7fff; }
static void cmd_cmatrix(void) {
    g_rng = (unsigned)sys_time_ms() | 1u;
    memset(cells, ' ', sizeof cells);
    int head[COLS]; for (int x = 0; x < COLS; x++) head[x] = rnd() % ROWS;
    for (;;) {
        event_t e; if (win_poll(&e)) { if (e.type == EV_KEY && e.pressed) break; }
        for (int x = 0; x < COLS; x++) {
            if (rnd() % 3 == 0) {
                int yh = head[x];
                cells[yh][x] = (char)(33 + (rnd() % 94));
                int ty = yh - 6; if (ty < 0) ty += ROWS; cells[ty][x] = ' ';
                head[x] = (yh + 1) % ROWS;
            }
        }
        cx = COLS - 1; cy = ROWS - 1;                  // curseur hors du chemin
        redraw();
        uint64_t t = sys_time_ms(); while (sys_time_ms() - t < 60) sys_yield();
    }
    memset(cells, ' ', sizeof cells); cx = cy = 0;
}

// --- « sex » : animation coquine en ASCII (phallus qui pompe + coeurs) --------
static const char *heart_bmp[6] = {
    " ## ## ",
    "#######",
    "#######",
    " ##### ",
    "  ###  ",
    "   #   ",
};
static void draw_heart(int x0, int y0, int sc, uint32_t col) {
    for (int y = 0; y < 6; y++)
        for (int x = 0; heart_bmp[y][x]; x++)
            if (heart_bmp[y][x] == '#') canvas_fill_rect(cv, x0 + x*sc, y0 + y*sc, sc, sc, col);
}
static void cmd_sex(void) {
    uint32_t bg = rgb(0x16,0x05,0x10), pink = rgb(0xff,0x5a,0xa0),
             red = rgb(0xff,0x2a,0x55), w = rgb(0xff,0xdf,0xe8), dim = rgb(0x9a,0x6a,0x7a);
    int W = cv->width, H = cv->height;
    g_rng = (unsigned)sys_time_ms() | 1u;
    enum { NH = 14 };
    int hx[NH], hy[NH], hs[NH];
    for (int i = 0; i < NH; i++) { hx[i] = rnd() % (W - 16); hy[i] = rnd() % H; hs[i] = 2 + rnd() % 3; }
    int px = W/2 - (PHALLUS_W * 2) / 2;                 // phallus centre (echelle 2)
    for (int frame = 0; ; frame++) {
        event_t e; if (win_poll(&e)) { if (e.type == EV_KEY && e.pressed) break; }
        canvas_fill(cv, bg);
        // Coeurs qui montent.
        for (int i = 0; i < NH; i++) {
            draw_heart(hx[i], hy[i], hs[i], (i & 1) ? pink : red);
            hy[i] -= hs[i];
            if (hy[i] < -6*hs[i]) { hy[i] = H + (rnd() % 40); hx[i] = rnd() % (W - 16); hs[i] = 2 + rnd() % 3; }
        }
        // Phallus qui « pompe » (oscillation verticale, onde triangulaire).
        int tri = frame % 16; int off = (tri < 8 ? tri : 16 - tri) - 4;   // -4..+4
        draw_dot_art(px, H/2 - (PHALLUS_H * 2) / 2 + off * 4, 2, pink);
        // Titre + invite.
        const char *t = "sexOs";
        canvas_draw_string(cv, t, W/2 - canvas_text_width(t, 3) / 2, 6, w, 3);
        const char *q = "(touche pour quitter)";
        canvas_draw_string(cv, q, W/2 - canvas_text_width(q, 1) / 2, H - 16, dim, 1);
        win_damage();
        uint64_t t0 = sys_time_ms(); while (sys_time_ms() - t0 < 70) sys_yield();
    }
    memset(cells, ' ', sizeof cells); cx = cy = 0;
}

// Lit une ligne au clavier (boucle propre). secret=1 -> echo en '*'.
static void read_input(const char *prompt, char *out, int max, int secret) {
    tprint(prompt); redraw();
    int n = 0;
    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (ev.type != EV_KEY || !ev.pressed) continue;
        if (ev.key == KEY_ENTER) { tputc('\n'); break; }
        else if (ev.key == KEY_BACKSPACE) { if (n > 0) { n--; if (cx > 0) { cx--; cells[cy][cx] = ' '; } } }
        else if (ev.ch && n < max-1) { out[n++] = ev.ch; tputc(secret ? '*' : ev.ch); }
        redraw();
    }
    out[n] = 0;
}
static int s2i(const char *s) { int v=0, sg=1; if (*s=='-'){sg=-1;s++;} while (*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return v*sg; }
static void put_int(long r) { char b[24]; if (r<0){tprint("-");utoa((unsigned long)(-r),b);} else utoa((unsigned long)r,b); tprint(b); }

// --- Comptes & privileges ----------------------------------------------------
static void cmd_users(void) {
    userinfo_t u; int i = 0;
    while (sys_users_list(i++, &u) == 1) { tprint(u.name); tprint(u.is_admin ? "  (admin)     " : "  (standard)  "); tprint(u.home); tprint("\n"); }
}
static void cmd_su(const char *arg) {
    char name[32]; if (arg[0]) next_tok(arg, name, sizeof name); else strcpy(name, "root");
    char pw[64]; read_input("mot de passe: ", pw, sizeof pw, 1);
    if (sys_login(name, pw) == 0) { tprint("connecte en tant que "); tprint(name); tprint("\n"); }
    else tprint("su: authentification refusee\n");
}
static void cmd_passwd(void) {
    char oldp[64], n1[64], n2[64];
    read_input("ancien mot de passe : ", oldp, sizeof oldp, 1);
    read_input("nouveau mot de passe: ", n1, sizeof n1, 1);
    read_input("confirmer           : ", n2, sizeof n2, 1);
    if (strcmp(n1, n2) != 0) { tprint("les mots de passe ne correspondent pas\n"); return; }
    tprint(sys_passwd(oldp, n1) == 0 ? "mot de passe change\n" : "echec (ancien mot de passe incorrect ?)\n");
}

// --- calc : evaluateur arithmetique (+ - * / parentheses) --------------------
static const char *g_cp;
static long p_expr(void);
static long p_factor(void) {
    while (*g_cp==' ') g_cp++;
    if (*g_cp=='(') { g_cp++; long v=p_expr(); while(*g_cp==' ')g_cp++; if(*g_cp==')')g_cp++; return v; }
    if (*g_cp=='-') { g_cp++; return -p_factor(); }
    long v=0; while (*g_cp>='0'&&*g_cp<='9'){ v=v*10+(*g_cp-'0'); g_cp++; } return v;
}
static long p_term(void) {
    long v=p_factor();
    for (;;) { while(*g_cp==' ')g_cp++; char o=*g_cp;
        if (o=='*'){g_cp++; v*=p_factor();} else if(o=='/'){g_cp++; long d=p_factor(); v=d?v/d:0;} else break; }
    return v;
}
static long p_expr(void) {
    long v=p_term();
    for (;;) { while(*g_cp==' ')g_cp++; char o=*g_cp;
        if (o=='+'){g_cp++; v+=p_term();} else if(o=='-'){g_cp++; v-=p_term();} else break; }
    return v;
}
static void cmd_calc(const char *arg) {
    if (!arg[0]) { tprint("usage: calc <expression>\n"); return; }
    g_cp = arg; long r = p_expr(); put_int(r); tprint("\n");
}

// --- base64 [-d] <texte> -----------------------------------------------------
static const char B64T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void cmd_base64(const char *arg) {
    int dec = 0; const char *s = arg;
    if (strncmp(arg, "-d ", 3) == 0) { dec = 1; s = arg+3; while (*s==' ') s++; }
    if (!*s) { tprint("usage: base64 [-d] <texte>\n"); return; }
    if (!dec) {
        int n = (int)strlen(s);
        for (int i = 0; i < n; i += 3) {
            int r = n-i; unsigned v = (unsigned char)s[i]<<16;
            if (r>1) v |= (unsigned char)s[i+1]<<8;
            if (r>2) v |= (unsigned char)s[i+2];
            char o[5]; o[0]=B64T[(v>>18)&63]; o[1]=B64T[(v>>12)&63];
            o[2]=r>1?B64T[(v>>6)&63]:'='; o[3]=r>2?B64T[v&63]:'='; o[4]=0; tprint(o);
        }
        tprint("\n");
    } else {
        unsigned buf=0; int bits=0;
        for (const char *p=s; *p; p++) {
            int val=-1; char c=*p;
            if(c>='A'&&c<='Z')val=c-'A'; else if(c>='a'&&c<='z')val=c-'a'+26;
            else if(c>='0'&&c<='9')val=c-'0'+52; else if(c=='+')val=62; else if(c=='/')val=63; else continue;
            buf=(buf<<6)|val; bits+=6;
            if (bits>=8){ bits-=8; char s2[2]={(char)((buf>>bits)&0xFF),0}; tprint(s2); }
        }
        tprint("\n");
    }
}

// --- sort / uniq / rev (sur un fichier) --------------------------------------
static int split_lines(const char *arg, char *lines[], int maxlines) {
    char path[256]; build_abs(arg, path);
    vfs_io_t io = { path, 0, fscratch, sizeof fscratch - 1 };
    long n = sys_vfs_read(&io); if (n < 0) return -1;
    fscratch[n] = 0; int nl = 0; char *p = fscratch;
    while (*p && nl < maxlines) { lines[nl++] = p; while (*p && *p != '\n') p++; if (*p) { *p = 0; p++; } }
    return nl;
}
static char *g_lines[1024];
static void cmd_sort(const char *arg) {
    if (!arg[0]) { tprint("usage: sort <fichier>\n"); return; }
    int nl = split_lines(arg, g_lines, 1024); if (nl < 0) { tprint("sort: introuvable\n"); return; }
    for (int i = 1; i < nl; i++) { char *k = g_lines[i]; int j = i-1; while (j>=0 && strcmp(g_lines[j],k)>0){g_lines[j+1]=g_lines[j];j--;} g_lines[j+1]=k; }
    for (int i = 0; i < nl; i++) { tprint(g_lines[i]); tprint("\n"); }
}
static void cmd_uniq(const char *arg) {
    if (!arg[0]) { tprint("usage: uniq <fichier>\n"); return; }
    int nl = split_lines(arg, g_lines, 1024); if (nl < 0) { tprint("uniq: introuvable\n"); return; }
    for (int i = 0; i < nl; i++) if (i==0 || strcmp(g_lines[i], g_lines[i-1])!=0) { tprint(g_lines[i]); tprint("\n"); }
}
static void cmd_rev(const char *arg) {
    if (!arg[0]) { tprint("usage: rev <fichier>\n"); return; }
    int nl = split_lines(arg, g_lines, 1024); if (nl < 0) { tprint("rev: introuvable\n"); return; }
    for (int i = 0; i < nl; i++) { int l=(int)strlen(g_lines[i]); for (int j=l-1;j>=0;j--){ char s[2]={g_lines[i][j],0}; tprint(s);} tprint("\n"); }
}
static void cmd_df(void) {
    unsigned long used = du_rec("/"); sysinfo_t si; sys_sysinfo(&si); char b[16];
    tprint("Systeme de fichiers\n");
    utoa(used, b); tprint("  fichiers : "); tprint(b); tprint(" octets\n");
    utoa(si.mem_used_mb, b); tprint("  RAM      : "); tprint(b); tprint(" / ");
    utoa(si.mem_total_mb, b); tprint(b); tprint(" Mio\n");
}
// sync : ecrit le systeme de fichiers sur le disque (persistance).
static void cmd_sync(void) {
    int r = sys_sync();
    if (r == 0) tprint("systeme de fichiers enregistre sur le disque\n");
    else tprint("pas de disque persistant (lance QEMU avec -hda disque.img)\n");
}
static void cmd_seq(const char *arg) {
    char t1[16], t2[16]; const char *p = next_tok(arg, t1, sizeof t1); next_tok(p, t2, sizeof t2);
    int a, b; if (t2[0]) { a = s2i(t1); b = s2i(t2); } else { a = 1; b = s2i(t1); }
    char bb[12]; for (int i = a; i <= b && i < a + 5000; i++) { utoa(i, bb); tprint(bb); tprint("\n"); }
}

// --- figlet / fortune (fun) --------------------------------------------------
static void cmd_figlet(const char *arg) {
    const char *t = arg[0] ? arg : "sexOs";
    canvas_fill(cv, rgb(0x0c,0x10,0x14));
    int sc = 6, tw = canvas_text_width(t, sc);
    while (tw > cv->width - 8 && sc > 1) { sc--; tw = canvas_text_width(t, sc); }
    canvas_draw_string(cv, t, (cv->width - tw)/2, cv->height/2 - 8*sc, rgb(0x6e,0xe7,0x9a), sc);
    canvas_draw_string(cv, "(touche pour quitter)", 8, cv->height - 16, rgb(0x8a,0x98,0xa6), 1);
    win_damage();
    for (;;) { event_t e; int r = win_wait(&e); if (r < 0) sys_exit(0); if (e.type==EV_KEY && e.pressed) break; }
    memset(cells, ' ', sizeof cells); cx = cy = 0;
}
static const char *fortunes[] = {
    "La RAM est volatile, l'amour aussi.",
    "Un bon programmeur regarde des deux cotes avant de traverser une voie a sens unique.",
    "Il y a 10 sortes de gens : ceux qui comptent en binaire et les autres.",
    "sudo make moi un cafe.",
    "Le seul bug qui marche du premier coup est celui que tu n'as pas encore trouve.",
    "rm -rf / : ne le fais jamais. Vraiment.",
    "On ne debogue pas, on ajoute des kprintf.",
    "Le reseau est en panne ? As-tu essaye de l'eteindre et de le rallumer ?",
};
static void cmd_fortune(void) {
    g_rng = (unsigned)sys_time_ms() | 1u;
    int n = (int)(sizeof fortunes / sizeof fortunes[0]);
    tprint(fortunes[rnd() % n]); tprint("\n");
}

// --- snake (jeu) : rendu graphique couleur, niveaux, pause, rejouer ----------
static void draw_ctext(const char *s, int y, int sc, uint32_t col) {
    canvas_draw_string(cv, s, cv->width/2 - canvas_text_width(s, sc)/2, y, col, sc);
}
// Rectangle aux coins arrondis (les coins prennent la couleur de fond 'bg').
static void fill_round(int x, int y, int w, int h, uint32_t c, uint32_t bg) {
    canvas_fill_rect(cv, x, y, w, h, c);
    canvas_fill_rect(cv, x, y, 2, 2, bg);
    canvas_fill_rect(cv, x+w-2, y, 2, 2, bg);
    canvas_fill_rect(cv, x, y+h-2, 2, 2, bg);
    canvas_fill_rect(cv, x+w-2, y+h-2, 2, 2, bg);
}
static void cmd_snake(void) {
    int W = cv->width, H = cv->height;
    const int GS = 16, top = 20;                       // case 16px, barre de score
    int cols = (W - 8) / GS, rows = (H - top - 8) / GS;
    if (cols > 60) cols = 60;
    if (rows > 40) rows = 40;
    int ox = (W - cols*GS) / 2, oy = top + (H - top - rows*GS) / 2;
    // Le "serpent" est un SEXE : hampe rose (corps), gland (tete), bourses (queue).
    uint32_t bg = rgb(0x0a,0x0e,0x12), border = rgb(0x3a,0x44,0x58),
             glans = rgb(0xff,0x7a,0xb8), shaft = rgb(0xff,0x9a,0xc8), balls = rgb(0xf0,0x86,0xbe),
             food = rgb(0xff,0x4f,0x5a), wt = rgb(0xe6,0xec,0xf2), dim = rgb(0x8a,0x98,0xa6), slit = rgb(0x9a,0x2a,0x5a);
    static int best = 0;                                // record de la session
    for (;;) {                                          // boucle de parties
        static int sx[2600], sy[2600];
        int len = 4, dx = 1, dy = 0, pdx = 1, pdy = 0;
        for (int i = 0; i < len; i++) { sx[i] = cols/2 - i; sy[i] = rows/2; }
        g_rng = (unsigned)sys_time_ms() | 1u;
        int fx = rnd()%cols, fy = rnd()%rows, score = 0, dead = 0, paused = 0, delay = 140;
        for (;;) {
            event_t e;
            while (win_poll(&e)) if (e.type==EV_KEY && e.pressed) {
                if (e.key==KEY_UP && pdy==0)        { dx=0; dy=-1; }
                else if (e.key==KEY_DOWN && pdy==0) { dx=0; dy=1; }
                else if (e.key==KEY_LEFT && pdx==0) { dx=-1; dy=0; }
                else if (e.key==KEY_RIGHT && pdx==0){ dx=1; dy=0; }
                else if (e.ch=='p' || e.ch=='P')    paused = !paused;
                else if (e.key==KEY_ESC || e.ch=='q') dead = 2;
            }
            if (dead) break;
            if (!paused) {
                int nx = sx[0]+dx, ny = sy[0]+dy;
                if (nx<0 || nx>=cols || ny<0 || ny>=rows) { dead = 1; break; }
                for (int i = 0; i < len; i++) if (sx[i]==nx && sy[i]==ny) dead = 1;
                if (dead) break;
                int grow = (nx==fx && ny==fy);
                for (int i = len; i > 0; i--) { sx[i]=sx[i-1]; sy[i]=sy[i-1]; }
                sx[0]=nx; sy[0]=ny; pdx=dx; pdy=dy;
                if (grow) {
                    if (len < 2599) len++;
                    score++;
                    for (;;) {
                        fx=rnd()%cols; fy=rnd()%rows; int on=0;
                        for (int i=0;i<len;i++) if (sx[i]==fx&&sy[i]==fy) { on=1; break; }
                        if (!on) break;
                    }
                    if (score % 5 == 0 && delay > 55) delay -= 12;   // accelere par paliers
                }
            }
            // --- rendu ---
            canvas_fill(cv, bg);
            char b[40]; char n[8];
            strcpy(b, "SNAKE   score "); utoa(score, n); strcat(b, n);
            strcat(b, "   record "); utoa(best>score?best:score, n); strcat(b, n);
            canvas_draw_string(cv, b, 6, 3, wt, 1);
            canvas_draw_string(cv, "fleches  p:pause  q:quitter", W - 27*8 - 6, 3, dim, 1);
            canvas_fill_rect(cv, ox-3, oy-3, cols*GS+6, 3, border);
            canvas_fill_rect(cv, ox-3, oy+rows*GS, cols*GS+6, 3, border);
            canvas_fill_rect(cv, ox-3, oy-3, 3, rows*GS+6, border);
            canvas_fill_rect(cv, ox+cols*GS, oy-3, 3, rows*GS+6, border);
            canvas_fill_rect(cv, ox+fx*GS+3, oy+fy*GS+3, GS-6, GS-6, food);   // pomme
            for (int i = len-1; i >= 0; i--) {
                int px = ox+sx[i]*GS, py = oy+sy[i]*GS;
                if (i == len-1 && len > 2) {
                    // queue = les bourses (deux boules cote a cote)
                    fill_round(px+1, py+4, 6, 10, balls, bg);
                    fill_round(px+8, py+4, 6, 10, balls, bg);
                } else if (i == 0) {
                    // tete = le gland (bulbe rose plus vif, pleine case) + la fente
                    fill_round(px, py, GS, GS, glans, bg);
                    int fxp = px+GS/2 + dx*(GS/2-3), fyp = py+GS/2 + dy*(GS/2-3);
                    canvas_fill_rect(cv, fxp-1, fyp-1, 3, 3, slit);
                } else {
                    // corps = la hampe
                    fill_round(px+1, py+1, GS-2, GS-2, shaft, bg);
                }
            }
            if (paused) draw_ctext("-- PAUSE --", H/2, 2, wt);
            win_damage();
            uint64_t t = sys_time_ms(); while (sys_time_ms()-t < (unsigned)delay) sys_yield();
        }
        if (score > best) best = score;
        if (dead == 2) break;                          // quitte le jeu
        // --- ecran de fin ---
        canvas_fill(cv, bg);
        draw_ctext("GAME OVER", H/2 - 50, 3, food);
        char b[40], n[8]; strcpy(b, "score : "); utoa(score, n); strcat(b, n); draw_ctext(b, H/2 - 6, 2, wt);
        strcpy(b, "record : "); utoa(best, n); strcat(b, n); draw_ctext(b, H/2 + 16, 1, dim);
        draw_ctext("R : rejouer     Q : quitter", H/2 + 44, 1, wt);
        win_damage();
        int again = 0;
        for (;;) { event_t e; int r = win_wait(&e); if (r<0) sys_exit(0);
            if (e.type==EV_KEY && e.pressed) { if (e.ch=='r'||e.ch=='R'){again=1;break;} if (e.ch=='q'||e.key==KEY_ESC){break;} } }
        if (!again) break;
    }
    memset(cells, ' ', sizeof cells); cx = cy = 0;
}

// --- ssh : client SSH (se connecter vers une autre machine) ------------------
static char ssh_out[32768];
static void cmd_ssh(const char *arg) {
    if (!arg[0]) { tprint("usage: ssh user@hote [commande]\n"); return; }
    char target[128]; const char *rest = next_tok(arg, target, sizeof target); while (*rest==' ') rest++;
    int at = -1; for (int i = 0; target[i]; i++) if (target[i]=='@') { at = i; break; }
    if (at < 0) { tprint("format attendu : user@hote\n"); return; }
    char user[64], host[128]; int k = 0;
    for (int i = 0; i < at && k < 63; i++) user[k++] = target[i];
    user[k] = 0; k = 0;
    for (int i = at+1; target[i] && k < 127; i++) host[k++] = target[i];
    host[k] = 0;
    uint32_t ip;
    if (!parse_ip(host, &ip)) {
        int r = 0; uint64_t end = sys_time_ms() + 5000;
        do { r = sys_dns_resolve(host, &ip); if (r==0) sys_yield(); } while (r==0 && sys_time_ms() < end);
        if (r != 1) { tprint("ssh: hote introuvable\n"); return; }
    }
    char pw[64]; read_input("mot de passe: ", pw, sizeof pw, 1);
    sshreq_t req = { ip, 22, user, pw, "", ssh_out, sizeof ssh_out - 1 };
    if (rest[0]) {
        req.command = rest;
        int n = sys_ssh_exec(&req);
        if (n < 0) { tprint("ssh: echec (connexion / authentification ?)\n"); return; }
        ssh_out[n] = 0; tprint(ssh_out); if (n>0 && ssh_out[n-1]!='\n') tprint("\n");
    } else {
        tprint("connecte. Tapez des commandes ('exit' pour quitter).\n");
        for (;;) {
            char line[256], prompt[200];
            strcpy(prompt, user); strcat(prompt, "@"); strcat(prompt, host); strcat(prompt, "$ ");
            read_input(prompt, line, sizeof line, 0);
            if (strcmp(line, "exit") == 0) break;
            if (!line[0]) continue;
            req.command = line;
            int n = sys_ssh_exec(&req);
            if (n < 0) { tprint("(echec)\n"); continue; }
            ssh_out[n] = 0; tprint(ssh_out); if (n>0 && ssh_out[n-1]!='\n') tprint("\n");
        }
    }
}

static char g_capbuf[16384];
static void run(char *line) {
    // Redirection « > fichier » : capture la sortie de la commande vers un fichier.
    char *redir = 0;
    for (char *q = line; *q; q++) if (*q == '>') { *q = 0; redir = q+1; while (*redir==' ') redir++; break; }
    if (redir && *redir) { g_cap = g_capbuf; g_caplen = 0; g_capmax = sizeof g_capbuf; }

    char *cmd = line, *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }
    if (!cmd[0]) { g_cap = 0; return; }
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
    else if (!strcmp(cmd, "id")) cmd_id();
    else if (!strcmp(cmd, "uname")) cmd_uname();
    else if (!strcmp(cmd, "about")) cmd_about();
    else if (!strcmp(cmd, "date")) cmd_date();
    else if (!strcmp(cmd, "sysinfo")) cmd_sysinfo();
    else if (!strcmp(cmd, "uptime")) cmd_uptime();
    else if (!strcmp(cmd, "free")) cmd_free();
    else if (!strcmp(cmd, "ip") || !strcmp(cmd, "ifconfig")) cmd_ip();
    else if (!strcmp(cmd, "resolve") || !strcmp(cmd, "nslookup")) cmd_resolve(arg);
    else if (!strcmp(cmd, "ping")) cmd_ping(arg);
    else if (!strcmp(cmd, "curl")) cmd_curl(arg);
    else if (!strcmp(cmd, "wget")) cmd_wget(arg);
    else if (!strcmp(cmd, "history")) cmd_history();
    else if (!strcmp(cmd, "cp")) cmd_cp(arg);
    else if (!strcmp(cmd, "mv")) cmd_mv(arg);
    else if (!strcmp(cmd, "head")) cmd_head(arg);
    else if (!strcmp(cmd, "tail")) cmd_tail(arg);
    else if (!strcmp(cmd, "wc")) cmd_wc(arg);
    else if (!strcmp(cmd, "grep")) cmd_grep(arg);
    else if (!strcmp(cmd, "find")) cmd_find(arg);
    else if (!strcmp(cmd, "tree")) cmd_tree(arg);
    else if (!strcmp(cmd, "stat")) cmd_stat(arg);
    else if (!strcmp(cmd, "du")) cmd_du(arg);
    else if (!strcmp(cmd, "hexdump") || !strcmp(cmd, "xxd")) cmd_hexdump(arg);
    else if (!strcmp(cmd, "ps")) cmd_ps();
    else if (!strcmp(cmd, "sleep")) cmd_sleep(arg);
    else if (!strcmp(cmd, "cal")) cmd_cal();
    else if (!strcmp(cmd, "cowsay")) cmd_cowsay(arg);
    else if (!strcmp(cmd, "cmatrix") || !strcmp(cmd, "matrix")) cmd_cmatrix();
    else if (!strcmp(cmd, "sex")) cmd_sex();
    else if (!strcmp(cmd, "beep")) cmd_beep(arg);
    else if (!strcmp(cmd, "play")) cmd_play();
    else if (!strcmp(cmd, "reboot")) cmd_reboot();
    else if (!strcmp(cmd, "Phallus") || !strcmp(cmd, "phallus")) cmd_phallus();
    else if (!strcmp(cmd, "hostkey")) cmd_hostkey();
    else if (!strcmp(cmd, "pubkey")) cmd_pubkey();
    else if (!strcmp(cmd, "pubkey-add")) cmd_pubkey_add(arg);
    else if (!strcmp(cmd, "ssh-keygen")) cmd_keygen();
    else if (!strcmp(cmd, "ssh")) cmd_ssh(arg);
    else if (!strcmp(cmd, "nano") || !strcmp(cmd, "edit")) cmd_nano(arg);
    else if (!strcmp(cmd, "users")) cmd_users();
    else if (!strcmp(cmd, "su")) cmd_su(arg);
    else if (!strcmp(cmd, "passwd")) cmd_passwd();
    else if (!strcmp(cmd, "calc")) cmd_calc(arg);
    else if (!strcmp(cmd, "base64")) cmd_base64(arg);
    else if (!strcmp(cmd, "sort")) cmd_sort(arg);
    else if (!strcmp(cmd, "uniq")) cmd_uniq(arg);
    else if (!strcmp(cmd, "rev")) cmd_rev(arg);
    else if (!strcmp(cmd, "df")) cmd_df();
    else if (!strcmp(cmd, "sync")) cmd_sync();
    else if (!strcmp(cmd, "seq")) cmd_seq(arg);
    else if (!strcmp(cmd, "figlet") || !strcmp(cmd, "banner")) cmd_figlet(arg);
    else if (!strcmp(cmd, "fortune")) cmd_fortune();
    else if (!strcmp(cmd, "snake")) cmd_snake();
    else { tprint(cmd); tprint(": commande inconnue\n"); }

    // Fin de redirection : ecrit la sortie capturee dans le fichier.
    if (g_cap) {
        g_cap = 0; g_capbuf[g_caplen] = 0;
        if (redir && *redir) {
            char path[256]; build_abs(redir, path);
            dirent_t e; if (sys_vfs_stat(path, &e) != 0) sys_vfs_create(path, 0);
            vfs_io_t io = { path, 0, g_capbuf, (uint64_t)g_caplen }; sys_vfs_save(&io);
        }
    }
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
