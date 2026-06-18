// =============================================================================
//  kernel/desktop.c -- Bureau : compositeur, écran de connexion, barre des
//  tâches, lanceur d'applications et boucle d'événements principale.
// =============================================================================
#include "desktop.h"
#include "window.h"
#include "framebuffer.h"
#include "gfx.h"
#include "heap.h"
#include "klib.h"
#include "input.h"
#include "ps2.h"
#include "pit.h"
#include "rtc.h"
#include "users.h"
#include "apps.h"
#include "ssh.h"
#include "io.h"

#define TASKBAR_H 36

static canvas_t back;         // back-buffer (double buffering)
static int cursor_x, cursor_y;
static bool menu_open;

// --- Curseur souris (bitmap : X=bordure, .=remplissage, espace=transparent) --
static const char *cursor_bmp[] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X.....XXXXX",
    "X..X..X    ",
    "X.X X..X   ",
    "XX  X..X   ",
    "     X..X  ",
    "      XX   ",
};

static void draw_cursor(canvas_t *c, int x, int y) {
    uint32_t border = fb_rgb(0x00,0x00,0x00);
    uint32_t fill   = fb_rgb(0xff,0xff,0xff);
    for (int row = 0; row < 16; row++)
        for (int col = 0; cursor_bmp[row][col]; col++) {
            char ch = cursor_bmp[row][col];
            if (ch == 'X') canvas_put_pixel(c, x + col, y + row, border);
            else if (ch == '.') canvas_put_pixel(c, x + col, y + row, fill);
        }
}

// --- Fond d'écran ------------------------------------------------------------
static void draw_wallpaper(canvas_t *c) {
    // Dégradé vertical bleu nuit.
    for (uint32_t y = 0; y < c->height; y++) {
        uint8_t t = (uint8_t)(0x10 + (y * 0x20) / c->height);
        uint32_t col = fb_rgb(t, t, (uint8_t)(t + 0x18));
        canvas_fill_rect(c, 0, y, c->width, 1, col);
    }
    // Mascotte discrète en filigrane, coin bas-droit.
    const char *m[] = { "D", "|", "|", "8" };
    for (int i = 0; i < 4; i++)
        canvas_draw_string(c, m[i], c->width - 60, c->height - 200 + i * 32,
                           fb_rgb(0x33, 0x3a, 0x4f), 3);
}

// --- Barre des tâches + lanceur ----------------------------------------------
static const char *menu_items[] = {
    "Terminal", "Explorateur de fichiers", "Parametres", "A propos", "Se deconnecter"
};
#define NMENU 5
static bool logout_requested;

static void launch(int item) {
    switch (item) {
        case 0: app_terminal_open(); break;
        case 1: app_files_open(); break;
        case 2: app_settings_open(); break;
        case 3: app_about_open(); break;
        case 4: logout_requested = true; break;
    }
}

static void draw_taskbar(canvas_t *c) {
    int ty = c->height - TASKBAR_H;
    canvas_fill_rect(c, 0, ty, c->width, TASKBAR_H, fb_rgb(0x18, 0x1b, 0x26));
    canvas_draw_hline(c, 0, ty, c->width, fb_rgb(0x2d, 0x6c, 0xdf));

    // Bouton Menu.
    canvas_fill_rect(c, 4, ty + 4, 76, TASKBAR_H - 8, fb_rgb(0x2d, 0x6c, 0xdf));
    canvas_draw_string(c, "Menu", 22, ty + 11, fb_rgb(0xff,0xff,0xff), 1);

    // Boutons des fenêtres ouvertes.
    int x = 88;
    for (int i = 0; i < wm_count() && x < (int)c->width - 200; i++) {
        window_t *w = wm_get(i);
        uint32_t col = w->minimized ? fb_rgb(0x2a,0x2e,0x3c)
                     : (w->focused ? fb_rgb(0x3a,0x42,0x58) : fb_rgb(0x24,0x28,0x34));
        canvas_fill_rect(c, x, ty + 5, 150, TASKBAR_H - 10, col);
        char label[20]; strncpy(label, w->title, 17); label[17] = 0;
        canvas_draw_string(c, label, x + 6, ty + 11, fb_rgb(0xff,0xff,0xff), 1);
        x += 156;
    }

    // Horloge.
    rtc_time_t t; rtc_now(&t);
    char clk[16];
    clk[0]='0'+t.hour/10; clk[1]='0'+t.hour%10; clk[2]=':';
    clk[3]='0'+t.minute/10; clk[4]='0'+t.minute%10; clk[5]=':';
    clk[6]='0'+t.second/10; clk[7]='0'+t.second%10; clk[8]=0;
    canvas_draw_string(c, clk, c->width - 80, ty + 11, fb_rgb(0xff,0xff,0xff), 1);

    // Utilisateur connecté.
    const user_t *u = users_current();
    if (u) canvas_draw_string(c, u->name, c->width - 80, ty + 1, fb_rgb(0x9a,0xc8,0xff), 1);

    // Menu déroulant.
    if (menu_open) {
        int mh = NMENU * 28 + 8;
        int my = ty - mh;
        canvas_fill_rect(c, 4, my, 240, mh, fb_rgb(0x22, 0x26, 0x33));
        canvas_draw_rect(c, 4, my, 240, mh, fb_rgb(0x2d, 0x6c, 0xdf));
        for (int i = 0; i < NMENU; i++)
            canvas_draw_string(c, menu_items[i], 16, my + 8 + i * 28, fb_rgb(0xff,0xff,0xff), 1);
    }
}

