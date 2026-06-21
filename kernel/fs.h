// =============================================================================
//  kernel/fs.h -- Persistance du VFS sur disque (instantane serialise)
// =============================================================================
#ifndef SEXOS_FS_H
#define SEXOS_FS_H

#include <stdbool.h>

void fs_init(void);    // detecte le disque puis restaure l'arborescence si presente
int  fs_save(void);    // ecrit l'arborescence courante du VFS sur le disque (0/-1)
bool fs_present(void); // un disque persistant est-il disponible ?

// --- Volumes montes sous /media (cle USB, disque SATA supplementaire) ---------
void fs_mount_volumes(void);  // monte au demarrage les volumes supplementaires
void usbfs_mount(void);       // monte la cle USB sur /media/usb (formate si vierge)
void usbfs_unmount(void);     // demonte /media/usb (cle retiree)
int  usbfs_sync(void);        // ecrit /media/usb sur la cle USB (0/-1)
bool usbfs_mounted(void);     // une cle USB est-elle montee ?
int  fs_persist(const char *path); // persiste selon le chemin (volume concerne, sinon disque)
void fs_sync_all(void);       // ecrit le disque systeme + tous les volumes montes

#endif
