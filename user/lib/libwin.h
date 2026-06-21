// =============================================================================
//  user/lib/libwin.h -- Bibliothèque cliente du compositeur (ring 3)
// =============================================================================
#ifndef SEXOS_LIBWIN_H
#define SEXOS_LIBWIN_H

#include "gfx.h"
#include "input.h"

// Crée une fenêtre auprès du compositeur ; renvoie un canvas (mémoire partagée)
// où dessiner, ou 0 en cas d'échec.
canvas_t *win_create(int w, int h, const char *title);

// À appeler AVANT win_create pour autoriser le redimensionnement (maximiser +
// poignée). Le canvas change de taille via le code de retour 2 de win_poll/wait.
void win_set_resizable(int on);

// Signale au compositeur que le tampon a été redessiné (à recomposer).
void win_damage(void);

// Récupère un événement routé vers cette fenêtre.
//  1  = événement disponible (rempli dans *ev)
//  0  = rien pour l'instant
//  -1 = le compositeur demande la fermeture (l'app doit quitter)
//  2  = la fenêtre a été redimensionnée (le canvas a changé : se redessiner)
int win_poll(event_t *ev);

// Bloque jusqu'à un événement (l'appli dort en attendant). Mêmes codes de retour
// que win_poll, mais ne renvoie jamais 0.
int win_wait(event_t *ev);

#endif
