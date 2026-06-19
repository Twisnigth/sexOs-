// =============================================================================
//  user/gfxdemo.c -- Démonstration graphique RING 3 (refactor Phase 2)
// -----------------------------------------------------------------------------
//  Prouve qu'un programme en ring 3 (CPL 3) peut, via des appels système :
//    - mapper le framebuffer (sys_fb_map) et y écrire des pixels,
//    - lire les événements clavier/souris (sys_input_poll),
//    - lire l'horloge (sys_time_ms).
//  Aucun accès matériel direct : tout passe par le noyau.
// =============================================================================
#include "monos.h"

int main(void) {
    // Preuve sur le port série : exécution en ring 3 (CPL 3) via syscall.
    char msg[] = "gfx: ring3 ok, cpl=X\n";
    int cpl = sys_get_cpl();
    msg[19] = (char)('0' + (cpl & 7));
    sys_write(msg, sizeof(msg) - 1);

    fbinfo_t fb;
    if (sys_fb_map(&fb)) return 1;
    uint32_t *px = (uint32_t *)(uintptr_t)fb.addr;
    uint32_t pp = fb.pitch / 4;

    // Dégradé plein écran (écrit depuis le ring 3).
    for (uint32_t y = 0; y < fb.height; y++)
        for (uint32_t x = 0; x < fb.width; x++)
            px[y * pp + x] = ((x * 255 / fb.width) << 16) |
                             ((y * 255 / fb.height) << 8) | 0x50;

    int bx = fb.width / 2, by = fb.height / 2;
    // Démo bornée (~quelques secondes) ; suit la souris (preuve entrée ring 3),
    // sort tôt sur Échap. Le minuteur n'est pas requis : on compte les trames.
    for (int frame = 0; frame < 1200; frame++) {
        event_t e;
        while (sys_input_poll(&e)) {
            if (e.type == EV_MOUSE) { bx = e.mx; by = e.my; }
            if (e.type == EV_KEY && e.pressed && e.key == KEY_ESC) return 0;
        }
        for (int dy = -15; dy < 15; dy++)
            for (int dx = -15; dx < 15; dx++) {
                int x = bx + dx, y = by + dy;
                if (x >= 0 && y >= 0 && x < (int)fb.width && y < (int)fb.height)
                    px[y * pp + x] = 0xFFFFFF;
            }
        for (volatile uint64_t d = 0; d < 3000000ULL; d++) { }   // petite cadence
    }
    return 0;
}
