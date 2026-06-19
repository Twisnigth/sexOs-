// =============================================================================
//  user/web.c -- NAVIGATEUR web en PROCESSUS ring 3 séparé.
// -----------------------------------------------------------------------------
//  Client du compositeur (fenêtre via libwin) ET de la pile réseau (sockets TCP
//  non bloquantes + DNS via syscalls, HTTP/HTTPS en espace utilisateur).
//  Le rendu HTML est délégué à html.c : parseur DOM + mise en page (titres, gras,
//  paragraphes, listes, liens). web.c PEINT les items et gère le défilement,
//  la barre d'adresse et les CLICS sur les liens (navigation).
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"
#include "http.h"
#include "html.h"

void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
void utoa(unsigned long, char *);

#define M       8                 // marge
#define URLY    6                 // barre d'adresse
#define STY     30                // ligne de statut
#define BODYY   66                // début de la zone de page (place au détail TLS)

static canvas_t *cv;
static char      url[512];
static int       urllen;
static char      bodybuf[400000];
static int       status, bodylen, scroll, total_h, nitems, loading;
static const char *err;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

// --- Résolution d'URL : href (souvent relatif) + URL de base -> URL absolue ---
static void url_resolve(const char *base, const char *href, char *out, int outsz) {
    // Absolu (contient "scheme://") ?
    int has_scheme = 0;
    for (const char *p = href; *p; p++) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') { has_scheme = 1; break; }
        if (!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='+'||*p=='-'||*p=='.')) break;
    }
    int oo = 0, cap = outsz - 1;
    #define PUT(s) do { for (const char *z = (s); *z && oo < cap; ) out[oo++] = *z++; } while (0)
    if (has_scheme) { PUT(href); out[oo] = 0; }
    else {
        // Découpe la base en scheme / host / path.
        char scheme[8] = {0}, host[160] = {0}, path[300] = {0};
        const char *b = base; int s = 0;
        while (*b && *b != ':' && s < 7) scheme[s++] = *b++; scheme[s] = 0;
        if (b[0] == ':' && b[1] == '/' && b[2] == '/') b += 3;
        int h = 0; while (*b && *b != '/' && h < 159) host[h++] = *b++; host[h] = 0;
        int pp = 0; if (*b != '/') path[pp++] = '/';
        while (*b && pp < 299) path[pp++] = *b++; path[pp] = 0;
        if (!path[0]) { path[0] = '/'; path[1] = 0; }

        if (href[0] == '#') { PUT(base); }
        else if (href[0] == '/' && href[1] == '/') { PUT(scheme); PUT(":"); PUT(href); }
        else {
            PUT(scheme); PUT("://"); PUT(host);
            if (href[0] == '/') PUT(href);
            else {                                  // relatif : dossier de la base + href
                int last = -1; for (int k = 0; path[k]; k++) if (path[k] == '/') last = k;
                for (int k = 0; k <= last && oo < cap; k++) out[oo++] = path[k];
                PUT(href);
            }
        }
        out[oo] = 0;
    }
    #undef PUT
    for (int k = 0; out[k]; k++) if (out[k] == '#') { out[k] = 0; break; }   // retire le fragment
}

static int content_w(void) { return cv->width - 2*M - 10; }    // largeur utile (place barre)
static int viewport_h(void) { return cv->height - BODYY - 4; }

static void draw_word(const html_item_t *it, int px, int py) {
    int sc = it->scale, cw = 8 * sc;
    for (int k = 0; k < it->len; k++) {
        char ch = it->text[k];
        canvas_draw_char(cv, ch, px + k*cw, py, it->color, sc);
        if (it->bold) canvas_draw_char(cv, ch, px + k*cw + 1, py, it->color, sc);   // gras = double tracé
    }
    if (it->under) canvas_fill_rect(cv, px, py + it->h - 2, it->w, 1, it->color);
}

