// =============================================================================
//  user/lib/ecdsa.h -- Vérification ECDSA P-256 (SHA-256) et P-384 (SHA-384)
// =============================================================================
#ifndef MONOS_ECDSA_H
#define MONOS_ECDSA_H

#include <stdint.h>

// Signature DER SEQUENCE{ r, s }. Clé publique (Qx,Qy) gros-boutiste : 32 octets
// pour P-256, 48 pour P-384. 'hash' = 32 (SHA-256) ou 48 octets (SHA-384).
int ecdsa_p256_verify(const uint8_t Qx[32], const uint8_t Qy[32],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[32]);
int ecdsa_p384_verify(const uint8_t Qx[48], const uint8_t Qy[48],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[48]);

#endif
