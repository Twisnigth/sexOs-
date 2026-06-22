// =============================================================================
//  kernel/pci.c -- Énumération PCI via l'espace de configuration (0xCF8/0xCFC)
// =============================================================================
#include "pci.h"
#include "io.h"
#include "klib.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define MAX_DEVICES 64

static pci_device_t devices[MAX_DEVICES];
static int device_count;

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)((bus << 16) | (slot << 11) | (func << 8) |
                      (off & 0xFC) | 0x80000000u);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, config_address(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDR, config_address(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read32(bus, slot, func, 0x00);
    uint16_t vendor = id & 0xFFFF;
    if (vendor == 0xFFFF) return;        // pas de périphérique

    if (device_count >= MAX_DEVICES) return;
    pci_device_t *d = &devices[device_count++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor_id = vendor;
    d->device_id = id >> 16;

    uint32_t cls = pci_read32(bus, slot, func, 0x08);
    d->prog_if   = (cls >> 8) & 0xFF;
    d->subclass  = (cls >> 16) & 0xFF;
    d->class_code = (cls >> 24) & 0xFF;
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, 0x10 + i * 4);
}

void pci_init(void) {
    device_count = 0;
    // Balayage exhaustif (suffisant et simple) : tous bus/slots/fonctions.
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read16(bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;
            uint8_t header = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF;
            int funcs = (header & 0x80) ? 8 : 1;   // périphérique multifonction ?
            for (int func = 0; func < funcs; func++)
                probe(bus, slot, func);
        }
    }
    kprintf("[pci] %d peripheriques detectes\n", device_count);
    for (int i = 0; i < device_count; i++) {
        pci_device_t *d = &devices[i];
        kprintf("  %x:%x.%x  %x:%x  classe %x/%x (%s)\n",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                d->class_code, d->subclass, pci_class_name(d->class_code));
    }
}

int pci_device_count(void) { return device_count; }
const pci_device_t *pci_get_device(int index) {
    return (index >= 0 && index < device_count) ? &devices[index] : 0;
}
const pci_device_t *pci_find(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < device_count; i++)
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    return 0;
}

const char *pci_class_name(uint8_t c) {
    switch (c) {
        case 0x00: return "non classe";
        case 0x01: return "stockage";
        case 0x02: return "reseau";
        case 0x03: return "affichage";
        case 0x04: return "multimedia";
        case 0x06: return "pont";
        case 0x0C: return "bus serie (USB...)";
        default:   return "autre";
    }
}
