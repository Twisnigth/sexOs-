// =============================================================================
//  kernel/ssh.h -- Client SSH-2 (transport + KEX curve25519-sha256)
// =============================================================================
#ifndef SEXOS_SSH_H
#define SEXOS_SSH_H

#include "net.h"

// Effectue le handshake SSH (versions, KEXINIT, KEX, vérification d'hôte,
// dérivation des clés) avec le serveur (ip:port). Renvoie 0 si tout réussit.
//  Sert de jalon de test pour la couche transport.
int ssh_handshake_test(ip4_t ip, uint16_t port);

// Client SSH complet : s'authentifie (mot de passe) et exécute 'command' sur
// le serveur distant ; écrit la sortie dans out (max outmax). Renvoie sa taille.
int ssh_client_exec(ip4_t ip, uint16_t port, const char *user, const char *password,
                    const char *command, char *out, int outmax);

// Serveur SSH : génère la clé d'hôte et écoute le port 22.
void ssh_server_init(void);
// Tâche noyau sshd : accepte et sert les connexions (sched_new_kernel_task).
void sshd_run(void);
// Clé publique d'hôte (ed25519, 32 octets) — pour l'afficher (fingerprint).
const uint8_t *ssh_host_pubkey(void);

// Texte de la clé publique d'hôte + empreinte SHA256 (pour le terminal local).
int ssh_hostkey_text(char *out, int max);
// Génère une paire ed25519, l'ajoute aux clés autorisées de 'user' et écrit la
// clé publique + la clé privée (format OpenSSH) dans out. Renvoie la taille.
int ssh_keygen_text(const char *user, char *out, int max);

#endif
