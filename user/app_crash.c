// =============================================================================
//  user/app_crash.c -- Application RING 3 (processus séparé) qui PLANTE.
//  Après quelques secondes, déréférence un pointeur nul -> #PF. Le noyau tue ce
//  PROCESSUS ; le compositeur et les autres applications continuent (isolation).
// =============================================================================
#include "monos.h"
#include "libwin.h"
#include "gfx.h"

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

int main(void) {
    canvas_t *c = win_create(280, 110, "Crash test");
    if (!c) return 1;
    int frame = 0;
    canvas_fill(c, rgb(0x30, 0x14, 0x14));
    canvas_draw_string(c, "Ce processus va planter", 12, 16, rgb(255, 255, 255), 1);
    canvas_draw_string(c, "(dereferencement de NULL).", 12, 34, rgb(255, 255, 255), 1);
    canvas_draw_string(c, "Le compositeur survivra.", 12, 58, rgb(0xff, 0xc0, 0xc0), 1);
    win_damage();
    for (;;) {
        event_t e; int r;
        while ((r = win_poll(&e)) != 0) { if (r < 0) sys_exit(0); }
        if (++frame > 400) { *(volatile int *)0 = 42; }   // #PF -> tâche tuée
        sys_yield();
    }
}
