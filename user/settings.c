// =============================================================================
//  user/settings.c -- Application RING 3 (processus séparé) : Paramètres.
// -----------------------------------------------------------------------------
//  Réécriture en processus ring 3 indépendant de l'ancienne appli Paramètres du
//  noyau. Affiche les informations système (version, mémoire, uptime, PCI), le
//  compte courant et l'état réseau, via les appels système dédiés. Page unique
//  rafraîchie chaque seconde (mémoire/uptime vivants).
// =============================================================================
#include "monos.h"
#include "libwin.h"
#include "gfx.h"

void utoa(unsigned long, char *);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

static canvas_t *cv;
static int yy;

// Écrit une ligne « libellé : valeur » et avance le curseur vertical.
static void line(const char *s, uint32_t col) {
    canvas_draw_string(cv, s, 16, yy, col, 1);
    yy += 18;
}
// Construit « prefixe<nombre>suffixe » dans b.
static void numline(char *b, const char *pre, unsigned long v, const char *suf) {
    char num[24]; strcpy(b, pre); utoa(v, num); strcat(b, num); strcat(b, suf);
}
// Formate une IP (ordre hôte) en a.b.c.d.
static void ipstr(char *b, uint32_t ip) {
    char n[8]; b[0] = 0;
    for (int i = 3; i >= 0; i--) {
        utoa((ip >> (i*8)) & 0xff, n); strcat(b, n);
        if (i) strcat(b, ".");
    }
}

static void render(void) {
    uint32_t BG = rgb(0x14,0x17,0x22), TITLE = rgb(0x9a,0xc8,0xff),
             W = rgb(0xff,0xff,0xff), DIM = rgb(0xa8,0xb2,0xc2);
    char b[96];

    canvas_fill(cv, BG);
    yy = 16;

    // --- Système -------------------------------------------------------------
    sysinfo_t si; sys_sysinfo(&si);
    line("Systeme", TITLE);
    line("MonOS version 2.0  (x86_64)", W);
    line("Demarrage : Limine (UEFI/BIOS)", W);
    numline(b, "Memoire totale   : ", si.mem_total_mb, " Mio"); line(b, W);
    numline(b, "Memoire utilisee : ", si.mem_used_mb, " Mio"); line(b, W);
    numline(b, "Uptime           : ", si.uptime_s, " s"); line(b, W);
    numline(b, "Peripheriques PCI: ", si.pci_count, ""); line(b, W);
    yy += 8;

    // --- Utilisateur ---------------------------------------------------------
    userinfo_t ui; sys_whoami(&ui);
    line("Utilisateur", TITLE);
    strcpy(b, "Compte : "); strcat(b, ui.name);
    strcat(b, ui.is_admin ? "  (administrateur)" : "  (standard)"); line(b, W);
    strcpy(b, "Dossier perso : "); strcat(b, ui.home); line(b, W);
    yy += 8;

    // --- Réseau --------------------------------------------------------------
    netinfo_t ni; sys_net_info(&ni);
    line("Reseau", TITLE);
    if (ni.up && ni.ip) {
        char ip[20];
        ipstr(ip, ni.ip);      strcpy(b, "Adresse IP : "); strcat(b, ip); line(b, W);
        ipstr(ip, ni.gateway); strcpy(b, "Passerelle : "); strcat(b, ip); line(b, W);
        ipstr(ip, ni.dns);     strcpy(b, "DNS        : "); strcat(b, ip); line(b, W);
    } else {
        line("Interface hors ligne (pas d'adresse)", DIM);
    }

    win_damage();
}

int main(void) {
    cv = win_create(360, 380, "Parametres");
    if (!cv) return 1;
    render();
    uint64_t last = sys_time_ms() / 1000;
    for (;;) {
        event_t e; int r;
        while ((r = win_poll(&e)) != 0) { if (r < 0) sys_exit(0); }
        uint64_t s = sys_time_ms() / 1000;
        if (s != last) { last = s; render(); }   // rafraîchit uptime/mémoire
        sys_yield();
    }
}
