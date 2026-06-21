// =============================================================================
//  kernel/usb.c -- Controleur USB xHCI + enumeration des peripheriques
// -----------------------------------------------------------------------------
//  Amorce le contrôleur xHCI (reset, anneaux de commande/événement, DCBAA,
//  scratchpad), réinitialise les ports, puis pour chaque port connecté :
//  Enable Slot -> Address Device -> GET_DESCRIPTOR (device + config) afin de
//  lire VID/PID et la classe d'interface. Tout est en scrutation (pas d'IRQ).
//  C'est la base commune au stockage USB et aux périphériques HID.
// =============================================================================
#include "usb.h"
#include "pci.h"
#include "heap.h"
#include "boot.h"
#include "vmm.h"
#include "pit.h"
#include "klib.h"

// --- TRB (Transfer Request Block) : 16 octets --------------------------------
typedef struct { uint64_t param; uint32_t status; uint32_t control; } __attribute__((packed)) trb_t;

#define TRB_TYPE(t)      ((t) << 10)
// Types de TRB
#define TR_NORMAL        1
#define TR_SETUP         2
#define TR_DATA          3
#define TR_STATUS        4
#define TR_LINK          6
#define TR_ENABLE_SLOT   9
#define TR_ADDRESS_DEV   11
#define TR_CMD_COMPLETE  33
#define TR_PORT_STATUS   34
#define TR_TRANSFER      32

#define RING_SZ 16                          // TRBs par anneau (+ Link)

static volatile uint8_t *cap;               // registres de capacite (base MMIO)
static volatile uint8_t *op;                // registres operationnels
static volatile uint8_t *rt;                // registres runtime
static volatile uint32_t *db;               // sonnettes (doorbells)
static int max_slots, num_ports;
static bool xhci_ok;

static trb_t   *cmd_ring;   static uint64_t cmd_ring_phys; static int cmd_idx; static uint8_t cmd_cycle;
static trb_t   *evt_ring;   static uint64_t evt_ring_phys; static int evt_idx; static uint8_t evt_cycle;
static uint64_t *dcbaa;     static uint64_t dcbaa_phys;

static usb_dev_t devs[16];
static int dev_count;

int usb_count(void) { return dev_count; }
const usb_dev_t *usb_get(int i) { return (i >= 0 && i < dev_count) ? &devs[i] : NULL; }

// --- acces registres ---------------------------------------------------------
static inline uint32_t rd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void     wr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
// IMPORTANT : ce contrôleur (QEMU) n'accepte que des accès MMIO 32 bits. Les
// registres 64 bits doivent donc être écrits en DEUX écritures 32 bits (octet
// de poids faible d'abord, puis poids fort) -- c'est aussi la méthode
// recommandée par la spec xHCI.
static inline void     wr64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    *(volatile uint32_t *)(b + o)     = (uint32_t)v;
    *(volatile uint32_t *)(b + o + 4) = (uint32_t)(v >> 32);
}

// Op regs
#define O_USBCMD 0x00
#define O_USBSTS 0x04
#define O_CRCR   0x18
#define O_DCBAAP 0x30
#define O_CONFIG 0x38
#define O_PORTSC(p) (0x400 + ((p)-1)*0x10)
// Runtime (interrupteur 0)
#define R_ERSTSZ 0x28
#define R_ERSTBA 0x30
#define R_ERDP   0x38

static void *dma_zeroed(size_t sz, uint64_t *phys) {
    void *p = dma_alloc(sz, phys);
    if (p) memset(p, 0, sz);
    return p;
}

// Petit delai par attente active (independant des interruptions / du PIT, pour
// ne JAMAIS pouvoir bloquer le demarrage).
static void spin(volatile uint64_t n) { while (n--) __asm__ volatile ("pause"); }

// Attend qu'un bit passe a 'val' (borne par un compteur d'iterations). 1=succes.
static bool wait_bit(uint32_t off, uint32_t mask, uint32_t val, uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        if (((rd32(op, off) & mask) ? 1u : 0u) == val) return true;
        spin(30);
    }
    return false;
}

// --- anneau d'evenements : lit le prochain evenement (scrutation, borne) -----
static bool next_event(trb_t *out, uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        trb_t *e = &evt_ring[evt_idx];
        if ((e->control & 1) == evt_cycle) {
            *out = *e;
            if (++evt_idx >= RING_SZ) { evt_idx = 0; evt_cycle ^= 1; }
            wr64(rt, R_ERDP, (evt_ring_phys + (uint64_t)evt_idx * sizeof(trb_t)) | (1ULL << 3));
            return true;
        }
        spin(30);
    }
    return false;
}

