// =============================================================================
//  kernel/ahci.h -- Pilote disque SATA (AHCI), pour les VM modernes (q35,
//  VMware, VirtualBox...) dont le disque n'est PAS de l'IDE « legacy ».
// =============================================================================
#ifndef SEXOS_AHCI_H
#define SEXOS_AHCI_H

#include <stdint.h>
#include <stdbool.h>

bool     ahci_init(void);                 // detecte un disque SATA (true si trouve)
bool     ahci_ok(void);
uint32_t ahci_sector_count(void);
bool     ahci_read(uint32_t lba, uint32_t count, void *buf);          // secteurs de 512 o
bool     ahci_write(uint32_t lba, uint32_t count, const void *buf);

#endif
