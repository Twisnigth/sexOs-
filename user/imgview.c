// =============================================================================
//  user/imgview.c -- Visionneuse d'images (processus ring 3, client compositeur)
// -----------------------------------------------------------------------------
//  Decode les BMP 24 bits et les PPM (P6). Liste les images trouvees dans
//  quelques dossiers (dont la cle USB) ; affiche l'image selectionnee a l'echelle.
//  Fenetre redimensionnable. Une image de demonstration est generee au depart.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"

void utoa(unsigned long, char *);
unsigned long strlen(const char *);
int strcmp(const char *, const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

#define LISTW 150
#define TOOLH 24
#define MAXF  64
#define MAXW  1280
#define MAXH  1024

static canvas_t *cv;
static uint8_t   filebuf[1 << 20];          // fichier brut (1 Mio max)
static uint32_t  img[MAXW * MAXH];          // pixels decodes
static int       img_w, img_h;              // dimensions de l'image courante
static char      files[MAXF][96];           // chemins complets des images trouvees
static int       nfiles, sel = -1;
static char      status[80];

// --- decodage ----------------------------------------------------------------
static uint32_t le32(const uint8_t *p) { return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

static int decode_bmp(const uint8_t *d, int n) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off = le32(d + 10);
    int w = (int)le32(d + 18), h = (int)le32(d + 22);
    int bpp = d[28] | (d[29] << 8);
    if (bpp != 24 || w <= 0 || w > MAXW) return 0;
    int flip = h > 0; if (h < 0) h = -h;
    if (h <= 0 || h > MAXH) return 0;
    int rowsz = (w * 3 + 3) & ~3;
    for (int y = 0; y < h; y++) {
        int sy = flip ? (h - 1 - y) : y;
        const uint8_t *row = d + off + (uint32_t)sy * rowsz;
        if (row + w*3 > d + n) break;
        for (int x = 0; x < w; x++)
            img[y * w + x] = rgb(row[x*3+2], row[x*3+1], row[x*3+0]);  // BGR -> RGB
    }
    img_w = w; img_h = h; return 1;
}

static int decode_ppm(const uint8_t *d, int n) {
    if (n < 10 || d[0] != 'P' || d[1] != '6') return 0;
    int i = 2, v[3], k = 0;
    while (k < 3 && i < n) {
        while (i < n && (d[i]==' '||d[i]=='\n'||d[i]=='\t'||d[i]=='\r')) i++;
        if (d[i]=='#') { while (i<n && d[i]!='\n') i++; continue; }
        int s = i; while (i<n && d[i]>' ') i++;
        int val = 0; for (int j=s;j<i;j++) val = val*10 + (d[j]-'0'); v[k++] = val;
    }
    i++;  // un seul blanc apres maxval
    int w = v[0], h = v[1];
    if (w <= 0 || w > MAXW || h <= 0 || h > MAXH) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int o = i + (y*w + x)*3;
            if (o+2 >= n) { img_w=w; img_h=y; return 1; }
            img[y*w + x] = rgb(d[o], d[o+1], d[o+2]);
        }
    img_w = w; img_h = h; return 1;
}

static void gen_demo(void) {       // image de demonstration (degrade + damier)
    img_w = 256; img_h = 192;
    for (int y = 0; y < img_h; y++)
        for (int x = 0; x < img_w; x++) {
            uint8_t r = (uint8_t)(x * 255 / img_w), g = (uint8_t)(y * 255 / img_h);
            uint8_t b = ((x>>4) ^ (y>>4)) & 1 ? 0xc0 : 0x40;
            img[y*img_w + x] = rgb(r, g, b);
        }
    strcpy(status, "image de demonstration");
}

static int ends_with(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (ls < lf) return 0;
    for (int i = 0; i < lf; i++) { char a=s[ls-lf+i], b=suf[i]; if (a>='A'&&a<='Z') a+=32; if (a!=b) return 0; }
    return 1;
}

static void scan_dir(const char *dir) {
    dirent_t e;
    for (int i = 0; nfiles < MAXF && sys_vfs_list(dir, i, &e) == 1; i++) {
        if (e.type != 0) continue;
        if (!ends_with(e.name, ".bmp") && !ends_with(e.name, ".ppm")) continue;
        char *p = files[nfiles];
        strcpy(p, dir); if (strcmp(dir, "/") != 0) strcat(p, "/"); strcat(p, e.name);
        nfiles++;
    }
}
static void rescan(void) {
    nfiles = 0;
    scan_dir("/home/user"); scan_dir("/home/user/Images"); scan_dir("/media/usb");
}

