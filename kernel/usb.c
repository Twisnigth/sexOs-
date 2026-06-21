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
#include "input.h"
#include "framebuffer.h"

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
#define TR_CONFIG_EP     12
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

// --- etat par peripherique (indexe par slot) : contextes + EP0 + EP HID -------
typedef struct {
    bool     used;
    uint8_t  slot, speed, port;
    trb_t   *ep0;     uint64_t ep0_phys;  int ep0_idx;  uint8_t ep0_cycle;
    uint8_t *inctx;   uint64_t in_phys;
    // endpoint d'interruption IN (clavier/souris HID)
    bool     is_hid, is_kbd, is_mouse;
    uint8_t  ep_dci;  uint16_t ep_mps;  uint8_t ep_interval;
    trb_t   *ep_ring; uint64_t ep_ring_phys; int ep_idx; uint8_t ep_cycle;
    uint8_t *rbuf;    uint64_t rbuf_phys;
    uint8_t  prev[8];                 // dernier rapport clavier (detection des appuis)
    int32_t  mx, my;                  // position souris
    // endpoints "bulk" (stockage de masse, classe 8)
    bool     is_msc;
    uint8_t  bin_dci, bout_dci; uint16_t bin_mps, bout_mps;
} xdev_t;
static xdev_t xdevs[16];              // indexe par numero de slot (1..max_slots)

// --- USB HID usage -> caractere, disposition AZERTY (alignee sur ps2.c) -------
//  Les codes "usage" HID sont positionnels (clavier US physique). On les remappe
//  vers l'AZERTY exactement comme la table scancode du pilote PS/2.
static const char hid2az[256] = {
    [0x1E]='1',[0x1F]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',[0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
    [0x2D]='-',[0x2E]='=',
    [0x14]='a',[0x1A]='z',[0x08]='e',[0x15]='r',[0x17]='t',[0x1C]='y',[0x18]='u',[0x0C]='i',[0x12]='o',[0x13]='p',
    [0x2F]='<',[0x30]='>',
    [0x04]='q',[0x16]='s',[0x07]='d',[0x09]='f',[0x0A]='g',[0x0B]='h',[0x0D]='j',[0x0E]='k',[0x0F]='l',[0x33]='m',
    [0x34]='\'',[0x35]='`',
    [0x1D]='w',[0x1B]='x',[0x06]='c',[0x19]='v',[0x05]='b',[0x11]='n',[0x10]=',',[0x36]=';',[0x37]=':',[0x38]='!',
    [0x64]='\\', [0x2C]=' ',
};
static const char hid2az_shift[256] = {
    [0x1E]='!',[0x1F]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x2D]='_',[0x2E]='+',
    [0x14]='A',[0x1A]='Z',[0x08]='E',[0x15]='R',[0x17]='T',[0x1C]='Y',[0x18]='U',[0x0C]='I',[0x12]='O',[0x13]='P',
    [0x2F]='{',[0x30]='}',
    [0x04]='Q',[0x16]='S',[0x07]='D',[0x09]='F',[0x0A]='G',[0x0B]='H',[0x0D]='J',[0x0E]='K',[0x0F]='L',[0x33]='M',
    [0x34]='"',[0x35]='~',
    [0x1D]='W',[0x1B]='X',[0x06]='C',[0x19]='V',[0x05]='B',[0x11]='N',[0x10]='?',[0x36]='.',[0x37]='/',[0x38]='!',
    [0x64]='|', [0x2C]=' ',
};

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

