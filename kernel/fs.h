// =============================================================================
//  kernel/fs.h -- Persistance du VFS sur disque (instantane serialise)
// =============================================================================
#ifndef SEXOS_FS_H
#define SEXOS_FS_H

#include <stdbool.h>

void fs_init(void);    // detecte le disque puis restaure l'arborescence si presente
int  fs_save(void);    // ecrit l'arborescence courante du VFS sur le disque (0/-1)
bool fs_present(void); // un disque persistant est-il disponible ?

#endif
