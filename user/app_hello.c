// =============================================================================
//  user/app_hello.c -- Application RING 3 (processus séparé) : suit la souris.
//  Prouve le routage des entrées du compositeur vers la fenêtre au focus.
// =============================================================================
#include "monos.h"
#include "libwin.h"
#include "gfx.h"

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

int main(void) {
    canvas_t *c = win_create(320, 180, "Bonjour (processus)");
    if (!c) return 1;
    int bx = 160, by = 110;
    for (;;) {
        event_t e; int r = win_poll(&e);
        if (r < 0) sys_exit(0);
        if (r == 1 && e.type == EV_MOUSE) { bx = e.mx; by = e.my; }
        canvas_fill(c, rgb(0x18, 0x20, 0x2c));
        canvas_draw_string(c, "Processus ring 3 distinct,", 12, 14, rgb(255, 255, 255), 1);
        canvas_draw_string(c, "relie au compositeur par IPC.", 12, 30, rgb(0xc8, 0xc8, 0xc8), 1);
        canvas_draw_string(c, "Bougez la souris ici :", 12, 50, rgb(0xc8, 0xc8, 0xc8), 1);
        if (bx >= 0 && by >= 0) canvas_fill_rect(c, bx - 8, by - 8, 16, 16, rgb(0xff, 0xd0, 0x40));
        win_damage();
        for (volatile int k = 0; k < 200000; k++) {}
    }
}
