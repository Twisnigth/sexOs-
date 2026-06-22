// =============================================================================
//  kernel/ahci.c -- Pilote disque SATA (AHCI 1.x) en scrutation
// -----------------------------------------------------------------------------
//  Beaucoup de VM (QEMU q35, VMware, VirtualBox) exposent leur disque via un
//  contrôleur SATA AHCI, et non via l'IDE « legacy » (0x1F0). Ce pilote trouve
//  le 1er disque SATA, l'identifie, et lit/écrit en LBA48 (READ/WRITE DMA EXT),
//  via une liste de commandes + table de commande + PRDT, comme l'exige AHCI.
//  Un seul disque est géré (suffisant pour la persistance du VFS).
// =============================================================================
#include "ahci.h"
#include "pci.h"
#include "heap.h"
#include "vmm.h"
#include "boot.h"
#include "klib.h"

static volatile uint8_t *abar;            // registres MMIO de la HBA (BAR5)
static int      port_idx = -1;            // port du disque trouve
static bool     present;
static uint32_t total_sectors;

// structures DMA du port (un seul port gere)
static uint8_t *clb;  static uint64_t clb_phys;    // liste de commandes (1 Kio)
static uint8_t *fb;   static uint64_t fb_phys;     // FIS recus (256 o)
static uint8_t *ctba; static uint64_t ctba_phys;   // table de commande
static uint8_t *dbuf; static uint64_t dbuf_phys;   // tampon de donnees (4 Kio)
#define AHCI_BOUNCE 4096

// --- acces registres (AHCI n'accepte que des acces 32 bits) ------------------
static inline uint32_t rd(uint32_t o)            { return *(volatile uint32_t *)(abar + o); }
static inline void     wr(uint32_t o, uint32_t v){ *(volatile uint32_t *)(abar + o) = v; }
#define PREG(p, r) (0x100 + (uint32_t)(p) * 0x80 + (r))   // registre du port p

// registres de port
#define PxCLB  0x00
#define PxCLBU 0x04
#define PxFB   0x08
#define PxFBU  0x0C
#define PxIS   0x10
#define PxCMD  0x18
#define PxTFD  0x20
#define PxSIG  0x24
#define PxSSTS 0x28
#define PxSERR 0x30
#define PxCI   0x38

static void *dz(size_t n, uint64_t *ph) { void *p = dma_alloc(n, ph); if (p) memset(p, 0, n); return p; }
static void spin(volatile uint32_t n) { while (n--) __asm__ volatile ("pause"); }

static void stop_port(int p) {
    wr(PREG(p, PxCMD), rd(PREG(p, PxCMD)) & ~1u);          // ST = 0
    wr(PREG(p, PxCMD), rd(PREG(p, PxCMD)) & ~(1u << 4));   // FRE = 0
    for (int i = 0; i < 2000000; i++)
        if (!(rd(PREG(p, PxCMD)) & ((1u << 14) | (1u << 15)))) break;  // FR, CR -> 0
}
static void start_port(int p) {
    for (int i = 0; i < 2000000; i++) if (!(rd(PREG(p, PxCMD)) & (1u << 15))) break;  // CR -> 0
    wr(PREG(p, PxCMD), rd(PREG(p, PxCMD)) | (1u << 4));    // FRE = 1
    wr(PREG(p, PxCMD), rd(PREG(p, PxCMD)) | 1u);           // ST = 1
}

// Remplit le FIS de commande H2D (Register - Host to Device) dans la table.
static void fill_fis(uint8_t cmd, uint64_t lba, uint16_t count) {
    uint8_t *f = ctba;                                     // CFIS au debut de la table
    memset(f, 0, 64);
    f[0] = 0x27;                                           // type FIS : Register H2D
    f[1] = 0x80;                                           // C = 1 (commande)
    f[2] = cmd;
    f[4] = (uint8_t)lba; f[5] = (uint8_t)(lba >> 8); f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                                           // device : LBA
    f[8] = (uint8_t)(lba >> 24); f[9] = (uint8_t)(lba >> 32); f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count; f[13] = (uint8_t)(count >> 8);
}

