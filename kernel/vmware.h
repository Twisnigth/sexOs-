// =============================================================================
//  kernel/vmware.h -- Intégration hôte VMware (canal « backdoor », port 0x5658)
// -----------------------------------------------------------------------------
//  Fournit le presse-papiers partagé hôte <-> invité via le mécanisme historique
//  GETSEL/SETSEL du backdoor VMware (texte). Inactif hors VMware.
// =============================================================================
#ifndef SEXOS_VMWARE_H
#define SEXOS_VMWARE_H

#include <stdbool.h>

bool vmware_present(void);   // vrai si on tourne sous VMware (backdoor répond)
void vmware_init(void);      // détecte VMware et lance la synchro du presse-papiers

#endif
