// =============================================================================
//  kernel/ssh.h -- Client SSH-2 (transport + KEX curve25519-sha256)
// =============================================================================
#ifndef MONOS_SSH_H
#define MONOS_SSH_H

#include "net.h"

// Effectue le handshake SSH (versions, KEXINIT, KEX, vérification d'hôte,
// dérivation des clés) avec le serveur (ip:port). Renvoie 0 si tout réussit.
//  Sert de jalon de test pour la couche transport.
int ssh_handshake_test(ip4_t ip, uint16_t port);

// Client SSH complet : s'authentifie (mot de passe) et exécute 'command' sur
// le serveur distant ; écrit la sortie dans out (max outmax). Renvoie sa taille.
int ssh_client_exec(ip4_t ip, uint16_t port, const char *user, const char *password,
                    const char *command, char *out, int outmax);

#endif
