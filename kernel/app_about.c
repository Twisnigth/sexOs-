// =============================================================================
//  kernel/app_about.c -- Fenêtre "À propos" (avec la mascotte)
// =============================================================================
#include "apps.h"
#include "window.h"
#include "framebuffer.h"
#include "heap.h"
#include "klib.h"

static void about_paint(window_t *win) {
    canvas_t *c = &win->canvas;
    uint32_t bg = fb_rgb(0x12, 0x14, 0x22);
    uint32_t white = fb_rgb(0xff,0xff,0xff);
    uint32_t accent = fb_rgb(0x4e,0xc9,0xff);
    uint32_t pink = fb_rgb(0xff,0x7a,0xb0);
    canvas_fill(c, bg);

    int cx = c->width / 2;
    canvas_draw_string(c, "sexOs", cx - canvas_text_width("sexOs", 4)/2, 20, white, 4);
    canvas_draw_string(c, "version 2.0", cx - canvas_text_width("version 2.0", 1)/2, 70, accent, 1);

    // Mascotte verticale.
    const char *m[] = { "D", "|", "|", "|", "8" };
    for (int i = 0; i < 5; i++)
        canvas_draw_string(c, m[i], cx - 12, 100 + i * 32, pink, 3);

    const char *lines[] = {
        "Systeme d'exploitation x86_64 pedagogique.",
        "Noyau en C + assembleur (NASM).",
        "Demarrage UEFI/BIOS via Limine, framebuffer GOP.",
        "Pilotes : PS/2, PCI, RTC. Interface graphique maison.",
    };
    int y = 280;
    for (int i = 0; i < 4; i++) {
        canvas_draw_string(c, lines[i], cx - canvas_text_width(lines[i], 1)/2, y, fb_rgb(0xc8,0xc8,0xc8), 1);
        y += 18;
    }
}

static void about_event(window_t *win, const event_t *e, int cx, int cy) {
    (void)win; (void)e; (void)cx; (void)cy;
}

void app_about_open(void) {
    window_t *win = wm_create("A propos de sexOs", 300, 160, 420, 380);
    if (!win) return;
    win->on_paint = about_paint;
    win->on_event = about_event;
    win->dirty = true;
}
