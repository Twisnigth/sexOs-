// =============================================================================
//  user/files.c -- EXPLORATEUR de fichiers en PROCESSUS ring 3 séparé.
// -----------------------------------------------------------------------------
//  Client du compositeur (fenêtre via libwin). Le système de fichiers est
//  manipulé UNIQUEMENT par appels système (sys_vfs_*) : aucun pointeur noyau.
//  Gestion de fichiers : copier / couper / coller / renommer / supprimer,
//  y compris entre volumes (/home, /media/usb, /media/disk). Les opérations
//  sont réalisées en espace utilisateur (lecture+écriture+création+suppression),
//  ce qui déclenche la persistance noyau sur le bon volume.
// =============================================================================
#include "sexos.h"
#include "libwin.h"
#include "gfx.h"
#include "input.h"

void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);
char *strcpy(char *, const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, unsigned long);
char *strcat(char *, const char *);
void *malloc(unsigned long);
void  free(void *);

#define W       640
#define H       440
#define ROW_H   20
#define MAXENT  256

static canvas_t *cv;
static char      cwd[256];
static int       sel;
static int       count;                 // nombre d'entrées listées
static dirent_t  ents[MAXENT];
static int       mode;                   // 0=normal, 1=nouveau dossier, 2=nouveau fichier, 3=renommer
static char      input[64];
static int       ilen;
static char      status[96];
static char      ren_src[64];            // nom d'origine lors d'un renommage
static char      clip_path[256];         // source pour copier/couper
static int       clip_cut;               // 1 = couper (déplacer), 0 = copier
static int       g_toolbar_h = 30, g_list_y = 50;

static const char *toolbar[] = {
    "Haut", "Ouvrir", "NvDossier", "NvFichier",
    "Copier", "Couper", "Coller", "Renommer", "Suppr", "Actualiser", "CleUSB"
};
#define NBTN (int)(sizeof(toolbar)/sizeof(toolbar[0]))

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

static void set_status(const char *s) { int i=0; while (s[i] && i<95) { status[i]=s[i]; i++; } status[i]=0; }
static const char *base_of(const char *p) { const char *b=p; for (const char *q=p;*q;q++) if (*q=='/') b=q+1; return b; }

static void reload(void) {
    count = 0;
    dirent_t e;
    for (int i = 0; count < MAXENT && sys_vfs_list(cwd, i, &e) == 1; i++) ents[count++] = e;
    if (sel >= count) sel = count ? count - 1 : 0;
}

// Construit cwd + nom -> out (chemin absolu).
static void join(const char *name, char *out) {
    strcpy(out, cwd);
    if (strcmp(cwd, "/") != 0) strcat(out, "/");
    strcat(out, name);
}
// Construit dir + "/" + name -> out.
static void join2(const char *dir, const char *name, char *out) {
    strcpy(out, dir);
    if (strcmp(dir, "/") != 0) strcat(out, "/");
    strcat(out, name);
}

static void go_up(void) {
    if (strcmp(cwd, "/") == 0) return;
    int l = strlen(cwd);
    while (l > 1 && cwd[l-1] != '/') l--;
    if (l > 1) l--;
    cwd[l ? l : 1] = 0; if (!l) { cwd[0] = '/'; cwd[1] = 0; }
    sel = 0; reload();
}

static int has_ext(const char *name, const char *suf) {
    int ls = (int)strlen(name), lf = (int)strlen(suf);
    if (ls < lf) return 0;
    for (int i = 0; i < lf; i++) { char a = name[ls-lf+i], b = suf[i]; if (a>='A'&&a<='Z') a+=32; if (a!=b) return 0; }
    return 1;
}
static int is_image(const char *n) {
    return has_ext(n,".png")||has_ext(n,".jpg")||has_ext(n,".jpeg")||has_ext(n,".bmp")||has_ext(n,".ppm");
}

