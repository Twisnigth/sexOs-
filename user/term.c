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

void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
int strcmp(const char *, const char *);
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
    tprint("            whoami date sysinfo\n");
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
    else { tprint(cmd); tprint(": commande inconnue\n"); }
}

int main(void) {
    cv = win_create(COLS * 8, ROWS * 16, "Terminal");
    if (!cv) return 1;
    memset(cells, ' ', sizeof cells);
    strcpy(cwd, "/home/user");
    dirent_t e; if (sys_vfs_stat(cwd, &e) != 0) strcpy(cwd, "/");
    tprint("sexOs Terminal -- PROCESSUS ring 3 separe. Tapez 'help'.\n");
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
