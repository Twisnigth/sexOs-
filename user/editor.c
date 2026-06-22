// =============================================================================
//  user/editor.c -- Éditeur de texte (processus ring 3, client du compositeur)
// -----------------------------------------------------------------------------
//  Édition d'un fichier texte du VFS : déplacement, saisie, défilement,
//  enregistrement. Presse-papiers PARTAGÉ avec les autres applis (terminal…) :
//    Ctrl+C copie la ligne, Ctrl+X la coupe, Ctrl+V colle, Ctrl+S enregistre.
//  Le fichier à ouvrir est transmis par l'explorateur via sys_arg_set/get.
//  Fenêtre redimensionnable.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"

unsigned long strlen(const char *);
char *strcpy(char *, const char *);
void *memmove(void *, const void *, unsigned long);

#define TEXTCAP   (128 * 1024)
#define TOPH      24
#define LH        14                          // hauteur de ligne (police x1 = 8x8)
#define CW        8                           // largeur d'un caractère
#define GUTTER    44                          // marge gauche (numéros de ligne)

static canvas_t *cv;
static char  text[TEXTCAP];
static int   tlen;                            // octets utilisés
static int   cur;                             // position du curseur [0..tlen]
static int   top;                             // première ligne affichée
static char  path[256];
static int   dirty;
static char  status[96];

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }
static void set_status(const char *s) { int i=0; while (s[i] && i<95) { status[i]=s[i]; i++; } status[i]=0; }

// --- géométrie du texte ------------------------------------------------------
static int line_start(int off) { while (off > 0 && text[off-1] != '\n') off--; return off; }
static int line_end(int off)   { while (off < tlen && text[off] != '\n') off++; return off; }
static int cur_line(void) { int n=0; for (int i=0;i<cur;i++) if (text[i]=='\n') n++; return n; }
static int cur_col(void)  { return cur - line_start(cur); }
static int total_lines(void) { int n=1; for (int i=0;i<tlen;i++) if (text[i]=='\n') n++; return n; }
static int rows_visible(void) { int r=((int)cv->height - TOPH)/LH; return r<1?1:r; }

// Décalage du début de la ligne d'indice 'ln' (0-based), ou tlen si au-delà.
static int offset_of_line(int ln) {
    int off=0, n=0;
    while (n<ln && off<tlen) { if (text[off]=='\n') n++; off++; }
    return off;
}

static void ensure_visible(void) {
    int l = cur_line();
    if (l < top) top = l;
    int rv = rows_visible();
    if (l >= top + rv) top = l - rv + 1;
    if (top < 0) top = 0;
}

// --- édition -----------------------------------------------------------------
static void insert_bytes(const char *s, int n) {
    if (n <= 0 || tlen + n > TEXTCAP) return;
    memmove(text + cur + n, text + cur, (unsigned long)(tlen - cur));
    for (int i=0;i<n;i++) text[cur+i]=s[i];
    tlen += n; cur += n; dirty = 1;
}
static void insert_ch(char c) { insert_bytes(&c, 1); }
static void backspace(void) {
    if (cur <= 0) return;
    memmove(text + cur - 1, text + cur, (unsigned long)(tlen - cur));
    tlen--; cur--; dirty = 1;
}
static void del_fwd(void) {
    if (cur >= tlen) return;
    memmove(text + cur, text + cur + 1, (unsigned long)(tlen - cur - 1));
    tlen--; dirty = 1;
}

// --- fichier -----------------------------------------------------------------
static void load_file(void) {
    vfs_io_t io = { path, 0, text, TEXTCAP - 1 };
    long n = sys_vfs_read(&io);
    if (n < 0) { tlen = 0; set_status("nouveau fichier"); }
    else { tlen = (int)n; set_status("ouvert"); }
    cur = 0; top = 0; dirty = 0;
}
static void save_file(void) {
    vfs_io_t io = { path, 0, text, (uint64_t)tlen };
    long n = sys_vfs_save(&io);
    if (n < 0) {                                  // fichier absent : le créer puis réessayer
        if (sys_vfs_create(path, 0) == 0) n = sys_vfs_save(&io);
    }
    if (n < 0) set_status("enregistrement refuse");
    else { dirty = 0; set_status("enregistre"); }
}