static void open_selected(void) {
    if (sel < 0 || sel >= count) return;
    dirent_t *e = &ents[sel];
    char path[256]; join(e->name, path);
    if (e->type == 1) { strcpy(cwd, path); sel = 0; reload(); set_status(""); return; }
    if (is_image(e->name)) { sys_arg_set(path); win_launch(APP_IMGVIEW); set_status("ouvre dans la visionneuse"); return; }
    sys_arg_set(path); win_launch(APP_EDITOR); set_status("ouvre dans l'editeur");
}

// --- Opérations de fichiers (récursives, en espace utilisateur) ---------------
static int copy_file(const char *src, const char *dst, long size) {
    char *buf = malloc(size > 0 ? (unsigned long)size : 1);
    if (!buf) return -1;
    vfs_io_t in = { src, 0, buf, (uint64_t)size };
    long n = sys_vfs_read(&in);
    if (n < 0) { free(buf); return -1; }
    sys_vfs_create(dst, 0);                       // crée (ignore si déjà là)
    vfs_io_t out = { dst, 0, buf, (uint64_t)n };
    long w = sys_vfs_save(&out);
    free(buf);
    return w < 0 ? -1 : 0;
}
static int copy_recursive(const char *src, const char *dst) {
    dirent_t st;
    if (sys_vfs_stat(src, &st) != 0) return -1;
    if (st.type == 0) return copy_file(src, dst, (long)st.size);
    if (sys_vfs_create(dst, 1) != 0) { dirent_t e; if (sys_vfs_stat(dst, &e) != 0) return -1; }
    dirent_t e;
    for (int i = 0; sys_vfs_list(src, i, &e) == 1; i++) {
        char s2[256], d2[256];
        join2(src, e.name, s2); join2(dst, e.name, d2);
        copy_recursive(s2, d2);
    }
    return 0;
}
static void delete_recursive(const char *path) {
    dirent_t st;
    if (sys_vfs_stat(path, &st) != 0) return;
    if (st.type == 1) {
        dirent_t e;
        while (sys_vfs_list(path, 0, &e) == 1) {     // supprime toujours le 1er enfant
            char c[256]; join2(path, e.name, c);
            delete_recursive(c);
        }
    }
    sys_vfs_delete(path);
}

static void do_copy(int cut) {
    if (sel < 0 || sel >= count) { set_status("rien a selectionner"); return; }
    join(ents[sel].name, clip_path); clip_cut = cut;
    set_status(cut ? "coupe (Coller pour deplacer)" : "copie (Coller pour coller)");
}
static void do_paste(void) {
    if (!clip_path[0]) { set_status("presse-papiers vide"); return; }
    char dst[256]; join2(cwd, base_of(clip_path), dst);
    int n = (int)strlen(clip_path);
    if (strncmp(dst, clip_path, (unsigned long)n) == 0 && dst[n] == '/') {
        set_status("destination dans la source"); return;   // dans un sous-dossier de la source
    }
    dirent_t e;
    if (sys_vfs_stat(dst, &e) == 0) {              // collision : suffixe "-copie"
        if (clip_cut) { set_status("nom deja present ici"); return; }
        strcat(dst, "-copie");
        if (sys_vfs_stat(dst, &e) == 0) { set_status("copie deja presente"); return; }
    }
    if (copy_recursive(clip_path, dst) != 0) { set_status("echec de la copie"); return; }
    if (clip_cut) { delete_recursive(clip_path); clip_path[0] = 0; set_status("deplace"); }
    else set_status("colle");
    reload();
}
static void start_rename(void) {
    if (sel < 0 || sel >= count) { set_status("rien a renommer"); return; }
    mode = 3; strcpy(ren_src, ents[sel].name);
    strcpy(input, ents[sel].name); ilen = (int)strlen(input);
    set_status("Nouveau nom puis Entree");
}

static void do_delete(void) {
    if (sel < 0 || sel >= count) return;
    char path[256]; join(ents[sel].name, path);
    delete_recursive(path);
    set_status("supprime"); reload();
}

