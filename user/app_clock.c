// =============================================================================
//  user/app_clock.c -- Application RING 3 (processus séparé) : horloge.
//  Affiche le temps de fonctionnement dans sa fenêtre (tampon partagé).
// =============================================================================
#include "monos.h"
#include "libwin.h"
#include "gfx.h"

void utoa(unsigned long, char *);
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

int main(void) {
    canvas_t *c = win_create(220, 90, "Horloge");
    if (!c) return 1;
    uint64_t last = (uint64_t)-1;
    for (;;) {
        event_t e; if (win_poll(&e) < 0) sys_exit(0);
        uint64_t s = sys_time_ms() / 1000;
        if (s != last) {
            last = s;
            canvas_fill(c, rgb(0x10, 0x14, 0x20));
            char num[24], b[40]; utoa(s, num);
            int i = 0; const char *p = "Uptime: ";
            while (p[i]) { b[i] = p[i]; i++; }
            int j = 0; while (num[j]) b[i++] = num[j++];
            b[i++] = ' '; b[i++] = 's'; b[i] = 0;
            canvas_draw_string(c, b, 14, 34, rgb(0x6e, 0xe7, 0x9a), 2);
            win_damage();
        }
        for (volatile int k = 0; k < 300000; k++) {}
    }
}