// Renvoie true si le clic est consommé par la barre des tâches / le menu.
static bool taskbar_click(canvas_t *c, int mx, int my) {
    int ty = c->height - TASKBAR_H;

    if (menu_open) {
        int mh = NMENU * 28 + 8;
        int myo = ty - mh;
        if (mx >= 4 && mx < 244 && my >= myo && my < myo + mh) {
            int i = (my - myo - 4) / 28;
            if (i >= 0 && i < NMENU) { launch(i); menu_open = false; return true; }
        }
        menu_open = false;   // clic ailleurs : ferme le menu
    }

    if (my < ty) return false;          // au-dessus de la barre

    if (mx >= 4 && mx < 80) { menu_open = !menu_open; return true; }   // bouton Menu

    // Boutons des fenêtres.
    int x = 88;
    for (int i = 0; i < wm_count() && x < (int)c->width - 200; i++) {
        window_t *w = wm_get(i);
        if (mx >= x && mx < x + 150) {
            if (w->minimized) { w->minimized = false; wm_focus(w); }
            else if (w->focused) { w->minimized = true; }
            else wm_focus(w);
            return true;
        }
        x += 156;
    }
    return true;   // clic sur la barre (zone vide) : consommé
}

// --- Présentation (composition d'une image complète) -------------------------
static void present(void) {
    draw_wallpaper(&back);
    wm_draw_all(&back);
    draw_taskbar(&back);
    draw_cursor(&back, cursor_x, cursor_y);
    canvas_blit(fb_canvas(), &back, 0, 0);
}

