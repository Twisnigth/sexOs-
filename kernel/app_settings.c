// =============================================================================
//  kernel/app_settings.c -- Application Paramètres
// =============================================================================
#include "apps.h"
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"
#include "users.h"
#include "rtc.h"
#include "pmm.h"
#include "pit.h"
#include "pci.h"
#include "boot.h"

static const char *sections[] = { "Systeme", "Affichage", "Date/heure", "Utilisateurs" };
#define NSEC 4

typedef struct { int section; char status[80]; } settings_t;

static void put(canvas_t *c, int x, int *y, const char *s, uint32_t col) {
    canvas_draw_string(c, s, x, *y, col, 1);
    *y += 18;
}

static void paint_systeme(canvas_t *c, int x, int y) {
    uint32_t w = fb_rgb(0xff,0xff,0xff), g = fb_rgb(0x9a,0xc8,0xff);
    char b[96], num[24];
    put(c, x, &y, "Informations systeme", g);
    put(c, x, &y, "MonOS version 2.0 (x86_64)", w);
    put(c, x, &y, "Demarrage : UEFI/BIOS via Limine", w);
    strcpy(b, "Memoire totale : "); utoa(pmm_total_bytes()/(1024*1024), num, 10); strcat(b, num); strcat(b, " Mio");
    put(c, x, &y, b, w);
    strcpy(b, "Memoire utilisee : "); utoa(pmm_used_bytes()/(1024*1024), num, 10); strcat(b, num); strcat(b, " Mio");
    put(c, x, &y, b, w);
    strcpy(b, "Temps de fonctionnement : "); utoa(pit_ms()/1000, num, 10); strcat(b, num); strcat(b, " s");
    put(c, x, &y, b, w);
    strcpy(b, "Peripheriques PCI : "); utoa(pci_device_count(), num, 10); strcat(b, num);
    put(c, x, &y, b, w);
}

static void paint_affichage(canvas_t *c, int x, int y) {
    uint32_t w = fb_rgb(0xff,0xff,0xff), g = fb_rgb(0x9a,0xc8,0xff);
    char b[96], num[24];
    put(c, x, &y, "Affichage", g);
    strcpy(b, "Resolution actuelle : "); utoa(fb_width(), num, 10); strcat(b, num);
    strcat(b, " x "); utoa(fb_height(), num, 10); strcat(b, num);
    put(c, x, &y, b, w);

    struct limine_framebuffer *lfb = boot_framebuffer();
    if (lfb && lfb->mode_count > 0) {
        strcpy(b, "Modes GOP disponibles : "); utoa(lfb->mode_count, num, 10); strcat(b, num);
        put(c, x, &y, b, w);
        uint64_t shown = lfb->mode_count < 8 ? lfb->mode_count : 8;
        for (uint64_t i = 0; i < shown; i++) {
            struct limine_video_mode *m = lfb->modes[i];
            strcpy(b, "  "); utoa(m->width, num, 10); strcat(b, num);
            strcat(b, "x"); utoa(m->height, num, 10); strcat(b, num);
            strcat(b, "x"); utoa(m->bpp, num, 10); strcat(b, num);
            put(c, x, &y, b, fb_rgb(0xc8,0xc8,0xc8));
        }
    }
    put(c, x, &y, "(changement de mode a chaud non supporte)", fb_rgb(0x88,0x88,0x88));
}

static void paint_datetime(canvas_t *c, int x, int y) {
    uint32_t w = fb_rgb(0xff,0xff,0xff), g = fb_rgb(0x9a,0xc8,0xff);
    char buf[32]; rtc_time_t t; rtc_now(&t); rtc_format(&t, buf);
    put(c, x, &y, "Date et heure (horloge materielle)", g);
    put(c, x, &y, buf, w);
}

static void paint_users(canvas_t *c, int x, int y, settings_t *s) {
    uint32_t w = fb_rgb(0xff,0xff,0xff), g = fb_rgb(0x9a,0xc8,0xff);
    put(c, x, &y, "Comptes utilisateurs", g);
    for (int i = 0; i < users_count(); i++) {
        const user_t *u = users_get(i);
        char b[80]; strcpy(b, " - "); strcat(b, u->name);
        strcat(b, u->is_admin ? "  [administrateur]" : "  [standard]");
        if (u == users_current()) strcat(b, "  <- connecte");
        put(c, x, &y, b, w);
    }
    y += 8;
    // Action reservee a l'admin (demonstration de la separation des privileges).
    canvas_fill_rect(c, x, y, 240, 24, users_can_admin() ? fb_rgb(0x2d,0x6c,0xdf) : fb_rgb(0x55,0x55,0x55));
    canvas_draw_string(c, "Action systeme (admin)", x + 8, y + 4, fb_rgb(0xff,0xff,0xff), 1);
    y += 30;
    put(c, x, &y, s->status, fb_rgb(0xff,0xcc,0x66));
}

static void settings_paint(window_t *win) {
    settings_t *s = (settings_t *)win->user;
    canvas_t *c = &win->canvas;
    canvas_fill(c, fb_rgb(0x20, 0x23, 0x2d));

    // Panneau de gauche : sections.
    canvas_fill_rect(c, 0, 0, 140, c->height, fb_rgb(0x18, 0x1a, 0x22));
    for (int i = 0; i < NSEC; i++) {
        int y = 10 + i * 30;
        if (i == s->section) canvas_fill_rect(c, 0, y - 4, 140, 26, fb_rgb(0x2d,0x6c,0xdf));
        canvas_draw_string(c, sections[i], 12, y, fb_rgb(0xff,0xff,0xff), 1);
    }

    int x = 158, y = 14;
    switch (s->section) {
        case 0: paint_systeme(c, x, y); break;
        case 1: paint_affichage(c, x, y); break;
        case 2: paint_datetime(c, x, y); break;
        case 3: paint_users(c, x, y, s); break;
    }
}

static void settings_event(window_t *win, const event_t *e, int cx, int cy) {
    settings_t *s = (settings_t *)win->user;
    if (e->type == EV_MOUSE && (e->buttons & MOUSE_LEFT)) {
        if (cx < 140) {
            int i = (cy - 6) / 30;
            if (i >= 0 && i < NSEC) { s->section = i; win->dirty = true; }
        } else if (s->section == 3) {
            // Clic sur le bouton "action systeme".
            // (position approximative du bouton calculée comme dans paint_users)
            if (users_can_admin()) strncpy(s->status, "Action systeme executee (admin).", sizeof(s->status)-1);
            else strncpy(s->status, "Refuse : reserve a l'administrateur.", sizeof(s->status)-1);
            win->dirty = true;
        }
    }
}

void app_settings_open(void) {
    window_t *win = wm_create("Parametres", 220, 130, 560, 360);
    if (!win) return;
    settings_t *s = (settings_t *)kcalloc(1, sizeof(settings_t));
    win->user = s;
    win->on_paint = settings_paint;
    win->on_event = settings_event;
    win->dirty = true;
}