// Place un TRB dans l'anneau de commandes et sonne la cloche 0.
static void cmd_push(uint64_t param, uint32_t status, uint32_t control) {
    cmd_ring[cmd_idx].param = param;
    cmd_ring[cmd_idx].status = status;
    cmd_ring[cmd_idx].control = control | cmd_cycle;
    if (++cmd_idx >= RING_SZ - 1) {        // dernier slot = Link TRB
        cmd_ring[RING_SZ-1].param = cmd_ring_phys;
        cmd_ring[RING_SZ-1].status = 0;
        cmd_ring[RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1 << 1) /*TC*/ | cmd_cycle;
        cmd_idx = 0; cmd_cycle ^= 1;
    }
    db[0] = 0;                              // sonnette du contrôleur (Command Ring)
}

// Execute une commande et renvoie le code d'achevement (0 si echec) + slot.
static int cmd_exec(uint64_t param, uint32_t control, uint8_t *slot_out) {
    cmd_push(param, 0, control);
    trb_t e;
    // L'anneau d'evenements est PARTAGE : on ignore les evenements non lies
    // (ex: Port Status Change) jusqu'a recevoir le Command Completion.
    for (int skip = 0; skip < 32; skip++) {
        if (!next_event(&e, 60000)) {
            kprintf("[xhci] cmd timeout: USBSTS=%x USBCMD=%x CRCR_lo=%x ERDP_lo=%x\n",
                    rd32(op, O_USBSTS), rd32(op, O_USBCMD), rd32(op, O_CRCR), rd32(rt, R_ERDP));
            return 0;
        }
        if (((e.control >> 10) & 0x3F) == TR_CMD_COMPLETE) {
            if (slot_out) *slot_out = (e.control >> 24) & 0xFF;
            return (e.status >> 24) & 0xFF;    // completion code (1 = success)
        }
        // autre evenement -> on continue a scruter
    }
    return 0;
}

// =============================================================================
//  Transfert de controle sur EP0 (Setup/Data/Status) pour un peripherique
// =============================================================================
static int control_in(trb_t *ep0_ring, uint64_t ep0_phys, int *ep0_idx, uint8_t *ep0_cycle,
                      uint8_t slot, uint8_t bmReq, uint8_t bReq, uint16_t wValue,
                      uint16_t wIndex, uint16_t wLength, uint64_t buf_phys) {
    // Setup Stage
    uint64_t setup = (uint64_t)bmReq | ((uint64_t)bReq << 8) | ((uint64_t)wValue << 16)
                   | ((uint64_t)wIndex << 32) | ((uint64_t)wLength << 48);
    int i = *ep0_idx; uint8_t cy = *ep0_cycle;
    ep0_ring[i].param = setup; ep0_ring[i].status = 8;
    ep0_ring[i].control = TRB_TYPE(TR_SETUP) | (3u << 16) /*TRT=IN*/ | (1u << 6) /*IDT*/ | cy;
    i++;
    // Data Stage (IN)
    ep0_ring[i].param = buf_phys; ep0_ring[i].status = wLength;
    ep0_ring[i].control = TRB_TYPE(TR_DATA) | (1u << 16) /*DIR=IN*/ | cy;
    i++;
    // Status Stage (OUT, IOC)
    ep0_ring[i].param = 0; ep0_ring[i].status = 0;
    ep0_ring[i].control = TRB_TYPE(TR_STATUS) | (1u << 5) /*IOC*/ | cy;
    i++;
    if (i >= RING_SZ - 1) {
        ep0_ring[RING_SZ-1].param = ep0_phys; ep0_ring[RING_SZ-1].status = 0;
        ep0_ring[RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1 << 1) | cy;
        i = 0; cy ^= 1;
    }
    *ep0_idx = i; *ep0_cycle = cy;
    db[slot] = 1;                           // sonnette EP0 du slot
    trb_t e;
    // attend le Transfer Event (type 32) lie a EP0, en ignorant le reste.
    for (int skip = 0; skip < 32; skip++) {
        if (!next_event(&e, 60000)) return -1;
        if (((e.control >> 10) & 0x3F) == TR_TRANSFER) {
            uint8_t cc = (e.status >> 24) & 0xFF;   // 1=Success, 13=Short Packet (ok)
            return (cc == 1 || cc == 13) ? 0 : -1;
        }
    }
    return -1;
}