// Sauvegarde/restauration de l'etat des interruptions : l'acces a l'anneau
// d'evenements est PARTAGE entre la tache HID (IF=1) et les transferts de
// stockage faits depuis un appel systeme (IF=0). On serialise donc tout acces
// par une section critique (cli), sans jamais reactiver les IRQ a tort.
static inline uint64_t irq_save(void) {
    uint64_t f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("push %0; popfq" :: "r"(f) : "memory", "cc");
}

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

    // 5) GET_DESCRIPTOR (config) -> classe d'interface + endpoints
    uint8_t iclass = 0, iproto = 0;
    uint8_t ep_addr = 0; uint16_t ep_mps = 8; uint8_t ep_interval = 6;
    uint8_t bin_addr = 0, bout_addr = 0; uint16_t bin_mps = 512, bout_mps = 512;
    memset(buf, 0, 64);
    if (control_in(ep0_ring, ep0_phys, &ep0_idx, &ep0_cycle, slot,
                   0x80, 6, 0x0200, 0, 64, buf_phys) == 0) {
        // parcourt les descripteurs : Interface (type 4) puis Endpoint (type 5)
        int total = buf[2] | (buf[3] << 8); if (total > 64) total = 64;
        for (int o = buf[0]; o + 1 < total && buf[o]; o += buf[o]) {
            if (buf[o+1] == 4) {                          // Interface
                if (iclass == 0) { iclass = buf[o+5]; iproto = buf[o+7]; }
            } else if (buf[o+1] == 5) {                   // Endpoint
                uint8_t addr = buf[o+2], attr = buf[o+3];
                uint16_t mps = buf[o+4] | (buf[o+5] << 8);
                if ((attr & 3) == 3 && (addr & 0x80) && !ep_addr) {       // Interrupt IN
                    ep_addr = addr; ep_mps = mps; ep_interval = buf[o+6];
                } else if ((attr & 3) == 2) {                             // Bulk
                    if ((addr & 0x80) && !bin_addr)  { bin_addr = addr;  bin_mps = mps; }
                    if (!(addr & 0x80) && !bout_addr) { bout_addr = addr; bout_mps = mps; }
                }
            }
        }
    }

    if (dev_count < 16) {
        usb_dev_t *d = &devs[dev_count++];
        d->slot = slot; d->port = port; d->speed = speed;
        d->vendor = vid; d->product = pid; d->dev_class = dclass; d->if_class = iclass;
    }
    kprintf("[xhci] port %d : peripherique %x:%x classe=%x (slot %d)\n",
            port, vid, pid, iclass ? iclass : dclass, slot);

    // memorise l'etat du peripherique (pour la configuration HID ulterieure)
    if (slot < 16) {
        xdev_t *x = &xdevs[slot];
        x->used = true; x->slot = slot; x->speed = speed; x->port = port;
        x->ep0 = ep0_ring; x->ep0_phys = ep0_phys; x->ep0_idx = ep0_idx; x->ep0_cycle = ep0_cycle;
        x->inctx = inctx; x->in_phys = in_phys;
        x->mx = (int32_t)fb_width() / 2; x->my = (int32_t)fb_height() / 2;
        if (iclass == 3 && ep_addr) {                     // HID avec endpoint d'interruption
            x->is_hid = true;
            x->is_kbd = (iproto == 1);
            x->is_mouse = (iproto == 2);
            x->ep_dci = (uint8_t)(2 * (ep_addr & 0x0F) + 1); // EP num IN -> DCI
            x->ep_mps = ep_mps; x->ep_interval = ep_interval;
        } else if (iclass == 8 && bin_addr && bout_addr) { // stockage de masse (bulk IN+OUT)
            x->is_msc = true;
            x->bin_dci  = (uint8_t)(2 * (bin_addr  & 0x0F) + 1); // IN  -> DCI impair
            x->bout_dci = (uint8_t)(2 * (bout_addr & 0x0F));     // OUT -> DCI pair
            x->bin_mps = bin_mps; x->bout_mps = bout_mps;
        }
    }
}

// =============================================================================
//  HID : configuration de l'endpoint d'interruption + scrutation des rapports
// =============================================================================

