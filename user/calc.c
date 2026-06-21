// =============================================================================
//  user/calc.c -- Calculatrice graphique (processus ring 3, client compositeur)
// -----------------------------------------------------------------------------
//  Calculatrice entiere (les apps ring 3 n'ont pas la virgule flottante).
//  Boutons cliquables + saisie clavier. + - * / = C.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"

void utoa(unsigned long, char *);
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

#define W 204
#define H 280
#define DISP_H 50
#define COLS 4
#define ROWS 4
#define PAD 4

static canvas_t *cv;
static long acc, entry;
static char  op;
static int   has_entry;

// disposition des touches (4 colonnes x 4 lignes)
static const char keys[ROWS][COLS] = {
    {'7','8','9','/'},
    {'4','5','6','*'},
    {'1','2','3','-'},
    {'0','C','=','+'},
};

static long apply(long a, char o, long b) {
    switch (o) { case '+': return a+b; case '-': return a-b; case '*': return a*b;
                 case '/': return b ? a/b : 0; default: return b; }
}

static void itoa_s(long v, char *out) {
    int i = 0; if (v < 0) { out[i++] = '-'; v = -v; }
    char tmp[24]; utoa((unsigned long)v, tmp);
    int j = 0; while (tmp[j]) out[i++] = tmp[j++]; out[i] = 0;
}

static void redraw(void) {
    canvas_fill(cv, rgb(0x1a, 0x1d, 0x26));
    // ecran (afficheur)
    canvas_fill_rect(cv, PAD, PAD, W - 2*PAD, DISP_H, rgb(0x10, 0x14, 0x1a));
    canvas_draw_rect(cv, PAD, PAD, W - 2*PAD, DISP_H, rgb(0x3a, 0x42, 0x58));
    char buf[24]; itoa_s(has_entry ? entry : acc, buf);
    int tw = canvas_text_width(buf, 2);
    canvas_draw_string(cv, buf, W - PAD - 8 - tw, PAD + 16, rgb(0x8e, 0xf0, 0xb0), 2);
    // grille de boutons
    int gy = DISP_H + 2*PAD;
    int bw = (W - 2*PAD - (COLS-1)*PAD) / COLS;
    int bh = (H - gy - PAD - (ROWS-1)*PAD) / ROWS;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int x = PAD + c*(bw+PAD), y = gy + r*(bh+PAD);
            char k = keys[r][c];
            int isop = (k=='/'||k=='*'||k=='-'||k=='+'||k=='=');
            uint32_t col = (k=='C') ? rgb(0xc0,0x50,0x50)
                         : isop      ? rgb(0xe0,0x90,0x30)
                                     : rgb(0x32,0x38,0x48);
            canvas_fill_rect(cv, x, y, bw, bh, col);
            canvas_draw_rect(cv, x, y, bw, bh, rgb(0x12,0x14,0x1c));
            char s[2] = { k, 0 };
            canvas_draw_string(cv, s, x + bw/2 - 6, y + bh/2 - 8, rgb(0xff,0xff,0xff), 2);
        }
    win_damage();
}

static void press(char k) {
    if (k >= '0' && k <= '9') {
        entry = entry*10 + (k - '0'); has_entry = 1;
    } else if (k == 'C') {
        acc = 0; entry = 0; op = 0; has_entry = 0;
    } else if (k == '=') {
        if (op) { acc = apply(acc, op, has_entry ? entry : acc); op = 0; }
        else if (has_entry) acc = entry;
        entry = 0; has_entry = 0;
    } else {  // operateur + - * /
        if (op && has_entry) acc = apply(acc, op, entry);
        else if (has_entry)  acc = entry;
        op = k; entry = 0; has_entry = 0;
    }
}

// associe un clic (cx,cy) a une touche, ou 0
static char hit(int cx, int cy) {
    int gy = DISP_H + 2*PAD;
    int bw = (W - 2*PAD - (COLS-1)*PAD) / COLS;
    int bh = (H - gy - PAD - (ROWS-1)*PAD) / ROWS;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int x = PAD + c*(bw+PAD), y = gy + r*(bh+PAD);
            if (cx >= x && cx < x+bw && cy >= y && cy < y+bh) return keys[r][c];
        }
    return 0;
}

int main(void) {
    cv = win_create(W, H, "Calculatrice");
    if (!cv) return 1;
    redraw();
    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (e.type == EV_MOUSE && (e.buttons & MOUSE_LEFT)) {
            char k = hit(e.mx, e.my);
            if (k) { press(k); redraw(); }
        } else if (e.type == EV_KEY && e.pressed) {
            char k = 0;
            if (e.ch >= '0' && e.ch <= '9') k = e.ch;
            else if (e.ch=='+'||e.ch=='-'||e.ch=='*'||e.ch=='/') k = e.ch;
            else if (e.ch=='c'||e.ch=='C') k = 'C';
            else if (e.key == KEY_ENTER || e.ch=='=') k = '=';
            else if (e.key == KEY_BACKSPACE) { entry /= 10; if (entry==0) has_entry=0; redraw(); continue; }
            if (k) { press(k); redraw(); }
        }
    }
}
