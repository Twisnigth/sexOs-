// =============================================================================
//  user/monitor.c -- MONITEUR D'ACTIVITE (style btop) en PROCESSUS ring 3 separe
// -----------------------------------------------------------------------------
//  Application graphique complete, cliente du compositeur (fenetre via libwin).
//  Toutes les donnees proviennent d'appels systeme :
//    - sys_proc_list : table des processus (pid, etat, tops CPU, memoire),
//    - sys_sysinfo   : memoire totale/utilisee, uptime, PCI,
//    - sys_rtc       : date/heure.
//  Le % CPU est calcule a partir des tops du minuteur attribues a chaque tache
//  entre deux echantillons (idle = pid 0). Aucun pointeur noyau partage.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"

void utoa(unsigned long, char *);

#define W      784
#define H      560
#define PAD    12
#define MAXP   32
#define HIST   184

static canvas_t *cv;

typedef struct { uint8_t second, minute, hour, day, month; uint16_t year; } rtct_t;

static procinfo_t procs[MAXP];
static int        nprocs;
static int        cpu_pct[MAXP];        // % CPU par processus (aligne sur procs[])
static int        total_busy;           // % CPU global (hors idle)
static uint64_t   prev_ticks[64];       // tops du tour precedent, indexes par pid
static uint64_t   prev_total;
static int        hist[HIST];           // historique du % CPU global
static int        hist_n;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Dégradé de charge : vert (faible) -> jaune (moyen) -> rouge (fort).
static uint32_t heat(int p) {
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    int r, g, b;
    if (p < 50) { int t = p * 255 / 50; r = 0x3a + (0xf0 - 0x3a) * t / 255; g = 0xd0; b = 0x6a + (0x40 - 0x6a) * t / 255; }
    else        { int t = (p - 50) * 255 / 50; r = 0xf0; g = 0xd0 + (0x50 - 0xd0) * t / 255; b = 0x40; }
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// --- Petit constructeur de chaînes -------------------------------------------
static char *cat(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *catn(char *p, uint64_t v) { char t[24]; utoa(v, t); return cat(p, t); }
static char *cat2(char *p, int v) { *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }

static void fmt_mem(uint64_t kb, char *out) {
    char *p = out;
    if (kb >= 1024) { p = catn(p, kb / 1024); *p++ = '.'; p = catn(p, (kb % 1024) * 10 / 1024); p = cat(p, " Mio"); }
    else            { p = catn(p, kb); p = cat(p, " Kio"); }
}

static const char *statename(int s) {
    switch (s) { case 1: return "pret"; case 2: return "actif"; case 3: return "dort"; case 4: return "zombie"; default: return "?"; }
}

// --- Échantillonnage des compteurs noyau -------------------------------------
static void sample(void) {
    nprocs = 0;
    procinfo_t pi;
    for (int i = 0; i < MAXP && sys_proc_list(i, &pi) == 1; i++) procs[nprocs++] = pi;

    uint64_t total = 0;
    for (int i = 0; i < nprocs; i++) total += procs[i].cpu_ticks;
    uint64_t dtot = total > prev_total ? total - prev_total : 0;

    uint64_t idle_prev = 0, idle_cur = 0;
    for (int i = 0; i < nprocs; i++) {
        int pid = procs[i].pid;
        uint64_t pc = (pid >= 0 && pid < 64) ? prev_ticks[pid] : 0;
        uint64_t d  = procs[i].cpu_ticks > pc ? procs[i].cpu_ticks - pc : 0;
        cpu_pct[i]  = dtot ? (int)(d * 100 / dtot) : 0;
        if (pid == 0) { idle_prev = pc; idle_cur = procs[i].cpu_ticks; }
    }
    uint64_t didle = idle_cur > idle_prev ? idle_cur - idle_prev : 0;
    total_busy = dtot ? (int)((dtot - didle) * 100 / dtot) : 0;

    if (hist_n < HIST) hist[hist_n++] = total_busy;
    else { for (int i = 1; i < HIST; i++) hist[i - 1] = hist[i]; hist[HIST - 1] = total_busy; }

    for (int i = 0; i < nprocs; i++) { int pid = procs[i].pid; if (pid >= 0 && pid < 64) prev_ticks[pid] = procs[i].cpu_ticks; }
    prev_total = total;
}

static void redraw(void) {
    uint32_t BG = rgb(0x0e, 0x12, 0x18), PANEL = rgb(0x15, 0x1b, 0x24),
             TXT = rgb(0xcf, 0xd8, 0xe0), DIM = rgb(0x6a, 0x76, 0x86),
             ACC = rgb(0x4e, 0xc9, 0xff), BAR = rgb(0x1e, 0x26, 0x30),
             EDGE = rgb(0x2a, 0x32, 0x40);
    char buf[96], *p;
    canvas_fill(cv, BG);

    // --- En-tête : titre + date/heure ---------------------------------------
    canvas_fill_rect(cv, 0, 0, cv->width, 26, rgb(0x12, 0x18, 0x22));
    canvas_draw_string(cv, "sexOs  --  Moniteur d'activite", PAD, 5, ACC, 1);
    rtct_t t; sys_rtc(&t);
    p = buf; p = catn(p, t.year); *p++ = '-'; p = cat2(p, t.month); *p++ = '-'; p = cat2(p, t.day);
    *p++ = ' '; p = cat2(p, t.hour); *p++ = ':'; p = cat2(p, t.minute); *p++ = ':'; p = cat2(p, t.second); *p = 0;
    canvas_draw_string(cv, buf, cv->width - canvas_text_width(buf, 1) - PAD, 5, DIM, 1);

    // --- Section CPU : pourcentage + historique -----------------------------
    int cy = 36;
    canvas_draw_string(cv, "CPU", PAD, cy + 4, TXT, 1);
    p = buf; p = catn(p, total_busy); *p++ = '%'; *p = 0;
    canvas_draw_string(cv, buf, PAD + 44, cy, heat(total_busy), 2);

    int gx = PAD, gy = cy + 26, gw = cv->width - 2 * PAD, gh = 72;
    canvas_fill_rect(cv, gx, gy, gw, gh, PANEL);
    int colw = gw / HIST; if (colw < 1) colw = 1;
    for (int k = 0; k < hist_n; k++) {                 // le plus récent à droite
        int pct = hist[hist_n - 1 - k];
        int bx = gx + gw - colw - k * colw;
        if (bx < gx) break;
        int hh = pct * gh / 100; if (hh < 1 && pct > 0) hh = 1;
        canvas_fill_rect(cv, bx, gy + gh - hh, colw, hh, heat(pct));
    }
    canvas_draw_rect(cv, gx, gy, gw, gh, EDGE);

    // --- Section mémoire : jauge --------------------------------------------
    int my = gy + gh + 16;
    sysinfo_t si; sys_sysinfo(&si);
    int mempct = si.mem_total_mb ? (int)((uint64_t)si.mem_used_mb * 100 / si.mem_total_mb) : 0;
    canvas_draw_string(cv, "MEM", PAD, my, TXT, 1);
    int mgx = PAD + 44, mgw = 520, mgh = 16;
    canvas_fill_rect(cv, mgx, my - 1, mgw, mgh, PANEL);
    canvas_fill_rect(cv, mgx, my - 1, mgw * mempct / 100, mgh, heat(mempct));
    canvas_draw_rect(cv, mgx, my - 1, mgw, mgh, EDGE);
    p = buf; p = catn(p, si.mem_used_mb); p = cat(p, " / "); p = catn(p, si.mem_total_mb);
    p = cat(p, " Mio  ("); p = catn(p, mempct); p = cat(p, "%)"); *p = 0;
    canvas_draw_string(cv, buf, mgx + mgw + 12, my, DIM, 1);

    // --- Table des processus -------------------------------------------------
    int ty = my + 32;
    canvas_fill_rect(cv, 0, ty - 2, cv->width, 18, rgb(0x12, 0x18, 0x22));
    canvas_draw_string(cv, "PID",  PAD,       ty, DIM, 1);
    canvas_draw_string(cv, "NOM",  PAD + 54,  ty, DIM, 1);
    canvas_draw_string(cv, "ETAT", PAD + 232, ty, DIM, 1);
    canvas_draw_string(cv, "CPU",  PAD + 320, ty, DIM, 1);
    canvas_draw_string(cv, "MEM",  PAD + 560, ty, DIM, 1);
    ty += 20;

    // Index des processus visibles (hors idle et hors zombies), triés par CPU.
    int idx[MAXP], nv = 0;
    for (int i = 0; i < nprocs; i++) {
        if (procs[i].pid == 0 || procs[i].state == 4) continue;
        idx[nv++] = i;
    }
    for (int a = 0; a < nv; a++)
        for (int b = a + 1; b < nv; b++)
            if (cpu_pct[idx[b]] > cpu_pct[idx[a]]) { int tmp = idx[a]; idx[a] = idx[b]; idx[b] = tmp; }

    for (int r = 0; r < nv; r++) {
        int i = idx[r];
        int rowy = ty + r * 18;
        if (rowy + 16 > (int)cv->height - 22) break;
        if (r & 1) canvas_fill_rect(cv, 0, rowy - 2, cv->width, 18, rgb(0x12, 0x16, 0x1e));

        p = buf; catn(p, procs[i].pid);
        canvas_draw_string(cv, buf, PAD, rowy, TXT, 1);
        canvas_draw_string(cv, procs[i].name, PAD + 54, rowy, TXT, 1);
        uint32_t sc = procs[i].state == 2 ? rgb(0x6e, 0xe7, 0x9a) : procs[i].state == 3 ? DIM : TXT;
        canvas_draw_string(cv, statename(procs[i].state), PAD + 232, rowy, sc, 1);

        int cp = cpu_pct[i];
        int cbx = PAD + 320, cbw = 150;
        canvas_fill_rect(cv, cbx, rowy + 1, cbw, 12, BAR);
        canvas_fill_rect(cv, cbx, rowy + 1, cbw * cp / 100, 12, heat(cp));
        p = buf; p = catn(p, cp); *p++ = '%'; *p = 0;
        canvas_draw_string(cv, buf, cbx + cbw + 8, rowy, TXT, 1);

        char mm[24]; fmt_mem(procs[i].mem_kb, mm);
        canvas_draw_string(cv, mm, PAD + 560, rowy, DIM, 1);
    }

    // --- Pied de page --------------------------------------------------------
    int fy = cv->height - 17;
    canvas_fill_rect(cv, 0, fy - 3, cv->width, 20, rgb(0x12, 0x18, 0x22));
    p = buf; p = catn(p, nv); p = cat(p, " processus actifs   |   uptime ");
    p = catn(p, si.uptime_s); p = cat(p, " s   |   [q] quitter"); *p = 0;
    canvas_draw_string(cv, buf, PAD, fy, DIM, 1);

    win_damage();
}

int main(void) {
    win_set_resizable(1);
    cv = win_create(W, H, "Moniteur d'activite");
    if (!cv) return 1;
    sample();
    redraw();
    uint64_t last = sys_time_ms();
    for (;;) {
        // Draine les événements sans bloquer (l'appli se rafraîchit seule).
        event_t e; int r;
        while ((r = win_poll(&e)) != 0) {
            if (r < 0) sys_exit(0);
            if (r == 2) redraw();                 // redimensionnement
            else if (r == 1 && e.type == EV_KEY && e.pressed && (e.ch == 'q' || e.ch == 'Q')) sys_exit(0);
        }
        uint64_t now = sys_time_ms();
        if (now - last >= 600) { last = now; sample(); redraw(); }
        sys_yield();                              // commutation coopérative
    }
}