// Transfert de controle SANS etape de donnees (ex: SET_CONFIGURATION).
static int control_nodata(xdev_t *x, uint8_t bmReq, uint8_t bReq,
                          uint16_t wValue, uint16_t wIndex) {
    uint64_t setup = (uint64_t)bmReq | ((uint64_t)bReq << 8) | ((uint64_t)wValue << 16)
                   | ((uint64_t)wIndex << 32);
    int i = x->ep0_idx; uint8_t cy = x->ep0_cycle;
    x->ep0[i].param = setup; x->ep0[i].status = 8;
    x->ep0[i].control = TRB_TYPE(TR_SETUP) | (0u << 16) /*TRT=No Data*/ | (1u << 6) /*IDT*/ | cy;
    i++;
    x->ep0[i].param = 0; x->ep0[i].status = 0;
    x->ep0[i].control = TRB_TYPE(TR_STATUS) | (1u << 16) /*DIR=IN*/ | (1u << 5) /*IOC*/ | cy;
    i++;
    if (i >= RING_SZ - 1) {
        x->ep0[RING_SZ-1].param = x->ep0_phys; x->ep0[RING_SZ-1].status = 0;
        x->ep0[RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1 << 1) | cy;
        i = 0; cy ^= 1;
    }
    x->ep0_idx = i; x->ep0_cycle = cy;
    db[x->slot] = 1;
    trb_t e;
    for (int skip = 0; skip < 32; skip++) {
        if (!next_event(&e, 60000)) return -1;
        if (((e.control >> 10) & 0x3F) == TR_TRANSFER) {
            uint8_t cc = (e.status >> 24) & 0xFF;
            return (cc == 1 || cc == 13) ? 0 : -1;
        }
    }
    return -1;
}

// Arme un transfert d'interruption IN (le contrôleur le complete a l'arrivee
// d'un rapport HID).
static void queue_int_in(xdev_t *x) {
    int i = x->ep_idx; uint8_t cy = x->ep_cycle;
    x->ep_ring[i].param = x->rbuf_phys;
    x->ep_ring[i].status = x->ep_mps;
    x->ep_ring[i].control = TRB_TYPE(TR_NORMAL) | (1u << 5) /*IOC*/ | (1u << 2) /*ISP*/ | cy;
    i++;
    if (i >= RING_SZ - 1) {
        x->ep_ring[RING_SZ-1].param = x->ep_ring_phys; x->ep_ring[RING_SZ-1].status = 0;
        x->ep_ring[RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1 << 1) | cy;
        i = 0; cy ^= 1;
    }
    x->ep_idx = i; x->ep_cycle = cy;
    db[x->slot] = x->ep_dci;
}

// Configure un peripherique HID : SET_CONFIGURATION, protocole boot, Configure
// Endpoint (endpoint d'interruption), puis amorce la 1re scrutation.
static void hid_setup(xdev_t *x) {
    // SET_CONFIGURATION(1)
    if (control_nodata(x, 0x00, 9, 1, 0) != 0) {
        kprintf("[hid] slot %d : SET_CONFIGURATION echec\n", x->slot); return;
    }
    // SET_PROTOCOL(boot=0) sur l'interface 0 (classe HID) -- rapports "boot".
    control_nodata(x, 0x21, 0x0B, 0, 0);

    // anneau de transfert pour l'endpoint + tampon de rapport
    x->ep_ring = dma_zeroed(4096, &x->ep_ring_phys); x->ep_idx = 0; x->ep_cycle = 1;
    x->rbuf    = dma_zeroed(64, &x->rbuf_phys);
    if (!x->ep_ring || !x->rbuf) return;

    // Input Context : ajoute slot (A0) + l'endpoint (A[dci])
    memset(x->inctx, 0, 2048);
    *(uint32_t *)(x->inctx + 4) = 0x1u | (1u << x->ep_dci);     // Add flags
    uint32_t *slotc = (uint32_t *)(x->inctx + 32);
    slotc[0] = ((uint32_t)x->ep_dci << 27) | ((uint32_t)x->speed << 20);  // context entries
    slotc[1] = (uint32_t)x->port << 16;
    // EP context de l'endpoint d'interruption IN
    uint32_t *epc = (uint32_t *)(x->inctx + 32 + (uint32_t)x->ep_dci * 32);
    uint8_t interval = x->ep_interval ? x->ep_interval : 6;
    epc[0] = ((uint32_t)interval << 16);
    epc[1] = (7u << 3) /*type=Interrupt IN*/ | ((uint32_t)x->ep_mps << 16) | (3u << 1) /*CErr*/;
    *(uint64_t *)(epc + 2) = x->ep_ring_phys | 1;              // TR dequeue | DCS
    epc[4] = x->ep_mps;                                        // Average TRB Length

    if (cmd_exec(x->in_phys, TRB_TYPE(TR_CONFIG_EP) | ((uint32_t)x->slot << 24), 0) != 1) {
        kprintf("[hid] slot %d : Configure Endpoint echec\n", x->slot); return;
    }
    queue_int_in(x);
    kprintf("[hid] slot %d pret : %s (dci=%d mps=%d)\n", x->slot,
            x->is_kbd ? "clavier" : x->is_mouse ? "souris" : "HID", x->ep_dci, x->ep_mps);
}

