// =============================================================================
//  kernel/pit.h -- Minuteur programmable (PIT 8254), IRQ0
// =============================================================================
#ifndef MONOS_PIT_H
#define MONOS_PIT_H

#include <stdint.h>

void     pit_init(uint32_t hz);   // configure la fréquence des ticks
uint64_t pit_ticks(void);         // nombre de ticks depuis le démarrage
uint64_t pit_ms(void);            // millisecondes écoulées
void     pit_sleep_ms(uint32_t ms);

#endif