// =============================================================================
//  Enumeration d'un port connecte
// =============================================================================
static void enumerate_port(int port) {
    uint32_t sc = rd32(op, O_PORTSC(port));
    uint8_t speed = (sc >> 10) & 0x0F;

    // 1) Enable Slot
    uint8_t slot = 0;
    int cc = cmd_exec(0, TRB_TYPE(TR_ENABLE_SLOT), &slot);
    if (cc != 1 || slot == 0) {
        kprintf("[xhci] port %d : Enable Slot echec (code=%d slot=%d)\n", port, cc, slot);
        return;
    }

    // 2) Contextes (32 octets) : input (33*32) + device (32*32) + anneau EP0
    uint64_t in_phys, dev_phys, ep0_phys;
    uint8_t *inctx = dma_zeroed(2048, &in_phys);
    uint8_t *devctx = dma_zeroed(2048, &dev_phys);
    trb_t   *ep0_ring = dma_zeroed(4096, &ep0_phys);
    if (!inctx || !devctx || !ep0_ring) return;
    int ep0_idx = 0; uint8_t ep0_cycle = 1;

    // Input Control Context : ajoute slot (A0) + EP0 (A1)
    *(uint32_t *)(inctx + 4) = 0x3;                 // Add flags : slot + EP0
    // Slot Context (a +32) : context entries=1, speed, port racine
    uint32_t *slotc = (uint32_t *)(inctx + 32);
    slotc[0] = (1u << 27) | ((uint32_t)speed << 20);
    slotc[1] = (uint32_t)port << 16;
    // EP0 Context (a +64) : type=control(4), mps selon vitesse, anneau TR
    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : (speed == 2) ? 8 : 64;
    uint32_t *ep0c = (uint32_t *)(inctx + 64);
    ep0c[1] = (4u << 3) | (mps << 16) | (3u << 1) /*CErr*/;
    *(uint64_t *)(ep0c + 2) = ep0_phys | 1;         // TR dequeue ptr | DCS

    dcbaa[slot] = dev_phys;

    // 3) Address Device
    if (cmd_exec(in_phys, TRB_TYPE(TR_ADDRESS_DEV) | ((uint32_t)slot << 24), 0) != 1) {
        kprintf("[xhci] port %d : Address Device echec\n", port); return;
    }

    // 4) GET_DESCRIPTOR (device, 18 o)
    uint64_t buf_phys; uint8_t *buf = dma_zeroed(256, &buf_phys);
    if (!buf) return;
    if (control_in(ep0_ring, ep0_phys, &ep0_idx, &ep0_cycle, slot,
                   0x80, 6, 0x0100, 0, 18, buf_phys) != 0) {
        kprintf("[xhci] port %d : GET_DESCRIPTOR(device) echec\n", port); return;
    }
    uint16_t vid = buf[8] | (buf[9] << 8);
    uint16_t pid = buf[10] | (buf[11] << 8);
    uint8_t  dclass = buf[4];

    // 5) GET_DESCRIPTOR (config) -> classe de la 1re interface
    uint8_t iclass = 0;
    memset(buf, 0, 64);
    if (control_in(ep0_ring, ep0_phys, &ep0_idx, &ep0_cycle, slot,
                   0x80, 6, 0x0200, 0, 64, buf_phys) == 0) {
        // parcourt les descripteurs : cherche le 1er Interface (type 4)
        int total = buf[2] | (buf[3] << 8); if (total > 64) total = 64;
        for (int o = buf[0]; o + 1 < total; o += buf[o]) {
            if (buf[o] == 0) break;
            if (buf[o+1] == 4) { iclass = buf[o+5]; break; }   // bInterfaceClass
        }
    }

    if (dev_count < 16) {
        usb_dev_t *d = &devs[dev_count++];
        d->slot = slot; d->port = port; d->speed = speed;
        d->vendor = vid; d->product = pid; d->dev_class = dclass; d->if_class = iclass;
    }
    kprintf("[xhci] port %d : peripherique %04x:%04x classe=%02x (slot %d)\n",
            port, vid, pid, iclass ? iclass : dclass, slot);
}