// Traduit un rapport clavier "boot" (8 octets) en evenements clavier.
static void hid_kbd_report(xdev_t *x) {
    uint8_t *r = x->rbuf;
    uint8_t mod = r[0];
    uint8_t mods = 0;
    if (mod & 0x22) mods |= MOD_SHIFT;        // L/R Shift
    if (mod & 0x11) mods |= MOD_CTRL;         // L/R Ctrl
    if (mod & 0x44) mods |= MOD_ALT;          // L/R Alt
    for (int k = 2; k < 8; k++) {
        uint8_t u = r[k];
        if (u == 0 || u == 1) continue;       // 0=vide, 1=ErrorRollOver
        // touche deja enfoncee au rapport precedent ? alors ce n'est pas un appui neuf
        bool was = false;
        for (int p = 2; p < 8; p++) if (x->prev[p] == u) { was = true; break; }
        if (was) continue;

        event_t e = {0}; e.type = EV_KEY; e.pressed = true; e.mods = mods;
        switch (u) {
            case 0x28: e.key = KEY_ENTER; break;
            case 0x29: e.key = KEY_ESC; break;
            case 0x2A: e.key = KEY_BACKSPACE; break;
            case 0x2B: e.key = KEY_TAB; break;
            case 0x4F: e.key = KEY_RIGHT; break;
            case 0x50: e.key = KEY_LEFT; break;
            case 0x51: e.key = KEY_DOWN; break;
            case 0x52: e.key = KEY_UP; break;
            case 0x4A: e.key = KEY_HOME; break;
            case 0x4D: e.key = KEY_END; break;
            case 0x4C: e.key = KEY_DELETE; break;
            case 0x4B: e.key = KEY_PAGEUP; break;
            case 0x4E: e.key = KEY_PAGEDOWN; break;
            default: {
                char c = (mods & MOD_SHIFT) ? hid2az_shift[u] : hid2az[u];
                if (!c) continue;
                e.ch = c;
            }
        }
        input_push(&e);
    }
    for (int k = 0; k < 8; k++) x->prev[k] = r[k];
}

// Traduit un rapport souris "boot" (>=3 octets) en evenement souris.
static void hid_mouse_report(xdev_t *x) {
    uint8_t *r = x->rbuf;
    uint8_t btn = r[0];
    int dx = (int8_t)r[1];
    int dy = (int8_t)r[2];
    x->mx += dx; x->my += dy;
    if (x->mx < 0) x->mx = 0;
    if (x->my < 0) x->my = 0;
    if (x->mx >= (int32_t)fb_width())  x->mx = fb_width() - 1;
    if (x->my >= (int32_t)fb_height()) x->my = fb_height() - 1;

    event_t e = {0};
    e.type = EV_MOUSE;
    e.mx = x->mx; e.my = x->my;
    e.dx = dx; e.dy = dy;
    e.buttons = (btn & 1 ? MOUSE_LEFT : 0) | (btn & 2 ? MOUSE_RIGHT : 0) | (btn & 4 ? MOUSE_MIDDLE : 0);
    input_push(&e);
}

// Traite un Transfer Event venant d'un endpoint HID : parse le rapport et
// re-arme la scrutation. Renvoie true si l'evenement a ete consomme ici.
static bool dispatch_hid(uint8_t slot) {
    if (slot >= 16 || !xdevs[slot].used || !xdevs[slot].is_hid) return false;
    xdev_t *x = &xdevs[slot];
    if (x->is_kbd)        hid_kbd_report(x);
    else if (x->is_mouse) hid_mouse_report(x);
    queue_int_in(x);
    return true;
}

