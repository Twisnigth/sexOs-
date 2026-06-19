// =============================================================================
//  user/lib/ecdsa.h -- Vérification ECDSA P-256 (secp256r1, SHA-256)
// =============================================================================
#ifndef MONOS_ECDSA_H
#define MONOS_ECDSA_H

#include <stdint.h>

// Vérifie une signature ECDSA P-256 (DER SEQUENCE{ r INTEGER, s INTEGER })
// avec la clé publique (Qx,Qy en 32 octets gros-boutistes) sur 'hash' (32 o).
// Renvoie 1 si valide, 0 sinon.
int ecdsa_p256_verify(const uint8_t Qx[32], const uint8_t Qy[32],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[32]);

#endif