// --- presse-papiers (partagé) ------------------------------------------------
static void copy_line(int cut) {
    int s = line_start(cur), e = line_end(cur);
    sys_clip_set(text + s, e - s);
    if (cut) {
        int de = (e < tlen) ? e + 1 : e;        // emporte le saut de ligne
        memmove(text + s, text + de, (unsigned long)(tlen - de));
        tlen -= de - s; cur = s; dirty = 1;
        set_status("ligne coupee");
    } else set_status("ligne copiee");
}
static void paste(void) {
    static char cb[8192];
    int n = sys_clip_get(cb, sizeof cb);
    insert_bytes(cb, n);
    set_status("colle");
}

// --- rendu -------------------------------------------------------------------
static void redraw(void) {
    uint32_t BG = rgb(0x1e,0x21,0x2b), TXT = rgb(0xe6,0xec,0xf2), DIM = rgb(0x6a,0x76,0x86);
    canvas_fill(cv, BG);

    // Barre du haut : nom du fichier, boutons, position.
    canvas_fill_rect(cv, 0, 0, cv->width, TOPH, rgb(0x2c,0x30,0x3e));
    canvas_fill_rect(cv, 4, 3, 64, 18, rgb(0x3a,0x42,0x58));
    canvas_draw_string(cv, "Enreg.", 12, 8, rgb(0xff,0xff,0xff), 1);
    canvas_fill_rect(cv, 74, 3, 64, 18, rgb(0x3a,0x42,0x58));
    canvas_draw_string(cv, "Nouv.", 84, 8, rgb(0xff,0xff,0xff), 1);
    const char *bn = path; for (const char *q=path;*q;q++) if (*q=='/') bn=q+1;
    canvas_draw_string(cv, bn, 148, 8, dirty ? rgb(0xff,0xd0,0x70) : TXT, 1);
    if (dirty) canvas_draw_string(cv, "*", 148 + (int)strlen(bn)*8 + 2, 8, rgb(0xff,0xd0,0x70), 1);
    // position ligne:col + statut, à droite
    char info[64]; int p=0;
    const char *st = status;
    while (st[p] && p<40) { info[p]=st[p]; p++; } info[p]=0;
    canvas_draw_string(cv, info, cv->width - 320, 8, DIM, 1);
    char pos[32]; int k=0; { int l=cur_line()+1, c=cur_col()+1; char t[12]; int ti=0;
        do { t[ti++]='0'+l%10; l/=10; } while(l); while(ti) pos[k++]=t[--ti]; pos[k++]=':';
        ti=0; do { t[ti++]='0'+c%10; c/=10; } while(c); while(ti) pos[k++]=t[--ti]; pos[k]=0; }
    canvas_draw_string(cv, pos, cv->width - 70, 8, DIM, 1);

    // Corps : lignes visibles.
    int rv = rows_visible();
    int off = offset_of_line(top);
    int cl = cur_line(), cc = cur_col();
    for (int r=0; r<rv; r++) {
        int ln = top + r, y = TOPH + r*LH;
        // numéro de ligne
        if (off <= tlen) {
            char num[8]; int ni=0, v=ln+1; char t[8]; int ti=0;
            do { t[ti++]='0'+v%10; v/=10; } while(v); while(ti) num[ni++]=t[--ti]; num[ni]=0;
            canvas_draw_string(cv, num, 6, y, rgb(0x44,0x4c,0x5c), 1);
        }
        // texte de la ligne
        int x = GUTTER, i = off;
        while (i < tlen && text[i] != '\n') {
            if (x > (int)cv->width - CW) break;
            char ch = text[i];
            if (ch == '\t') x += CW*4;
            else { if (ch >= ' ') canvas_draw_char(cv, ch, x, y, rgb(0xd6,0xde,0xe8), 1); x += CW; }
            i++;
        }
        // curseur sur cette ligne
        if (ln == cl) {
            int cx = GUTTER + cc*CW;
            if (cx <= (int)cv->width - 2) canvas_fill_rect(cv, cx, y-1, 2, LH, rgb(0x6e,0xc0,0xff));
        }
        // avancer au début de la ligne suivante
        while (off < tlen && text[off] != '\n') off++;
        if (off < tlen) off++;
        else { /* derniere ligne */ }
    }
    win_damage();
}

