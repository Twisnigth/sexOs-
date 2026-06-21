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
} usb_dev_t;

void  usb_init(void);                       // detecte le xHCI et enumere
void  usb_task(void);                        // tache noyau : scrute les HID (clavier/souris)
int   usb_count(void);
const usb_dev_t *usb_get(int i);

#endif