static void render(void) {
    uint32_t PAGE = rgb(0xf6,0xf6,0xf1), CHROME = rgb(0x22,0x28,0x32),
             BAR = rgb(0xff,0xff,0xff), TXT = rgb(0xe6,0xec,0xf2),
             DIM = rgb(0x9a,0xa6,0xb4), OK = rgb(0x5a,0xc8,0x7a), ERR = rgb(0xff,0x8a,0x7a);
    char buf[300], num[16];

    canvas_fill(cv, PAGE);
    canvas_fill_rect(cv, 0, 0, cv->width, BODYY, CHROME);
    canvas_fill_rect(cv, M, URLY, cv->width - 2*M, 18, BAR);
    canvas_draw_string(cv, url, M + 6, URLY + 1, rgb(0x10,0x12,0x16), 1);
    int cux = M + 6 + urllen * 8;
    canvas_fill_rect(cv, cux, URLY + 2, 7, 14, rgb(0x2d,0x6c,0xdf));

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
        a = " o"; while (*a) *p++=*a++; *p=0;
        canvas_draw_string(cv, buf, sx, STY, status==200?OK:DIM, 1);
        // Titre de la page (à droite de la ligne de statut).
        const char *t = html_title();
        if (t[0]) { int tx = sx + (int)strlen(buf)*8 + 16; canvas_draw_string(cv, t, tx, STY, DIM, 1); }
        if (http_last_secure && http_last_vinfo[0])
            canvas_draw_string(cv, http_last_vinfo, M, STY + 16, DIM, 1);
    }

    // Zone de page : peinture des items visibles.
    int x0 = M, y0 = BODYY + 2, vh = viewport_h();
    int maxs = total_h - vh; if (maxs < 0) maxs = 0;
    if (scroll > maxs) scroll = maxs; if (scroll < 0) scroll = 0;
    if (!loading) {
        const html_item_t *its = html_items();
        for (int i = 0; i < nitems; i++) {
            int sy = its[i].y - scroll;
            if (sy + its[i].h <= 0 || sy >= vh) continue;          // hors écran
            draw_word(&its[i], x0 + its[i].x, y0 + sy);
        }
        // Barre de défilement.
        if (total_h > vh) {
            int track = vh, sh = track * vh / total_h; if (sh < 14) sh = 14;
            int syb = BODYY + (maxs ? (track - sh) * scroll / maxs : 0);
            canvas_fill_rect(cv, cv->width - 6, BODYY, 4, track, rgb(0xdd,0xdd,0xd6));
            canvas_fill_rect(cv, cv->width - 6, syb, 4, sh, rgb(0x9a,0xa6,0xb4));
        }
    }
    win_damage();
}

static void do_fetch(void) {
    url[urllen] = 0;
    loading = 1; err = 0; status = 0; bodylen = 0; nitems = 0; total_h = 0; scroll = 0;
    render();                                      // affiche « Chargement... »
    bodylen = http_fetch(url, bodybuf, sizeof bodybuf, &status, &err);
    loading = 0;
    if (bodylen < 0) { bodylen = 0; nitems = 0; total_h = 0; }
    else nitems = html_render(bodybuf, bodylen, content_w(), &total_h);
    scroll = 0;
    render();
}

int main(void) {
    cv = win_create(900, 640, "Navigateur");
    if (!cv) return 1;
    strcpy(url, "https://example.com/");
    urllen = (int)strlen(url);

    netinfo_t ni; sys_net_info(&ni);
    if (ni.up && ni.ip) do_fetch();
    else { err = "reseau indisponible (pas d'IP)"; render(); }

    int prevbtn = 0;
    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (e.type == EV_KEY && e.pressed) {
            if (e.key == KEY_ENTER)            do_fetch();
            else if (e.key == KEY_BACKSPACE) { if (urllen > 0) { urllen--; url[urllen] = 0; } render(); }
            else if (e.key == KEY_UP)       { scroll -= 40; render(); }
            else if (e.key == KEY_DOWN)     { scroll += 40; render(); }
            else if (e.key == KEY_PAGEUP)   { scroll -= viewport_h() - 24; render(); }
            else if (e.key == KEY_PAGEDOWN) { scroll += viewport_h() - 24; render(); }
            else if (e.ch)                  { if (urllen < (int)sizeof(url)-1) { url[urllen++] = e.ch; url[urllen] = 0; } render(); }
        } else if (e.type == EV_MOUSE) {
            int press = (e.buttons & MOUSE_LEFT) && !(prevbtn & MOUSE_LEFT);
            prevbtn = e.buttons;
            if (press && e.my >= BODYY) {                 // clic dans la page : lien ?
                int dx = e.mx - M, dy = (e.my - (BODYY + 2)) + scroll;
                const char *href = html_link_at(dx, dy);
                if (href && href[0]) {
                    char nu[512]; url_resolve(url, href, nu, sizeof nu);
                    strcpy(url, nu); urllen = (int)strlen(url);
                    do_fetch();
                }
            }
        }
    }
}
