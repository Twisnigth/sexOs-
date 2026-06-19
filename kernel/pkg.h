// =============================================================================
//  kernel/pkg.h -- Gestionnaire de paquets natif (style pacman)
// =============================================================================
#ifndef SEXOS_PKG_H
#define SEXOS_PKG_H

#include <stdint.h>

// Sortie texte (vers le terminal). Renvoie 0 si succès.
typedef void (*pkg_out_t)(const char *s);

void pkg_set_output(pkg_out_t fn);

// Configure l'adresse du dépôt HTTP (par défaut 10.0.2.2:8000, hôte QEMU).
void pkg_set_repo(uint32_t ip, uint16_t port);

int pkg_sync(void);                       // pacman -Sy  (télécharge la base du dépôt)
int pkg_install(const char *name);        // pacman -S <pkg>
int pkg_remove(const char *name);         // pacman -R <pkg>
int pkg_query(void);                      // pacman -Q   (liste les paquets installés)
int pkg_upgrade(void);                    // pacman -Syu

#endif