static void toolbar_action(int b) {
    switch (b) {
        case 0: go_up(); break;
        case 1: open_selected(); break;
        case 2: mode = 1; ilen = 0; input[0] = 0; set_status("Nom du dossier puis Entree"); break;
        case 3: mode = 2; ilen = 0; input[0] = 0; set_status("Nom du fichier puis Entree"); break;
        case 4: do_copy(0); break;
        case 5: do_copy(1); break;
        case 6: do_paste(); break;
        case 7: start_rename(); break;
        case 8: do_delete(); break;
        case 9: reload(); set_status("actualise"); break;
        case 10: {                                 // raccourci vers la cle USB
            dirent_t e;
            if (sys_vfs_stat("/media/usb", &e) == 0) {
                strcpy(cwd, "/media/usb"); sel = 0; reload(); set_status("cle USB");
            } else set_status("aucune cle USB montee");
            break;
        }
    }
}

// --- Barre d'outils : mesure (draw=0), dessine (draw=1) ou teste un clic -------
//  Les boutons passent à la ligne quand ils dépassent la largeur de la fenêtre.
static int toolbar_layout(int hit_x, int hit_y, int draw) {
    int x = 6, y = 4, h = 22, rowh = 26, maxw = (int)cv->width - 6, hit = -1;
    for (int i = 0; i < NBTN; i++) {
        int w = (int)strlen(toolbar[i]) * 8 + 12;
        if (x + w > maxw && x > 6) { x = 6; y += rowh; }
        if (draw) {
            int on = (i == 6 && clip_path[0]) ? 1 : 0;   // « Coller » mis en avant si dispo
            canvas_fill_rect(cv, x, y, w, h, on ? rgb(0x2d,0x6c,0xdf) : rgb(0x3a,0x40,0x52));
            canvas_draw_string(cv, toolbar[i], x + 6, y + 4, rgb(0xff,0xff,0xff), 1);
        } else if (hit_x >= 0 && hit_x >= x && hit_x < x + w && hit_y >= y && hit_y < y + h) {
            hit = i;
        }
        x += w + 5;
    }
    g_toolbar_h = y + h + 4;
    g_list_y = g_toolbar_h + 18;
    return hit;
}

static void redraw(void) {
    canvas_fill(cv, rgb(0x24, 0x27, 0x31));
    toolbar_layout(-1, -1, 0);                              // mesure g_toolbar_h
    canvas_fill_rect(cv, 0, 0, cv->width, g_toolbar_h, rgb(0x2c, 0x30, 0x3e));
    toolbar_layout(-1, -1, 1);                              // dessine les boutons

    // Barre de chemin.
    canvas_fill_rect(cv, 0, g_toolbar_h, cv->width, 18, rgb(0x1b, 0x1e, 0x27));
    canvas_draw_string(cv, cwd, 6, g_toolbar_h + 1, rgb(0x9a, 0xd0, 0xff), 1);

    // Liste des entrées.
    for (int i = 0; i < count; i++) {
        int y = g_list_y + i * ROW_H;
        if (y + ROW_H > (int)cv->height - 20) break;
        if (i == sel) canvas_fill_rect(cv, 0, y, cv->width, ROW_H, rgb(0x2d, 0x6c, 0xdf));
        uint32_t icon = (ents[i].type == 1) ? rgb(0xe0, 0xc4, 0x4f) : rgb(0x9a, 0xa0, 0xb4);
        canvas_fill_rect(cv, 8, y + 4, 12, 12, icon);
        canvas_draw_string(cv, ents[i].name, 28, y + 2, rgb(0xff, 0xff, 0xff), 1);
        if (ents[i].type == 1) canvas_draw_string(cv, "<dossier>", cv->width - 90, y + 2, rgb(0xc8, 0xc8, 0xc8), 1);
    }

    // Barre du bas : saisie ou statut.
    if (mode) {
        const char *lbl = mode == 1 ? "Dossier: " : mode == 2 ? "Fichier: " : "Renommer: ";
        canvas_fill_rect(cv, 0, cv->height - 20, cv->width, 20, rgb(0x3a, 0x2c, 0x52));
        canvas_draw_string(cv, lbl, 4, cv->height - 18, rgb(0xff, 0xff, 0xff), 1);
        canvas_draw_string(cv, input, 4 + 8 * (int)strlen(lbl), cv->height - 18, rgb(0xff, 0xff, 0x99), 1);
    } else {
        canvas_fill_rect(cv, 0, cv->height - 20, cv->width, 20, rgb(0x1b, 0x1e, 0x27));
        canvas_draw_string(cv, status, 6, cv->height - 18, rgb(0x9a, 0xa0, 0xb4), 1);
    }
    win_damage();
}

