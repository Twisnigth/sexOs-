// =============================================================================
//  kernel/ps2.h -- Pilote clavier + souris PS/2
// =============================================================================
#ifndef SEXOS_PS2_H
#define SEXOS_PS2_H

#include <stdint.h>
#include <stdbool.h>

void ps2_init(void);
void ps2_mouse_pos(int32_t *x, int32_t *y);
uint8_t ps2_mouse_buttons(void);

#endif
