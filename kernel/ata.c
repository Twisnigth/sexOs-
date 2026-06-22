// =============================================================================
//  kernel/ata.c -- Pilote disque IDE/ATA en PIO (canal primaire 0x1F0, LBA28)
// -----------------------------------------------------------------------------
//  Lecture/ecriture secteur par secteur en E/S programmee (PIO), sans DMA.
//  Suffit pour un petit stockage persistant. Disque "maitre" du canal primaire.
// =============================================================================
#include "ata.h"
#include "io.h"
#include "klib.h"

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

static bool     present;
static uint32_t total_sectors;

static void delay400(void) { for (int i = 0; i < 4; i++) inb(ATA_CTRL); }   // ~400 ns
static void wait_bsy(void) { while (inb(ATA_STATUS) & ST_BSY) ; }
static bool wait_drq(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return false;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return true;
    }
    return false;
}

bool ata_init(void) {
    present = false;
    outb(ATA_DRIVE, 0xA0);                 // maitre
    delay400();
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0x00) return false;       // bus flottant / pas de disque
    // IDENTIFY
    outb(ATA_SECCNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);
    st = inb(ATA_STATUS);
    if (st == 0) return false;
    wait_bsy();
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return false;   // pas un disque ATA
    if (!wait_drq()) return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    total_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);  // LBA28
    present = true;
    kprintf("[ata] disque IDE detecte : %u secteurs (%u Mio)\n",
            total_sectors, total_sectors / 2048);
    return true;
}

bool ata_ok(void) { return present; }
uint32_t ata_sector_count(void) { return total_sectors; }

static bool rw(uint32_t lba, uint32_t count, void *buf, bool write) {
    if (!present) return false;
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        wait_bsy();
        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   // LBA, maitre
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA0, lba & 0xFF);
        outb(ATA_LBA1, (lba >> 8) & 0xFF);
        outb(ATA_LBA2, (lba >> 16) & 0xFF);
        outb(ATA_CMD, write ? 0x30 : 0x20);             // WRITE / READ SECTORS
        if (!wait_drq()) return false;
        if (write) {
            for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
            outb(ATA_CMD, 0xE7);                        // FLUSH CACHE
            wait_bsy();
        } else {
            for (int i = 0; i < 256; i++) *p++ = inw(ATA_DATA);
        }
        lba++;
    }
    return true;
}

bool ata_read(uint32_t lba, uint32_t count, void *buf)        { return rw(lba, count, buf, false); }
bool ata_write(uint32_t lba, uint32_t count, const void *buf) { return rw(lba, count, (void *)buf, true); }
