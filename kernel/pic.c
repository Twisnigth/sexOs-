// =============================================================================
//  kernel/pic.c -- 8259 PIC : réimplantation des IRQ et masquage
// -----------------------------------------------------------------------------
//  On utilise le PIC hérité (plus simple et parfaitement fonctionnel en QEMU)
//  plutôt que l'APIC. Les IRQ 0-15 sont déplacées sur les vecteurs 32-47 pour
//  ne pas entrer en conflit avec les exceptions CPU 0-31.
// =============================================================================
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_remap(void) {
    uint8_t a1 = inb(PIC1_DATA);   // sauvegarde des masques
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();   // init (ICW1)
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();  // ICW2 : PIC1 -> vecteurs 32+
    outb(PIC2_DATA, 0x28); io_wait();  // ICW2 : PIC2 -> vecteurs 40+
    outb(PIC1_DATA, 0x04); io_wait();  // ICW3 : esclave sur IRQ2
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();  // ICW4 : mode 8086
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, a1);               // restaure les masques
    outb(PIC2_DATA, a2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}