// =============================================================================
//  Amorcage du controleur
// =============================================================================
void usb_init(void) {
    const pci_device_t *dev = pci_find(0x0C, 0x03);    // bus serie USB
    if (!dev || dev->prog_if != 0x30) {                // 0x30 = xHCI
        kprintf("[usb] pas de controleur xHCI\n");
        return;
    }
    // active MMIO + bus master
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x6);

    uint64_t bar = ((uint64_t)dev->bar[0] & ~0xFULL) | ((uint64_t)dev->bar[1] << 32);
    vmm_map_mmio(bar, 0x10000);
    cap = (volatile uint8_t *)phys_to_virt(bar);

    // ATTENTION : certains environnements (QEMU/TCG) ne renvoient que 0 pour les
    // lectures MMIO en 8/16 bits sur les registres de capacite. On lit donc TOUT
    // en 32 bits et on extrait les octets nous-memes (CAPLENGTH = octet 0,
    // HCIVERSION = octets 2-3 du meme dword a l'offset 0).
    uint32_t cap0 = rd32(cap, 0x00);
    uint8_t  caplen = cap0 & 0xFF;
    uint16_t hciver = (cap0 >> 16) & 0xFFFF;
    uint32_t hcs1 = rd32(cap, 0x04);
    uint32_t hcs2 = rd32(cap, 0x08);
    uint32_t dboff = rd32(cap, 0x14) & ~0x3u;
    uint32_t rtsoff = rd32(cap, 0x18) & ~0x1Fu;
    op = cap + caplen;
    rt = cap + rtsoff;
    db = (volatile uint32_t *)(cap + dboff);
    max_slots = hcs1 & 0xFF;
    num_ports = (hcs1 >> 24) & 0xFF;
    kprintf("[xhci] xHCI v%x, caplen=%d, %d slots, %d ports (dboff=%x rtsoff=%x)\n",
            hciver, caplen, max_slots, num_ports, dboff, rtsoff);

    // stop + reset
    wr32(op, O_USBCMD, rd32(op, O_USBCMD) & ~1u);
    wait_bit(O_USBSTS, 1, 1, 20000);                     // HCHalted
    wr32(op, O_USBCMD, rd32(op, O_USBCMD) | (1u << 1)); // HCRST
    if (!wait_bit(O_USBCMD, (1u << 1), 0, 60000)) { kprintf("[xhci] reset timeout\n"); return; }
    wait_bit(O_USBSTS, (1u << 11), 0, 60000);           // CNR (controller not ready) -> 0

    if (max_slots > 8) max_slots = 8;
    wr32(op, O_CONFIG, max_slots);

    // DCBAA + scratchpad
    dcbaa = dma_zeroed(2048, &dcbaa_phys);
    uint32_t spbufs = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (spbufs) {
        uint64_t sp_arr_phys; uint64_t *sp_arr = dma_zeroed(spbufs * 8, &sp_arr_phys);
        for (uint32_t i = 0; i < spbufs; i++) { uint64_t pg; dma_zeroed(4096, &pg); sp_arr[i] = pg; }
        dcbaa[0] = sp_arr_phys;
    }
    wr64(op, O_DCBAAP, dcbaa_phys);

    // anneau de commandes
    cmd_ring = dma_zeroed(4096, &cmd_ring_phys); cmd_idx = 0; cmd_cycle = 1;
    wr64(op, O_CRCR, cmd_ring_phys | 1);               // RCS = 1

    // anneau d'evenements + ERST
    evt_ring = dma_zeroed(4096, &evt_ring_phys); evt_idx = 0; evt_cycle = 1;
    uint64_t erst_phys; uint64_t *erst = dma_zeroed(64, &erst_phys);
    erst[0] = evt_ring_phys;
    erst[1] = RING_SZ;                                 // taille du segment
    wr32(rt, R_ERSTSZ, 1);
    wr64(rt, R_ERDP, evt_ring_phys);
    wr64(rt, R_ERSTBA, erst_phys);
    // active l'interrupteur 0 (IMAN.IE) -- inoffensif et requis par certains
    // contrôleurs avant de poster des evenements.
    wr32(rt, 0x20, 0x2);

    // run
    wr32(op, O_USBCMD, rd32(op, O_USBCMD) | 1u);
    if (!wait_bit(O_USBSTS, 1, 0, 60000)) { kprintf("[xhci] demarrage timeout\n"); return; }
    xhci_ok = true;
    kprintf("[xhci] running: USBSTS=%x CRCR_lo=%x cmd_phys=%x evt_phys=%x erst_phys=%x\n",
            rd32(op, O_USBSTS), rd32(op, O_CRCR), (uint32_t)cmd_ring_phys,
            (uint32_t)evt_ring_phys, (uint32_t)erst_phys);

    // reset + enumeration des ports connectes
    for (int p = 1; p <= num_ports; p++) {
        uint32_t sc = rd32(op, O_PORTSC(p));
        if (!(sc & 1)) continue;                        // CCS : pas connecte
        // reset du port (conserve les bits qui ne s'effacent pas en ecriture)
        wr32(op, O_PORTSC(p), (sc & 0x0E00C3E0) | (1u << 4));   // PR
        wait_bit(O_PORTSC(p), (1u << 4), 0, 50000);       // PR -> 0 (approx via op base)
        spin(1000000);
        sc = rd32(op, O_PORTSC(p));
        if (sc & (1u << 1)) enumerate_port(p);          // PED : port active
    }
    kprintf("[usb] %d peripherique(s) USB detecte(s)\n", dev_count);
}