// Vide l'anneau d'evenements et distribue les rapports HID (non bloquant).
static void usb_poll(void) {
    if (!xhci_ok) return;
    uint64_t fl = irq_save();
    for (int n = 0; n < 64; n++) {
        trb_t e;
        if (!next_event(&e, 1)) break;        // 1 seule tentative : non bloquant
        if (((e.control >> 10) & 0x3F) != TR_TRANSFER) continue;
        dispatch_hid((e.control >> 24) & 0xFF);
    }
    irq_restore(fl);
}

// Tache noyau : scrute les peripheriques HID en continu.
void usb_task(void) {
    for (;;) { usb_poll(); pit_sleep_ms(4); }
}

// =============================================================================
//  Stockage de masse USB : Bulk-Only Transport (BOT) + commandes SCSI
// =============================================================================
typedef struct {
    bool     ok;
    uint8_t  slot, in_dci, out_dci;
    trb_t   *in_ring;  uint64_t in_ring_phys;  int in_idx;  uint8_t in_cycle;
    trb_t   *out_ring; uint64_t out_ring_phys; int out_idx; uint8_t out_cycle;
    uint8_t *cbw;  uint64_t cbw_phys;     // Command Block Wrapper (31 o)
    uint8_t *csw;  uint64_t csw_phys;     // Command Status Wrapper (13 o)
    uint8_t *data; uint64_t data_phys;    // tampon DMA (4 Kio)
    uint32_t block_size, block_count;
    uint32_t tag;
} msc_t;
static msc_t msc;
#define MSC_BOUNCE 4096

bool     usb_msc_present(void)    { return msc.ok; }
uint32_t usb_msc_blocks(void)     { return msc.block_count; }
uint32_t usb_msc_block_size(void) { return msc.block_size; }

// Transfert "bulk" : place un Normal TRB sur l'anneau d'un endpoint, sonne, et
// attend le Transfer Event correspondant (en traitant les rapports HID croises).
// Renvoie le completion code (1=Success, 13=Short Packet, 0=timeout).
static int bulk_xfer(trb_t *ring, uint64_t ring_phys, int *idx, uint8_t *cycle,
                     uint8_t dci, uint64_t phys, uint32_t len) {
    int i = *idx; uint8_t cy = *cycle;
    ring[i].param = phys;
    ring[i].status = len;                          // TRB transfer length
    ring[i].control = TRB_TYPE(TR_NORMAL) | (1u << 5) /*IOC*/ | (1u << 2) /*ISP*/ | cy;
    i++;
    if (i >= RING_SZ - 1) {
        ring[RING_SZ-1].param = ring_phys; ring[RING_SZ-1].status = 0;
        ring[RING_SZ-1].control = TRB_TYPE(TR_LINK) | (1 << 1) | cy;
        i = 0; cy ^= 1;
    }
    *idx = i; *cycle = cy;
    db[msc.slot] = dci;
    for (int skip = 0; skip < 64; skip++) {
        trb_t e;
        if (!next_event(&e, 300000)) return 0;
        if (((e.control >> 10) & 0x3F) != TR_TRANSFER) continue;
        uint8_t es = (e.control >> 24) & 0xFF;
        uint8_t ed = (e.control >> 16) & 0x1F;
        if (es == msc.slot && ed == dci) return (e.status >> 24) & 0xFF;
        dispatch_hid(es);                          // evenement HID croise -> traite
    }
    return 0;
}
static inline bool xfer_ok(int cc) { return cc == 1 || cc == 13; }