static void commit_input(void) {
    input[ilen] = 0;
    if (mode == 3) {                                        // renommer
        if (ilen) {
            char src[256], dst[256];
            join2(cwd, ren_src, src); join2(cwd, input, dst);
            dirent_t e;
            if (strcmp(src, dst) == 0) set_status("nom inchange");
            else if (sys_vfs_stat(dst, &e) == 0) set_status("nom deja pris");
            else if (copy_recursive(src, dst) == 0) { delete_recursive(src); set_status("renomme"); reload(); }
            else set_status("echec renommage");
        }
        mode = 0; return;
    }
    if (ilen) {                                             // nouveau dossier / fichier
        char path[256]; join(input, path);
        if (sys_vfs_create(path, mode == 1 ? 1 : 0) != 0) set_status("creation refusee");
        else { set_status("cree"); reload(); }
    }
    mode = 0;
}

static void on_key(const event_t *e) {
    if (mode) {
        if (e->key == KEY_ENTER) commit_input();
        else if (e->key == KEY_BACKSPACE) { if (ilen > 0) input[--ilen] = 0; }
        else if (e->key == KEY_ESC) { mode = 0; set_status("annule"); }
        else if (e->ch && ilen < (int)sizeof(input) - 1) { input[ilen++] = e->ch; input[ilen] = 0; }
        return;
    }
    if (e->mods & MOD_CTRL) {
        char c = e->ch;
        if (c=='c'||c=='C') { do_copy(0); return; }
        if (c=='x'||c=='X') { do_copy(1); return; }
        if (c=='v'||c=='V') { do_paste(); return; }
        if (c=='r'||c=='R') { start_rename(); return; }
    }
    if (e->key == KEY_UP)        { if (sel > 0) sel--; }
    else if (e->key == KEY_DOWN) { if (sel < count - 1) sel++; }
    else if (e->key == KEY_ENTER) open_selected();
    else if (e->key == KEY_DELETE) do_delete();
    else if (e->key == KEY_BACKSPACE) go_up();
}

static void on_mouse(const event_t *e) {
    if (!(e->buttons & MOUSE_LEFT)) return;
    int cx = e->mx, cy = e->my;
    if (cy < g_toolbar_h) { int b = toolbar_layout(cx, cy, 0); if (b >= 0) toolbar_action(b); return; }
    if (cy >= g_list_y) {
        int idx = (cy - g_list_y) / ROW_H;
        if (idx >= 0 && idx < count) {
            if (idx == sel) open_selected();      // 2e clic = ouvrir
            else sel = idx;
        }
    }
}

int main(void) {
    win_set_resizable(1);
    cv = win_create(W, H, "Explorateur");
    if (!cv) return 1;
    strcpy(cwd, "/home/user");
    dirent_t e; if (sys_vfs_stat(cwd, &e) != 0) strcpy(cwd, "/");
    set_status("");
    reload();
    redraw();

    for (;;) {
        event_t ev; int r = win_wait(&ev);
        if (r < 0) sys_exit(0);
        if (r == 1) {
            if (ev.type == EV_KEY) { if (ev.pressed) on_key(&ev); }
            else if (ev.type == EV_MOUSE) on_mouse(&ev);
        }
        redraw();   // redessine aussi sur redimensionnement (r==2)
    }
}
