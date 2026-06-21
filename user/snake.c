// =============================================================================
//  user/snake.c -- Jeu Snake (processus ring 3, client du compositeur)
// -----------------------------------------------------------------------------
//  Grille fixe mise à l'échelle dans la fenêtre (redimensionnable). Déplacement
//  cadencé par le temps (sys_time_ms) ; l'appli dort entre deux images
//  (sys_wait_event) -> pas de CPU gaspillé. Flèches ou WASD ; Espace = rejouer.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"

void utoa(unsigned long, char *);
unsigned long strlen(const char *);

#define GW 24
#define GH 18
#define TOP 26                       // bandeau du score
#define MAXC (GW * GH)

static canvas_t *cv;
typedef struct { int x, y; } cell_t;
static cell_t snake[MAXC];
static int    slen, dir, pdir;       // dir : 0 haut 1 bas 2 gauche 3 droite
static cell_t food;
static int    score, best, over;
static uint32_t rng;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int on_snake(int x, int y) {
    for (int i = 0; i < slen; i++) if (snake[i].x == x && snake[i].y == y) return 1;
    return 0;
}
static void place_food(void) {
    do { food.x = rnd() % GW; food.y = rnd() % GH; } while (on_snake(food.x, food.y));
}
static void reset_game(void) {
    slen = 3; dir = pdir = 3;                         // vers la droite
    for (int i = 0; i < slen; i++) { snake[i].x = GW/2 - i; snake[i].y = GH/2; }
    score = 0; over = 0;
    place_food();
}

static void step(void) {
    if (over) return;
    dir = pdir;
    cell_t h = snake[0];
    if (dir == 0) h.y--; else if (dir == 1) h.y++; else if (dir == 2) h.x--; else h.x++;
    if (h.x < 0 || h.x >= GW || h.y < 0 || h.y >= GH) { over = 1; if (score > best) best = score; return; }
    for (int i = 0; i < slen; i++) if (snake[i].x == h.x && snake[i].y == h.y) { over = 1; if (score > best) best = score; return; }
    int grow = (h.x == food.x && h.y == food.y);
    if (grow && slen < MAXC) slen++;
    for (int i = slen - 1; i > 0; i--) snake[i] = snake[i-1];
    snake[0] = h;
    if (grow) { score++; place_food(); }
}

static void redraw(void) {
    int W = cv->width, H = cv->height;
    canvas_fill(cv, rgb(0x12, 0x16, 0x1e));
    // bandeau du score
    canvas_fill_rect(cv, 0, 0, W, TOP, rgb(0x1c, 0x22, 0x2e));
    char buf[48]; char num[16];
    int p = 0; const char *s = "Score: "; while (s[p]) { buf[p] = s[p]; p++; }
    utoa((unsigned long)score, num); for (int i = 0; num[i]; i++) buf[p++] = num[i];
    s = "   Record: "; for (int i = 0; s[i]; i++) buf[p++] = s[i];
    utoa((unsigned long)best, num); for (int i = 0; num[i]; i++) buf[p++] = num[i];
    buf[p] = 0;
    canvas_draw_string(cv, buf, 8, 8, rgb(0xe6, 0xec, 0xf2), 1);

    // aire de jeu : cellule carrée centrée
    int aw = W, ah = H - TOP;
    int cs = aw / GW; if (ah / GH < cs) cs = ah / GH; if (cs < 4) cs = 4;
    int gw = cs * GW, gh = cs * GH;
    int ox = (W - gw) / 2, oy = TOP + (ah - gh) / 2;
    canvas_fill_rect(cv, ox, oy, gw, gh, rgb(0x0c, 0x10, 0x16));
    canvas_draw_rect(cv, ox, oy, gw, gh, rgb(0x2a, 0x32, 0x40));
    // pomme
    canvas_fill_rect(cv, ox + food.x*cs + 1, oy + food.y*cs + 1, cs - 2, cs - 2, rgb(0xe0, 0x50, 0x50));
    // serpent
    for (int i = 0; i < slen; i++) {
        uint32_t col = (i == 0) ? rgb(0x8a, 0xf0, 0x9a) : rgb(0x4c, 0xc0, 0x6a);
        canvas_fill_rect(cv, ox + snake[i].x*cs + 1, oy + snake[i].y*cs + 1, cs - 2, cs - 2, col);
    }
    if (over) {
        const char *m = "PERDU - Espace pour rejouer";
        int tw = (int)strlen(m) * 8;
        canvas_fill_rect(cv, ox + (gw - tw)/2 - 8, oy + gh/2 - 12, tw + 16, 24, rgb(0x2d, 0x6c, 0xdf));
        canvas_draw_string(cv, m, ox + (gw - tw)/2, oy + gh/2 - 4, rgb(0xff,0xff,0xff), 1);
    }
    win_damage();
}

static void on_key(const event_t *e) {
    int k = e->key; char c = e->ch;
    if ((k == KEY_UP    || c=='w' || c=='W') && dir != 1) pdir = 0;
    else if ((k == KEY_DOWN  || c=='s' || c=='S') && dir != 0) pdir = 1;
    else if ((k == KEY_LEFT  || c=='a' || c=='A') && dir != 3) pdir = 2;
    else if ((k == KEY_RIGHT || c=='d' || c=='D') && dir != 2) pdir = 3;
    else if (c == ' ' && over) reset_game();
}

int main(void) {
    rng = (uint32_t)sys_time_ms() | 1u;
    win_set_resizable(1);
    cv = win_create(480, 400, "Snake");
    if (!cv) return 1;
    reset_game();
    redraw();

    uint64_t last = sys_time_ms();
    for (;;) {
        sys_wait_event(60);                      // dort jusqu'a une entree ou 60 ms
        event_t e; int r;
        while ((r = win_poll(&e)) != 0) {
            if (r < 0) sys_exit(0);
            if (r == 2) redraw();                // redimensionnement
            else if (r == 1 && e.type == EV_KEY && e.pressed) on_key(&e);
        }
        uint64_t now = sys_time_ms();
        int speed = 130 - (score * 4); if (speed < 60) speed = 60;   // accelere avec le score
        if (now - last >= (uint64_t)speed) { last = now; step(); redraw(); }
    }
}
