// =============================================================================
//  kernel/ata.h -- Pilote disque IDE/ATA en PIO (canal primaire, LBA28)
// =============================================================================
#ifndef SEXOS_ATA_H
#define SEXOS_ATA_H

#include <stdint.h>
#include <stdbool.h>

bool     ata_init(void);                 // detecte le disque maitre (0x1F0)
bool     ata_ok(void);                   // un disque est-il present ?
uint32_t ata_sector_count(void);         // nombre de secteurs de 512 o
bool     ata_read(uint32_t lba, uint32_t count, void *buf);
bool     ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif
