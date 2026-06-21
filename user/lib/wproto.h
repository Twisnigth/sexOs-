// =============================================================================
//  user/lib/wproto.h -- Protocole compositeur <-> applications (IPC)
// =============================================================================
#ifndef SEXOS_WPROTO_H
#define SEXOS_WPROTO_H

#include "input.h"

enum {
    WMSG_CREATE  = 1,   // app -> comp : {w, h, title}
    WMSG_DAMAGE  = 2,   // app -> comp : {win} (le tampon a été redessiné)
    WMSG_DESTROY = 3,   // app -> comp : {win}
    WMSG_CREATED = 10,  // comp -> app : {win, shm, w, h}
    WMSG_EVENT   = 11,  // comp -> app : {win, ev} (entrée routée vers le focus)
    WMSG_CLOSE   = 12,  // comp -> app : {win} (l'utilisateur a cliqué fermer)
    WMSG_RESIZE  = 13,  // comp -> app : {win, shm, w, h} (nouvelle taille + tampon)
    WMSG_LAUNCH  = 14,  // app -> comp : {w = app_id} (demande de lancer une appli)
};

typedef struct {
    int     type;
    int     win;        // identifiant de fenêtre
    int     shm;        // identifiant de mémoire partagée (WMSG_CREATED)
    int     w, h;       // dimensions
    int     flags;      // WMSG_CREATE : bit 0 = fenêtre redimensionnable
    event_t ev;         // pour WMSG_EVENT
    char    title[32];  // pour WMSG_CREATE
} wmsg_t;

#define WIN_RESIZABLE 1

#endif