// Execute une commande SCSI via BOT. dir : 0=aucune donnee, 1=IN, 2=OUT.
// Les donnees transitent par le tampon msc.data. Renvoie le bCSWStatus (0=OK).
static int bot_command(const uint8_t *cmd, int cmd_len, int dir, uint32_t data_len) {
    uint8_t *c = msc.cbw;
    memset(c, 0, 31);
    *(uint32_t *)(c + 0) = 0x43425355;             // dCBWSignature 'USBC'
    *(uint32_t *)(c + 4) = ++msc.tag;              // dCBWTag
    *(uint32_t *)(c + 8) = data_len;               // dCBWDataTransferLength
    c[12] = (dir == 1) ? 0x80 : 0x00;              // bmCBWFlags
    c[13] = 0;                                     // bCBWLUN
    c[14] = (uint8_t)cmd_len;                      // bCBWCBLength
    memcpy(c + 15, cmd, cmd_len);

    if (!xfer_ok(bulk_xfer(msc.out_ring, msc.out_ring_phys, &msc.out_idx, &msc.out_cycle,
                           msc.out_dci, msc.cbw_phys, 31))) return -1;
    if (data_len) {
        if (dir == 1) {
            if (!xfer_ok(bulk_xfer(msc.in_ring, msc.in_ring_phys, &msc.in_idx, &msc.in_cycle,
                                   msc.in_dci, msc.data_phys, data_len))) return -1;
        } else {
            if (!xfer_ok(bulk_xfer(msc.out_ring, msc.out_ring_phys, &msc.out_idx, &msc.out_cycle,
                                   msc.out_dci, msc.data_phys, data_len))) return -1;
        }
    }
    if (!xfer_ok(bulk_xfer(msc.in_ring, msc.in_ring_phys, &msc.in_idx, &msc.in_cycle,
                           msc.in_dci, msc.csw_phys, 13))) return -1;
    uint8_t *s = msc.csw;
    if (*(uint32_t *)(s + 0) != 0x53425355) return -1;   // dCSWSignature 'USBS'
    return s[12];                                  // bCSWStatus (0 = succes)
}

// lit/ecrit `count` secteurs (memoire NOYAU). Serialise avec la tache HID.
int usb_msc_read(uint32_t lba, uint32_t count, void *dst) {
    if (!msc.ok || !dst) return -1;
    uint32_t per = MSC_BOUNCE / msc.block_size; if (!per) per = 1;
    uint64_t fl = irq_save();
    uint8_t *out = (uint8_t *)dst; int rc = 0;
    while (count) {
        uint32_t n = count > per ? per : count;
        uint8_t cmd[16]; memset(cmd, 0, 16);
        cmd[0] = 0x28;                             // READ(10)
        cmd[2] = lba >> 24; cmd[3] = lba >> 16; cmd[4] = lba >> 8; cmd[5] = lba;
        cmd[7] = n >> 8; cmd[8] = n;
        if (bot_command(cmd, 10, 1, n * msc.block_size) != 0) { rc = -1; break; }
        memcpy(out, msc.data, n * msc.block_size);
        out += n * msc.block_size; lba += n; count -= n;
    }
    irq_restore(fl);
    return rc;
}
int usb_msc_write(uint32_t lba, uint32_t count, const void *src) {
    if (!msc.ok || !src) return -1;
    uint32_t per = MSC_BOUNCE / msc.block_size; if (!per) per = 1;
    uint64_t fl = irq_save();
    const uint8_t *in = (const uint8_t *)src; int rc = 0;
    while (count) {
        uint32_t n = count > per ? per : count;
        memcpy(msc.data, in, n * msc.block_size);
        uint8_t cmd[16]; memset(cmd, 0, 16);
        cmd[0] = 0x2A;                             // WRITE(10)
        cmd[2] = lba >> 24; cmd[3] = lba >> 16; cmd[4] = lba >> 8; cmd[5] = lba;
        cmd[7] = n >> 8; cmd[8] = n;
        if (bot_command(cmd, 10, 2, n * msc.block_size) != 0) { rc = -1; break; }
        in += n * msc.block_size; lba += n; count -= n;
    }
    irq_restore(fl);
    return rc;
}