// --- entrées -----------------------------------------------------------------
static void move_vert(int dir) {
    int col = cur_col();
    int l = cur_line() + dir;
    if (l < 0) { cur = 0; return; }
    int tl = total_lines();
    if (l >= tl) { cur = tlen; return; }
    int ls = offset_of_line(l), le = line_end(ls);
    int w = le - ls;
    cur = ls + (col < w ? col : w);
}

static void on_key(const event_t *e) {
    if (e->mods & MOD_CTRL) {
        char c = e->ch;
        if (c=='s'||c=='S') { save_file(); return; }
        if (c=='c'||c=='C') { copy_line(0); return; }
        if (c=='x'||c=='X') { copy_line(1); return; }
        if (c=='v'||c=='V') { paste(); return; }
        if (c=='a'||c=='A') { cur = 0; return; }
        if (c=='e'||c=='E') { cur = tlen; return; }
    }
    switch (e->key) {
        case KEY_LEFT:  if (cur>0) cur--; break;
        case KEY_RIGHT: if (cur<tlen) cur++; break;
        case KEY_UP:    move_vert(-1); break;
        case KEY_DOWN:  move_vert(1); break;
        case KEY_HOME:  cur = line_start(cur); break;
        case KEY_END:   cur = line_end(cur); break;
        case KEY_PAGEUP:   for (int i=0;i<rows_visible();i++) move_vert(-1); break;
        case KEY_PAGEDOWN: for (int i=0;i<rows_visible();i++) move_vert(1); break;
        case KEY_ENTER: insert_ch('\n'); break;
        case KEY_TAB:   insert_ch('\t'); break;
        case KEY_BACKSPACE: backspace(); break;
        case KEY_DELETE: del_fwd(); break;
        default: if (e->ch >= ' ' && !(e->mods & MOD_CTRL)) insert_ch(e->ch); break;
    }
    ensure_visible();
}

static void on_mouse(const event_t *e) {
    if (!(e->buttons & MOUSE_LEFT)) return;
    if (e->my < TOPH) {
        if (e->mx >= 4 && e->mx < 68) save_file();
        else if (e->mx >= 74 && e->mx < 138) { tlen=0; cur=0; top=0; dirty=0; set_status("nouveau"); }
        return;
    }
    int ln = top + (e->my - TOPH) / LH;
    int col = (e->mx - GUTTER) / CW; if (col < 0) col = 0;
    int tl = total_lines(); if (ln >= tl) ln = tl-1; if (ln<0) ln=0;
    int ls = offset_of_line(ln), le = line_end(ls), w = le - ls;
    cur = ls + (col < w ? col : w);
}

int main(void) {
    sys_arg_get(path, sizeof path);
    if (!path[0]) strcpy(path, "/home/user/sans-titre.txt");

    win_set_resizable(1);
    cv = win_create(620, 440, "Editeur de texte");
    if (!cv) return 1;
    load_file();
    redraw();

    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (r == 2) { ensure_visible(); redraw(); continue; }
        if (r == 1) {
            if (e.type == EV_KEY && e.pressed) on_key(&e);
            else if (e.type == EV_MOUSE) on_mouse(&e);
            redraw();
        }
    }
}
