// =============================================================================
//  kernel/serial.c -- Pilote port série COM1 (16550)
// -----------------------------------------------------------------------------
//  Sert de journal de débogage : visible dans QEMU avec -serial stdio.
// =============================================================================
#include "serial.h"
#include "io.h"
#include <stddef.h>

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   // désactive les interruptions
    outb(COM1 + 3, 0x80);   // active DLAB (réglage du diviseur de débit)
    outb(COM1 + 0, 0x03);   // diviseur bas : 38400 bauds
    outb(COM1 + 1, 0x00);   // diviseur haut
    outb(COM1 + 3, 0x03);   // 8 bits, pas de parité, 1 stop
    outb(COM1 + 2, 0xC7);   // active+vide la FIFO, seuil 14 octets
    outb(COM1 + 4, 0x0B);   // RTS/DSR activés
}

static int serial_tx_ready(void) {
    return inb(COM1 + 5) & 0x20;   // bit THR vide
}

void serial_putc(char c) {
    if (c == '\n') {
        while (!serial_tx_ready()) {}
        outb(COM1, '\r');
    }
    while (!serial_tx_ready()) {}
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    for (size_t i = 0; s[i]; i++) serial_putc(s[i]);
}
