// =============================================================================
//  user/web.c -- NAVIGATEUR web minimal en PROCESSUS ring 3 séparé.
// -----------------------------------------------------------------------------
//  Client du compositeur (fenêtre via libwin) ET de la pile réseau (sockets TCP
//  non bloquantes + DNS via syscalls, HTTP en espace utilisateur). Barre
//  d'adresse éditable, rendu HTML simplifié (texte) avec défilement.
//  Socle du futur navigateur : tout est en ring 3, le réseau passe par la tâche
//  réseau du noyau qui pompe le NIC.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"
#include "http.h"

void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
void utoa(unsigned long, char *);

#define M       8                 // marge
#define URLY    6                 // barre d'adresse
#define STY     30                // ligne de statut
#define BODYY   66                // début de la zone de page (laisse place au détail TLS)

static canvas_t *cv;
static char      url[256];
static int       urllen;
static char      bodybuf[400000];
static char      text[500000];
static int       textlen;
static int       status, bodylen, scroll, total_lines, loading;
static const char *err;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }
static int eqs(const char *a, const char *b) { int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0; return a[i] == b[i]; }

// --- HTML -> texte (suppression des balises, entités, sauts de bloc) ---------
static void htmlstrip(const char *src, int n) {
    int o = 0, lastsp = 1;
    for (int i = 0; i < n && o < (int)sizeof(text) - 2; ) {
        char ch = src[i];
        if (ch == '<') {
            int j = i + 1, closing = 0, ni = 0; char name[16];
            if (j < n && src[j] == '/') { closing = 1; j++; }
            while (j < n && ni < 15) {
                char c = src[j];
                if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) { if (c>='A'&&c<='Z') c+=32; name[ni++]=c; j++; }
                else break;
            }
            name[ni] = 0;
            while (j < n && src[j] != '>') j++;
            if (j < n) j++;
            if (!closing && (eqs(name,"script") || eqs(name,"style"))) {
                while (j < n) {                          // saute le contenu jusqu'à </...>
                    if (src[j]=='<' && j+1<n && src[j+1]=='/') {
                        int k = j+2, m = 0;
                        while (k<n && name[m] && ((src[k]|32)==name[m])) { k++; m++; }
                        if (!name[m]) { while (k<n && src[k]!='>') k++; if (k<n) k++; j = k; break; }
                    }
                    j++;
                }
            }
            if (eqs(name,"br")||eqs(name,"p")||eqs(name,"div")||eqs(name,"li")||eqs(name,"tr")||
                eqs(name,"h1")||eqs(name,"h2")||eqs(name,"h3")||eqs(name,"h4")||eqs(name,"hr")||
                eqs(name,"ul")||eqs(name,"ol")||eqs(name,"table")||eqs(name,"title")||eqs(name,"head")) {
                if (o > 0 && text[o-1] != '\n') { text[o++] = '\n'; lastsp = 1; }
            }
            i = j; continue;
        }
        if (ch == '&') {
            int j = i + 1, ei = 0; char e[10];
            while (j<n && src[j]!=';' && src[j]!='<' && src[j]!=' ' && ei<9) e[ei++]=src[j++];
            e[ei] = 0; if (j<n && src[j]==';') j++;
            char out = 0;
            if (eqs(e,"lt")) out='<'; else if (eqs(e,"gt")) out='>'; else if (eqs(e,"amp")) out='&';
            else if (eqs(e,"quot")) out='"'; else if (eqs(e,"apos")) out='\''; else if (eqs(e,"nbsp")) out=' ';
            else if (e[0]=='#') { int v=0; for (int k=1; e[k]; k++) if (e[k]>='0'&&e[k]<='9') v=v*10+(e[k]-'0'); out=(v>=32&&v<127)?(char)v:' '; }
            if (out == ' ') { if (!lastsp) { text[o++]=' '; lastsp=1; } }
            else if (out)   { text[o++]=out; lastsp=0; }
            i = j; continue;
        }
        // Espaces, caractères de contrôle et non-ASCII (UTF-8) -> espace simple
        // (la police est ASCII ; évite d'afficher des octets multi-octets bruts).
        if (ch <= ' ' || (unsigned char)ch > 126) { if (!lastsp) { text[o++]=' '; lastsp=1; } i++; continue; }
        text[o++] = ch; lastsp = 0; i++;
    }
    textlen = o;
}

static void draw_text(int cols, int rows) {
    uint32_t FG = rgb(0x18,0x1c,0x22);
    int line = 0, col = 0, x0 = M, y0 = BODYY + 2;
    for (int i = 0; i < textlen; ) {
        char ch = text[i];
        if (ch == '\n') { line++; col = 0; i++; continue; }
        if (ch == ' ')  { if (col > 0 && col < cols) col++; i++; continue; }
        int ws = i; while (i < textlen && text[i] != ' ' && text[i] != '\n') i++;
        int wl = i - ws;
        if (col + wl > cols && col > 0) { line++; col = 0; }
        for (int k = 0; k < wl; k++) {
            if (col >= cols) { line++; col = 0; }
            int vis = line - scroll;
            if (vis >= 0 && vis < rows) canvas_draw_char(cv, text[ws+k], x0 + col*8, y0 + vis*16, FG, 1);
            col++;
        }
    }
    total_lines = line + 1;
}

