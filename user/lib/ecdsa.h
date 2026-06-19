// =============================================================================
//  user/lib/ecdsa.h -- Vérification ECDSA P-256 (SHA-256) et P-384 (SHA-384)
// =============================================================================
#ifndef SEXOS_ECDSA_H
#define SEXOS_ECDSA_H

#include <stdint.h>

// Signature DER SEQUENCE{ r, s }. Clé publique (Qx,Qy) gros-boutiste : 32 octets
// pour P-256, 48 pour P-384. 'hash' = 32 (SHA-256) ou 48 octets (SHA-384).
int ecdsa_p256_verify(const uint8_t Qx[32], const uint8_t Qy[32],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[32]);
int ecdsa_p384_verify(const uint8_t Qx[48], const uint8_t Qy[48],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[48]);

// --- ECDHE P-256 (échange de clés TLS) ---------------------------------------
// Clé publique = 04 || X || Y (65 octets) à partir de la clé privée (32 octets).
void ec_p256_pub(const uint8_t priv[32], uint8_t pub[65]);
// Secret partagé = X(priv * point_pair) ; 'peer' = 04||X||Y. 0=OK / -1=erreur.
int  ec_p256_ecdh(const uint8_t priv[32], const uint8_t peer[65], uint8_t out[32]);

#endif
