// =============================================================================
//  kernel/pci.h -- Énumération du bus PCI
// =============================================================================
#ifndef MONOS_PCI_H
#define MONOS_PCI_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint32_t bar[6];
} pci_device_t;

void pci_init(void);
int  pci_device_count(void);
const pci_device_t *pci_get_device(int index);
// Cherche le premier périphérique d'une classe/sous-classe donnée (NULL sinon).
const pci_device_t *pci_find(uint8_t class_code, uint8_t subclass);
const char *pci_class_name(uint8_t class_code);

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

#endif