// Lance la commande déjà préparée (slot 0) et attend son achèvement.
static bool run_cmd(int p, bool write, uint32_t len) {
    wr(PREG(p, PxIS), 0xFFFFFFFF);                         // efface les interruptions du port
    uint32_t *ch = (uint32_t *)clb;                        // en-tete de commande, slot 0
    ch[0] = 5u | (write ? (1u << 6) : 0u) | (1u << 16);    // CFL=5 dwords, W, PRDTL=1
    ch[1] = 0;                                             // PRDBC
    ch[2] = (uint32_t)ctba_phys;
    ch[3] = (uint32_t)(ctba_phys >> 32);
    uint32_t *prd = (uint32_t *)(ctba + 0x80);             // 1re entree PRDT
    prd[0] = (uint32_t)dbuf_phys;
    prd[1] = (uint32_t)(dbuf_phys >> 32);
    prd[2] = 0;
    prd[3] = (len - 1) & 0x3FFFFF;                         // DBC = octets - 1

    for (int i = 0; i < 2000000; i++)                      // attend BSY/DRQ a 0
        if (!(rd(PREG(p, PxTFD)) & (0x80 | 0x08))) break;
    wr(PREG(p, PxCI), 1u);                                 // emet la commande (slot 0)
    for (int i = 0; i < 4000000; i++) {
        if (!(rd(PREG(p, PxCI)) & 1u)) break;              // commande terminee
        if (rd(PREG(p, PxIS)) & (1u << 30)) return false;  // TFES : erreur task file
        spin(10);
    }
    if (rd(PREG(p, PxCI)) & 1u) return false;              // timeout
    if (rd(PREG(p, PxTFD)) & 0x01) return false;           // ERR
    return true;
}

bool ahci_init(void) {
    present = false;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *d = pci_get_device(i);
        if (d->class_code != 0x01 || d->subclass != 0x06) continue;   // SATA AHCI
        uint64_t bar5 = (uint64_t)(d->bar[5] & ~0xFu);
        if (!bar5) continue;
        uint32_t cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
        pci_write32(d->bus, d->slot, d->func, 0x04, cmd | 0x6);        // MMIO + bus master
        vmm_map_mmio(bar5, 0x2000);
        abar = (volatile uint8_t *)phys_to_virt(bar5);
        wr(0x04, rd(0x04) | (1u << 31));                               // GHC.AE = 1 (AHCI)

        uint32_t pi = rd(0x0C);                                        // ports implementes
        for (int p = 0; p < 32; p++) {
            if (!(pi & (1u << p))) continue;
            if ((rd(PREG(p, PxSSTS)) & 0x0F) != 3) continue;           // DET : pas de disque
            if (rd(PREG(p, PxSIG)) != 0x00000101) continue;            // pas un disque SATA
            // disque trouve : on l'amorce
            port_idx = p;
            stop_port(p);
            clb  = dz(1024, &clb_phys);
            fb   = dz(256,  &fb_phys);
            ctba = dz(4096, &ctba_phys);
            dbuf = dz(AHCI_BOUNCE, &dbuf_phys);
            if (!clb || !fb || !ctba || !dbuf) return false;
            wr(PREG(p, PxCLB),  (uint32_t)clb_phys);
            wr(PREG(p, PxCLBU), (uint32_t)(clb_phys >> 32));
            wr(PREG(p, PxFB),   (uint32_t)fb_phys);
            wr(PREG(p, PxFBU),  (uint32_t)(fb_phys >> 32));
            wr(PREG(p, PxSERR), 0xFFFFFFFF);
            wr(PREG(p, PxIS),   0xFFFFFFFF);
            start_port(p);

            fill_fis(0xEC, 0, 0);                                      // IDENTIFY DEVICE
            if (run_cmd(p, false, 512)) {
                uint16_t *id = (uint16_t *)dbuf;
                uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16)
                             | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
                uint32_t s28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
                total_sectors = s48 ? (uint32_t)s48 : s28;
                present = true;
                kprintf("[ahci] disque SATA detecte (port %d) : %u secteurs (%u Mio)\n",
                        p, total_sectors, total_sectors / 2048);
                return true;
            }
        }
    }
    return false;
}

bool     ahci_ok(void)           { return present; }
uint32_t ahci_sector_count(void) { return total_sectors; }

static bool ahci_rw(uint32_t lba, uint32_t count, void *buf, bool write) {
    if (!present) return false;
    uint8_t *u = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > (AHCI_BOUNCE / 512) ? (AHCI_BOUNCE / 512) : count;
        if (write) memcpy(dbuf, u, n * 512);
        fill_fis(write ? 0x35 : 0x25, lba, (uint16_t)n);   // WRITE / READ DMA EXT
        if (!run_cmd(port_idx, write, n * 512)) return false;
        if (!write) memcpy(u, dbuf, n * 512);
        u += n * 512; lba += n; count -= n;
    }
    return true;
}
bool ahci_read(uint32_t lba, uint32_t count, void *buf)        { return ahci_rw(lba, count, buf, false); }
bool ahci_write(uint32_t lba, uint32_t count, const void *buf) { return ahci_rw(lba, count, (void *)buf, true); }