// Configure un peripherique de stockage de masse : endpoints bulk + SCSI.
static void msc_setup(xdev_t *x) {
    if (msc.ok) return;                            // un seul disque USB gere
    if (control_nodata(x, 0x00, 9, 1, 0) != 0) {   // SET_CONFIGURATION(1)
        kprintf("[msc] SET_CONFIGURATION echec\n"); return;
    }
    msc.slot = x->slot; msc.in_dci = x->bin_dci; msc.out_dci = x->bout_dci;
    msc.in_ring  = dma_zeroed(4096, &msc.in_ring_phys);  msc.in_idx = 0;  msc.in_cycle = 1;
    msc.out_ring = dma_zeroed(4096, &msc.out_ring_phys); msc.out_idx = 0; msc.out_cycle = 1;
    msc.cbw  = dma_zeroed(64, &msc.cbw_phys);
    msc.csw  = dma_zeroed(64, &msc.csw_phys);
    msc.data = dma_zeroed(MSC_BOUNCE, &msc.data_phys);
    if (!msc.in_ring || !msc.out_ring || !msc.cbw || !msc.csw || !msc.data) return;

    // Configure Endpoint : ajoute le bulk OUT (type 2) et le bulk IN (type 6)
    memset(x->inctx, 0, 2048);
    uint32_t maxdci = msc.in_dci > msc.out_dci ? msc.in_dci : msc.out_dci;
    *(uint32_t *)(x->inctx + 4) = 0x1u | (1u << msc.in_dci) | (1u << msc.out_dci);
    uint32_t *slotc = (uint32_t *)(x->inctx + 32);
    slotc[0] = (maxdci << 27) | ((uint32_t)x->speed << 20);
    slotc[1] = (uint32_t)x->port << 16;
    uint32_t *oc = (uint32_t *)(x->inctx + 32 + (uint32_t)msc.out_dci * 32);
    oc[1] = (2u << 3) | ((uint32_t)x->bout_mps << 16) | (3u << 1);
    *(uint64_t *)(oc + 2) = msc.out_ring_phys | 1;
    oc[4] = x->bout_mps;
    uint32_t *ic = (uint32_t *)(x->inctx + 32 + (uint32_t)msc.in_dci * 32);
    ic[1] = (6u << 3) | ((uint32_t)x->bin_mps << 16) | (3u << 1);
    *(uint64_t *)(ic + 2) = msc.in_ring_phys | 1;
    ic[4] = x->bin_mps;
    if (cmd_exec(x->in_phys, TRB_TYPE(TR_CONFIG_EP) | ((uint32_t)x->slot << 24), 0) != 1) {
        kprintf("[msc] Configure Endpoint echec\n"); return;
    }

    // SCSI : TEST UNIT READY (quelques essais), puis READ CAPACITY(10)
    uint8_t cmd[16];
    for (int t = 0; t < 5; t++) {
        memset(cmd, 0, 16); cmd[0] = 0x00;          // TEST UNIT READY
        if (bot_command(cmd, 6, 0, 0) == 0) break;
        memset(cmd, 0, 16); cmd[0] = 0x03; cmd[4] = 18;  // REQUEST SENSE
        bot_command(cmd, 6, 1, 18);
        spin(500000);
    }
    memset(cmd, 0, 16); cmd[0] = 0x25;              // READ CAPACITY(10)
    if (bot_command(cmd, 10, 1, 8) == 0) {
        uint8_t *d = msc.data;
        uint32_t last = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
        uint32_t bsz  = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
        msc.block_count = last + 1;
        msc.block_size  = bsz ? bsz : 512;
        msc.ok = true;
        kprintf("[msc] disque USB pret : %d secteurs de %d o (%d Mio)\n",
                msc.block_count, msc.block_size,
                (uint32_t)(((uint64_t)msc.block_count * msc.block_size) >> 20));
    } else {
        kprintf("[msc] READ CAPACITY echec\n");
    }
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

    // configure d'abord le stockage de masse (poignee de main SCSI sans HID),
    // puis les peripheriques HID (clavier / souris).
    for (int s = 1; s < 16; s++)
        if (xdevs[s].used && xdevs[s].is_msc) msc_setup(&xdevs[s]);
    for (int s = 1; s < 16; s++)
        if (xdevs[s].used && xdevs[s].is_hid) hid_setup(&xdevs[s]);
}
