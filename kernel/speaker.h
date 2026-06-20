// =============================================================================
//  kernel/speaker.h -- Haut-parleur PC (canal 2 du PIT 8254 + port 0x61)
// =============================================================================
#ifndef SEXOS_SPEAKER_H
#define SEXOS_SPEAKER_H

#include <stdint.h>

void speaker_on(uint32_t freq);          // emet une frequence (Hz) en continu
void speaker_off(void);                   // coupe le son
void beep(uint32_t freq, uint32_t ms);    // bip : frequence pendant ms (bloquant)
void speaker_jingle(void);                // petit jingle de demarrage

#endif
