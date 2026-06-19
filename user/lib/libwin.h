// =============================================================================
//  user/lib/libwin.h -- Bibliothèque cliente du compositeur (ring 3)
// =============================================================================
#ifndef MONOS_LIBWIN_H
#define MONOS_LIBWIN_H

#include "gfx.h"
#include "input.h"

// Crée une fenêtre auprès du compositeur ; renvoie un canvas (mémoire partagée)
// où dessiner, ou 0 en cas d'échec.
canvas_t *win_create(int w, int h, const char *title);

// Signale au compositeur que le tampon a été redessiné (à recomposer).
void win_damage(void);

// Récupère un événement routé vers cette fenêtre.
//  1  = événement disponible (rempli dans *ev)
//  0  = rien pour l'instant
//  -1 = le compositeur demande la fermeture (l'app doit quitter)
int win_poll(event_t *ev);

// Bloque jusqu'à un événement (l'appli dort en attendant). Mêmes codes de retour
// que win_poll, mais ne renvoie jamais 0.
int win_wait(event_t *ev);

#endif