// --- Écran de connexion ------------------------------------------------------
static bool login_screen(void) {
    char username[USER_NAME_MAX] = "user";
    char password[USER_NAME_MAX] = "";
    int  ulen = 4, plen = 0;
    int  field = 0;            // 0 = utilisateur, 1 = mot de passe
    const char *error = "";

    for (;;) {
        // --- Rendu ---
        canvas_fill(&back, fb_rgb(0x0e, 0x10, 0x1c));
        int cx = back.width / 2;
        canvas_draw_string(&back, "MonOS", cx - canvas_text_width("MonOS", 5)/2, 80, fb_rgb(0xff,0xff,0xff), 5);
        // Mascotte.
        const char *m[] = { "D", "|", "|", "8" };
        for (int i = 0; i < 4; i++)
            canvas_draw_string(&back, m[i], cx - 9, 170 + i * 28, fb_rgb(0xff,0x7a,0xb0), 3);

        int bx = cx - 160, by = 300;
        canvas_fill_rect(&back, bx, by, 320, 170, fb_rgb(0x1c, 0x20, 0x2c));
        canvas_draw_rect(&back, bx, by, 320, 170, fb_rgb(0x2d, 0x6c, 0xdf));
        canvas_draw_string(&back, "Connexion", bx + 16, by + 12, fb_rgb(0xff,0xff,0xff), 2);

        canvas_draw_string(&back, "Utilisateur:", bx + 16, by + 50, fb_rgb(0xc8,0xc8,0xc8), 1);
        canvas_fill_rect(&back, bx + 120, by + 46, 180, 20, fb_rgb(field==0?0x33:0x28, 0x33, 0x44));
        canvas_draw_string(&back, username, bx + 124, by + 48, fb_rgb(0xff,0xff,0xff), 1);

        canvas_draw_string(&back, "Mot de passe:", bx + 16, by + 80, fb_rgb(0xc8,0xc8,0xc8), 1);
        canvas_fill_rect(&back, bx + 120, by + 76, 180, 20, fb_rgb(field==1?0x33:0x28, 0x33, 0x44));
        char stars[USER_NAME_MAX]; for (int i = 0; i < plen; i++) stars[i] = '*'; stars[plen] = 0;
        canvas_draw_string(&back, stars, bx + 124, by + 78, fb_rgb(0xff,0xff,0xff), 1);

        canvas_draw_string(&back, "Tab: champ suivant   Entree: valider", bx + 16, by + 110, fb_rgb(0x88,0x90,0xa0), 1);
        canvas_draw_string(&back, "(essayez root/root ou user/user)", bx + 16, by + 128, fb_rgb(0x88,0x90,0xa0), 1);
        if (error[0]) canvas_draw_string(&back, error, bx + 16, by + 146, fb_rgb(0xff,0x66,0x66), 1);

        draw_cursor(&back, cursor_x, cursor_y);
        canvas_blit(fb_canvas(), &back, 0, 0);

        // --- Événements ---
        event_t e;
        bool got = false;
        while (input_poll(&e)) {
            got = true;
            if (e.type == EV_MOUSE) { cursor_x = e.mx; cursor_y = e.my; }
            else if (e.type == EV_KEY && e.pressed) {
                if (e.key == KEY_TAB) field ^= 1;
                else if (e.key == KEY_ENTER) {
                    username[ulen] = 0; password[plen] = 0;
                    const user_t *u = users_authenticate(username, password);
                    if (u) { users_set_current(u); return true; }
                    error = "Identifiants invalides."; plen = 0;
                } else if (e.key == KEY_BACKSPACE) {
                    if (field == 0 && ulen > 0) ulen--;
                    if (field == 1 && plen > 0) plen--;
                } else if (e.ch) {
                    if (field == 0 && ulen < USER_NAME_MAX - 1) username[ulen++] = e.ch;
                    if (field == 1 && plen < USER_NAME_MAX - 1) password[plen++] = e.ch;
                }
            }
        }
        sshd_poll();              // sert une éventuelle connexion SSH entrante
        if (!got) __asm__ volatile ("hlt");
    }
}

// --- Boucle principale -------------------------------------------------------
void desktop_run(void) {
    // Alloue le back-buffer (même taille que l'écran).
    back.width  = fb_width();
    back.height = fb_height();
    back.pitch  = fb_width() * 4;
    back.pixels = (uint32_t *)kmalloc((size_t)back.width * back.height * 4);
    if (!back.pixels) { kprintf("[desktop] back-buffer impossible !\n"); for(;;) hlt(); }

    wm_init();
    ps2_mouse_pos(&cursor_x, &cursor_y);

    for (;;) {
        // Écran de connexion.
        if (!login_screen()) continue;
        kprintf("[desktop] connexion : %s\n", users_current()->name);

        // Fenêtre de bienvenue.
        app_about_open();

        // Boucle du bureau jusqu'à déconnexion.
        logout_requested = false;
        menu_open = false;
        uint64_t last_sec = ~0ULL;
        for (;;) {
            event_t e;
            bool changed = false;
            while (input_poll(&e)) {
                changed = true;
                if (e.type == EV_MOUSE) {
                    cursor_x = e.mx; cursor_y = e.my;
                    bool press = (e.buttons & MOUSE_LEFT);
                    if (press && (e.my >= (int)back.height - TASKBAR_H || menu_open)) {
                        taskbar_click(&back, e.mx, e.my);
                    } else {
                        if (press && menu_open) menu_open = false;
                        wm_handle_mouse(&e);
                    }
                } else if (e.type == EV_KEY) {
                    wm_handle_key(&e);
                }
            }

            // Fermeture des fenêtres demandées.
            for (int i = 0; i < wm_count(); i++) {
                window_t *w = wm_get(i);
                if (w->wants_close) { wm_close(w); changed = true; break; }
            }

            // Mise à jour de l'horloge chaque seconde.
            uint64_t sec = pit_ms() / 1000;
            if (sec != last_sec) { last_sec = sec; changed = true; }

            if (logout_requested) break;

            sshd_poll();          // sert une éventuelle connexion SSH entrante
            if (changed) present();
            else __asm__ volatile ("hlt");
        }

        // Déconnexion : ferme toutes les fenêtres.
        while (wm_count() > 0) wm_close(wm_get(0));
        users_set_current(NULL);
    }
}