static void load(int idx) {
    if (idx < 0 || idx >= nfiles) return;
    vfs_io_t io = { files[idx], 0, filebuf, sizeof(filebuf) };
    long n = sys_vfs_read(&io);
    if (n <= 0) { strcpy(status, "lecture impossible"); return; }
    int ok = decode_bmp(filebuf, (int)n) || decode_ppm(filebuf, (int)n);
    if (!ok) { strcpy(status, "format non reconnu (BMP24/PPM)"); img_w = img_h = 0; return; }
    sel = idx; strcpy(status, "");
}

static const char *basename(const char *p) {
    const char *b = p; for (const char *q = p; *q; q++) if (*q == '/') b = q + 1; return b;
}

static void redraw(void) {
    canvas_fill(cv, rgb(0x1a, 0x1d, 0x26));
    // barre d'outils
    canvas_fill_rect(cv, 0, 0, cv->width, TOOLH, rgb(0x22, 0x26, 0x32));
    canvas_fill_rect(cv, 4, 3, 80, 18, rgb(0x3a, 0x42, 0x58));
    canvas_draw_string(cv, "Actualiser", 8, 8, rgb(0xff,0xff,0xff), 1);
    if (status[0]) canvas_draw_string(cv, status, 92, 8, rgb(0xc8,0xd0,0xdc), 1);
    else if (sel >= 0) canvas_draw_string(cv, basename(files[sel]), 92, 8, rgb(0xc8,0xd0,0xdc), 1);
    // liste des fichiers (gauche)
    canvas_fill_rect(cv, 0, TOOLH, LISTW, cv->height - TOOLH, rgb(0x14, 0x17, 0x1f));
    for (int i = 0; i < nfiles; i++) {
        int y = TOOLH + 4 + i*18;
        if (y + 18 > (int)cv->height) break;
        if (i == sel) canvas_fill_rect(cv, 0, y-2, LISTW, 18, rgb(0x2d, 0x6c, 0xdf));
        canvas_draw_string(cv, basename(files[i]), 6, y, rgb(0xe6,0xec,0xf2), 1);
    }
    if (nfiles == 0) canvas_draw_string(cv, "(aucune image)", 6, TOOLH+8, rgb(0x9a,0xa0,0xb4), 1);
    // zone image (droite), mise a l'echelle pour tenir
    int ax = LISTW + 1, ay = TOOLH + 1;
    int aw = cv->width - ax - 1, ah = cv->height - ay - 1;
    canvas_draw_vline(cv, LISTW, TOOLH, cv->height - TOOLH, rgb(0x3a,0x42,0x58));
    if (img_w > 0 && img_h > 0 && aw > 0 && ah > 0) {
        // facteur d'echelle (entier sur 1000) pour preserver le ratio
        int sxk = aw * 1000 / img_w, syk = ah * 1000 / img_h;
        int k = sxk < syk ? sxk : syk; if (k > 1000) k = 1000;   // pas d'agrandissement > 1x
        int dw = img_w * k / 1000, dh = img_h * k / 1000;
        int ox = ax + (aw - dw)/2, oy = ay + (ah - dh)/2;
        for (int y = 0; y < dh; y++) {
            int sy = y * img_h / dh;
            for (int x = 0; x < dw; x++) {
                int sx = x * img_w / dw;
                canvas_put_pixel(cv, ox + x, oy + y, img[sy*img_w + sx]);
            }
        }
    }
    win_damage();
}

static void on_mouse(const event_t *e) {
    if (!(e->buttons & MOUSE_LEFT)) return;
    if (e->my < TOOLH) { if (e->mx >= 4 && e->mx < 84) { rescan(); redraw(); } return; }
    if (e->mx < LISTW) {
        int idx = (e->my - TOOLH - 4 + 2) / 18;
        if (idx >= 0 && idx < nfiles) { load(idx); redraw(); }
    }
}

int main(void) {
    win_set_resizable(1);
    cv = win_create(520, 380, "Visionneuse d'images");
    if (!cv) return 1;
    gen_demo();
    rescan();
    redraw();
    for (;;) {
        event_t e; int r = win_wait(&e);
        if (r < 0) sys_exit(0);
        if (r == 2) { redraw(); continue; }            // redimensionnement
        if (r == 1 && e.type == EV_MOUSE) on_mouse(&e);
    }
}
