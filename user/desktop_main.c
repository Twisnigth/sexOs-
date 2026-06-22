// =============================================================================
//  user/desktop_main.c -- Point d'entrée du BUREAU en ring 3 (init graphique)
// -----------------------------------------------------------------------------
//  Initialise la libOS (tas + framebuffer), monte le VFS et les comptes DANS le
//  processus, puis lance la boucle du bureau (desktop_run) — le tout en CPL 3.
// =============================================================================
#include "vfs.h"
#include "users.h"

extern void libos_init(void);
extern void desktop_run(void);

int main(void) {
    libos_init();      // tas + mappage du framebuffer
    vfs_init();        // arborescence fichiers (en mémoire, dans ce processus)
    users_init();      // comptes (root/user)
    desktop_run();     // boucle du compositeur + applications (ne revient pas)
    return 0;
}
