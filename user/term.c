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
    tprint("commandes : help clear echo ls cd pwd cat mkdir touch rm\n");
    tprint("            whoami date sysinfo Phallus\n");
    tprint("    ssh   : hostkey pubkey pubkey-add ssh-keygen\n");
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
    prompt(); redraw();

    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (ev.type != EV_KEY || !ev.pressed) continue;
        if (ev.key == KEY_ENTER) {
            tputc('\n'); input[ilen] = 0;
            run(input);
            ilen = 0; input[0] = 0;
            prompt();
        } else if (ev.key == KEY_BACKSPACE) {
            if (ilen > 0) { ilen--; input[ilen] = 0; if (cx > 0) { cx--; cells[cy][cx] = ' '; } }
        } else if (ev.ch) {
            if (ilen < 255) { input[ilen++] = ev.ch; input[ilen] = 0; tputc(ev.ch); }
        }
        redraw();
    }
}
