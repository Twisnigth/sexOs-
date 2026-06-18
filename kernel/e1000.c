// =============================================================================
//  kernel/e1000.c -- Pilote carte réseau Intel e1000 (82540EM, défaut QEMU)
// -----------------------------------------------------------------------------
//  Registres MMIO accédés via la fenêtre HHDM (le BAR0 pointe dans le trou MMIO
//  PCI sous 4 Gio, mappé par Limine). Anneaux de descripteurs RX/TX en DMA.
// =============================================================================
#include "net.h"
#include "pci.h"
#include "heap.h"
#include "boot.h"
#include "vmm.h"
#include "klib.h"

// --- Registres ---------------------------------------------------------------
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL    0x5400
#define REG_RAH    0x5404
#define REG_MTA    0x5200

#define CTRL_SLU   0x40
#define CTRL_ASDE  0x20
#define CTRL_RST   0x04000000

#define RCTL_EN    0x02
#define RCTL_BAM   0x8000
#define RCTL_SECRC 0x04000000
#define RCTL_BSIZE 0x00         // 2048 octets

#define TCTL_EN    0x02
#define TCTL_PSP   0x08

#define RX_DESC 32
#define TX_DESC 32
#define BUF_SZ  2048

#define CMD_EOP  0x01
#define CMD_IFCS 0x02
#define CMD_RS   0x08
#define STAT_DD  0x01

struct rx_desc { uint64_t addr; uint16_t len; uint16_t csum; uint8_t status; uint8_t err; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

static volatile uint8_t *mmio;
static struct rx_desc *rx_ring; static uint64_t rx_ring_phys;
static struct tx_desc *tx_ring; static uint64_t tx_ring_phys;
static uint8_t *rx_buf; static uint64_t rx_buf_phys;
static uint8_t *tx_buf; static uint64_t tx_buf_phys;
static int rx_cur, tx_cur;
static mac_t mac;
static bool present;

static inline void reg_write(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static inline uint32_t reg_read(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }

bool nic_present(void) { return present; }
mac_t nic_mac(void) { return mac; }

void nic_send(const void *frame, uint16_t len) {
    if (!present) return;
    if (len > BUF_SZ) len = BUF_SZ;
    memcpy(tx_buf + tx_cur * BUF_SZ, frame, len);
    struct tx_desc *d = &tx_ring[tx_cur];
    d->addr = tx_buf_phys + (uint64_t)tx_cur * BUF_SZ;
    d->len = len;
    d->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    d->status = 0;
    int next = (tx_cur + 1) % TX_DESC;
    reg_write(REG_TDT, next);
    // Attente de l'achèvement de la transmission.
    for (int i = 0; i < 1000000 && !(d->status & STAT_DD); i++) { }
    tx_cur = next;
}

void nic_poll(void) {
    if (!present) return;
    while (rx_ring[rx_cur].status & STAT_DD) {
        uint16_t len = rx_ring[rx_cur].len;
        net_rx(rx_buf + rx_cur * BUF_SZ, len);
        rx_ring[rx_cur].status = 0;
        reg_write(REG_RDT, rx_cur);              // rend le descripteur au NIC
        rx_cur = (rx_cur + 1) % RX_DESC;
    }
}

bool e1000_init(void) {
    const pci_device_t *dev = pci_find(0x02, 0x00);   // classe réseau Ethernet
    if (!dev) { kprintf("[e1000] aucune carte reseau\n"); return false; }

    // Active l'espace mémoire + le bus mastering (DMA).
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    uint64_t bar = dev->bar[0] & ~0xFULL;
    vmm_map_mmio(bar, 0x20000);          // mappe les registres MMIO (128 Kio)
    mmio = (volatile uint8_t *)phys_to_virt(bar);

    // MAC depuis le registre d'adresse de réception (initialisé par QEMU).
    uint32_t ral = reg_read(REG_RAL), rah = reg_read(REG_RAH);
    mac.b[0] = ral; mac.b[1] = ral >> 8; mac.b[2] = ral >> 16; mac.b[3] = ral >> 24;
    mac.b[4] = rah; mac.b[5] = rah >> 8;

    reg_write(REG_IMC, 0xFFFFFFFF);              // pas d'interruptions (on poll)

    // Vide la table multicast.
    for (int i = 0; i < 128; i++) reg_write(REG_MTA + i * 4, 0);

    // Liaison montante.
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    // Anneaux + tampons DMA.
    rx_ring = (struct rx_desc *)dma_alloc(RX_DESC * sizeof(struct rx_desc), &rx_ring_phys);
    tx_ring = (struct tx_desc *)dma_alloc(TX_DESC * sizeof(struct tx_desc), &tx_ring_phys);
    rx_buf  = (uint8_t *)dma_alloc(RX_DESC * BUF_SZ, &rx_buf_phys);
    tx_buf  = (uint8_t *)dma_alloc(TX_DESC * BUF_SZ, &tx_buf_phys);
    memset(rx_ring, 0, RX_DESC * sizeof(struct rx_desc));
    memset(tx_ring, 0, TX_DESC * sizeof(struct tx_desc));
    for (int i = 0; i < RX_DESC; i++)
        rx_ring[i].addr = rx_buf_phys + (uint64_t)i * BUF_SZ;

    // Configuration RX.
    reg_write(REG_RDBAL, (uint32_t)rx_ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, RX_DESC * sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_DESC - 1);
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE);

    // Configuration TX.
    reg_write(REG_TDBAL, (uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, TX_DESC * sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 0x0060200A);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));

    rx_cur = tx_cur = 0;
    present = true;

    kprintf("[e1000] carte %x:%x, MAC %x:%x:%x:%x:%x:%x\n",
            dev->vendor_id, dev->device_id,
            mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);
    return true;
}
