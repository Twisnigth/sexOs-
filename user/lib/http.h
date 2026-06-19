// =============================================================================
//  user/lib/http.h -- Client HTTP/1.1 minimal pour ring 3 (au-dessus des sockets)
// =============================================================================
#ifndef MONOS_HTTP_H
#define MONOS_HTTP_H

#include <stdint.h>

// Récupère une URL "http://hôte[:port]/chemin" (hôte = nom DNS ou IP a.b.c.d).
// Écrit le CORPS de la réponse dans body (au plus maxbody octets) et renvoie sa
// longueur, ou -1 en cas d'échec. *status reçoit le code HTTP, *err un message
// court (ou NULL). N'utilise QUE des appels système (sockets non bloquantes) :
// l'attente se fait par sys_yield (coopératif), donc le reste du bureau continue.
int http_fetch(const char *url, char *body, int maxbody, int *status, const char **err);

// État TLS de la dernière requête (rempli par http_fetch).
extern int  http_last_secure;     // 1 si https
extern int  http_last_verified;   // 1 si certificat vérifié
extern char http_last_vinfo[72];  // détail de la vérification

#endif
