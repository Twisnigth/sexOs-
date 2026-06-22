// =============================================================================
//  kernel/speaker.c -- Haut-parleur PC
// -----------------------------------------------------------------------------
//  Le canal 2 du PIT (8254) genere un signal carre dont la frequence est
//  audible via le petit haut-parleur de la carte mere. Le port 0x61 (bits 0 et
//  1) relie ce canal au haut-parleur. Le canal 0 (minuteur systeme) n'est pas
//  touche.
// =============================================================================
#include "speaker.h"
#include "io.h"
#include "pit.h"

#define PIT_FREQ 1193182u

void speaker_on(uint32_t freq) {
    if (freq == 0) { speaker_off(); return; }
    uint32_t div = PIT_FREQ / freq;
    outb(0x43, 0xB6);                         // canal 2, acces lo/hi, mode 3 (onde carree)
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t t = inb(0x61);
    if ((t & 3) != 3) outb(0x61, t | 3);      // active la grille + la sortie haut-parleur
}

void speaker_off(void) {
    outb(0x61, inb(0x61) & 0xFC);             // coupe les bits 0 et 1
}

void beep(uint32_t freq, uint32_t ms) {
    if (ms > 3000) ms = 3000;                 // borne de securite
    speaker_on(freq);
    pit_sleep_ms(ms);
    speaker_off();
}

void speaker_jingle(void) {
    // Petit arpege de demarrage : do - mi - sol - do (aigu).
    static const uint16_t notes[] = { 523, 659, 784, 1047 };
    for (int i = 0; i < 4; i++) beep(notes[i], 90);
    speaker_off();
}