static void render(void) {
    uint32_t PAGE = rgb(0xf6,0xf6,0xf1), CHROME = rgb(0x22,0x28,0x32),
             BAR = rgb(0xff,0xff,0xff), TXT = rgb(0xe6,0xec,0xf2),
             DIM = rgb(0x9a,0xa6,0xb4), OK = rgb(0x5a,0xc8,0x7a), ERR = rgb(0xff,0x8a,0x7a);
    char buf[300], num[16];

    canvas_fill(cv, PAGE);
    // Barre supérieure (chrome).
    canvas_fill_rect(cv, 0, 0, cv->width, BODYY, CHROME);
    // Barre d'adresse.
    canvas_fill_rect(cv, M, URLY, cv->width - 2*M, 18, BAR);
    canvas_draw_string(cv, url, M + 6, URLY + 1, rgb(0x10,0x12,0x16), 1);
    int cux = M + 6 + urllen * 8;
    canvas_fill_rect(cv, cux, URLY + 2, 7, 14, rgb(0x2d,0x6c,0xdf));   // curseur
    // Ligne de statut.
    if (loading) {
        char *p = buf; const char *a = "Chargement de "; while (*a) *p++=*a++;
        for (int i=0; url[i] && i<200; i++) *p++=url[i];
        *p++='.'; *p++='.'; *p++='.'; *p=0;
        canvas_draw_string(cv, buf, M, STY, TXT, 1);
    } else if (err) {
        char *p = buf; const char *a = "Erreur : "; while (*a) *p++=*a++;
        const char *e = err; while (*e) *p++=*e++; *p=0;
        canvas_draw_string(cv, buf, M, STY, ERR, 1);
    } else {
        // Indicateur TLS (cadenas) pour les pages https.
        int sx = M;
        if (http_last_secure) {
            const char *tag = http_last_verified ? "[TLS verifie]" : "[TLS non verifie]";
            uint32_t tc = http_last_verified ? OK : rgb(0xff,0xc0,0x40);
            canvas_draw_string(cv, tag, sx, STY, tc, 1);
            sx += (int)strlen(tag) * 8 + 12;
        }
        char *p = buf; const char *a = "HTTP "; while (*a) *p++=*a++;
        utoa(status, num); for (char *q=num; *q; q++) *p++=*q;
        a = "  -  "; while (*a) *p++=*a++;
        utoa(bodylen, num); for (char *q=num; *q; q++) *p++=*q;
        a = " o  -  [Entree] charger"; while (*a) *p++=*a++; *p=0;
        canvas_draw_string(cv, buf, sx, STY, status==200?OK:DIM, 1);
        // Détail de vérification sur la ligne suivante si https.
        if (http_last_secure && http_last_vinfo[0])
            canvas_draw_string(cv, http_last_vinfo, M, STY + 16, DIM, 1);
    }

    // Zone de page.
    int cols = (cv->width - 2*M - 8) / 8;
    int rows = (cv->height - BODYY - 4) / 16;
    if (!loading) {
        draw_text(cols, rows);
        int maxs = total_lines - rows; if (maxs < 0) maxs = 0;
        if (scroll > maxs) { scroll = maxs; }     // borne le défilement
        // Barre de défilement.
        if (total_lines > rows) {
            int track = cv->height - BODYY - 4;
            int sh = track * rows / total_lines; if (sh < 12) sh = 12;
            int sy = BODYY + (maxs ? (track - sh) * scroll / maxs : 0);
            canvas_fill_rect(cv, cv->width - 5, BODYY, 4, track, rgb(0xdd,0xdd,0xd6));
            canvas_fill_rect(cv, cv->width - 5, sy, 4, sh, rgb(0x9a,0xa6,0xb4));
        }
    }
    win_damage();
}

static void do_fetch(void) {
    url[urllen] = 0;
    loading = 1; err = 0; status = 0; bodylen = 0; textlen = 0; scroll = 0;
    render();                                     // affiche « Chargement... »
    bodylen = http_fetch(url, bodybuf, sizeof bodybuf, &status, &err);
    loading = 0;
    if (bodylen < 0) { textlen = 0; bodylen = 0; }
    else htmlstrip(bodybuf, bodylen);
    scroll = 0;
    render();
}

int main(void) {
    cv = win_create(840, 600, "Navigateur");
    if (!cv) return 1;
    memset(text, 0, 1);
    strcpy(url, "https://google.com/");
    urllen = (int)strlen(url);

    // Affiche une page d'accueil, puis tente le chargement initial.
    netinfo_t ni; sys_net_info(&ni);
    if (ni.up && ni.ip) do_fetch();
    else { err = "reseau indisponible (pas d'IP)"; render(); }

    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (e.type != EV_KEY || !e.pressed) continue;
        if (e.key == KEY_ENTER)            do_fetch();
        else if (e.key == KEY_BACKSPACE) { if (urllen > 0) { urllen--; url[urllen] = 0; } render(); }
        else if (e.key == KEY_UP)       { if (scroll > 0) scroll--; render(); }
        else if (e.key == KEY_DOWN)     { scroll++; render(); }
        else if (e.key == KEY_PAGEUP)   { scroll -= 12; if (scroll < 0) scroll = 0; render(); }
        else if (e.key == KEY_PAGEDOWN) { scroll += 12; render(); }
        else if (e.ch)                  { if (urllen < 255) { url[urllen++] = e.ch; url[urllen] = 0; } render(); }
    }
}
