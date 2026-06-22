// =============================================================================
//  kernel/usb.h -- Pile USB (controleur xHCI + enumeration)
// =============================================================================
#ifndef SEXOS_USB_H
#define SEXOS_USB_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  slot;
    uint8_t  port;
    uint8_t  speed;          // 1=FS 2=LS 3=HS 4=SS
    uint16_t vendor, product;
    uint8_t  dev_class;      // bDeviceClass (souvent 0)
    uint8_t  if_class;       // classe de la 1re interface (HID=3, stockage=8, hub=9)
    char     name[40];       // nom produit (descripteur string iProduct)
} usb_dev_t;

void  usb_init(void);                       // detecte le xHCI et enumere
void  usb_task(void);                        // tache noyau : scrute les HID (clavier/souris)
int   usb_count(void);
const usb_dev_t *usb_get(int i);

const char *usb_diagnostic(void);           // resume lisible de l'etat USB (pour l'ecran)

// --- Stockage de masse USB (Bulk-Only Transport + SCSI) ----------------------
bool     usb_msc_present(void);              // un disque USB est-il pret ?
uint32_t usb_msc_blocks(void);              // nombre de secteurs
uint32_t usb_msc_block_size(void);          // taille d'un secteur (octets)
int      usb_msc_read(uint32_t lba, uint32_t count, void *dst);        // 0/-1
int      usb_msc_write(uint32_t lba, uint32_t count, const void *src); // 0/-1

#endif
