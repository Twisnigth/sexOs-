// =============================================================================
//  kernel/pic.h -- Contrôleur d'interruptions programmable 8259 (PIC)
// =============================================================================
#ifndef MONOS_PIC_H
#define MONOS_PIC_H

#include <stdint.h>

void pic_remap(void);            // réimplante les IRQ sur les vecteurs 32-47
void pic_send_eoi(uint8_t irq);  // signale la fin d'interruption
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);

#endif
