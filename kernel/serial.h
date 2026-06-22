// =============================================================================
//  kernel/serial.h -- Pilote port série COM1 (journal de débogage)
// =============================================================================
#ifndef SEXOS_SERIAL_H
#define SEXOS_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);

#endif
