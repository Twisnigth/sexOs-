// =============================================================================
//  user/lib/tls.h -- Client TLS 1.3 minimal pour ring 3 (au-dessus des sockets)
// -----------------------------------------------------------------------------
//  Suite unique : TLS_CHACHA20_POLY1305_SHA256, échange de clés X25519.
//  ATTENTION : le certificat du serveur n'est PAS vérifié (pas de RSA/ECDSA ni
//  de magasin d'autorités). La connexion est donc CHIFFREE mais NON AUTHENTIFIEE
//  (vulnérable à un homme du milieu). À ne pas utiliser pour des secrets réels.
// =============================================================================
#ifndef SEXOS_TLS_H
#define SEXOS_TLS_H

#include <stdint.h>

typedef struct {
    int      conn;                 // socket TCP sous-jacente
    uint8_t  cw_key[32], cw_iv[12]; uint64_t cw_seq;   // écriture application
    uint8_t  sr_key[32], sr_iv[12]; uint64_t sr_seq;   // lecture application
    int      established, eof;
    int      verified;             // 1 = certificat vérifié (chaîne + hôte + dates)
    char     verify_info[72];      // détail (raison si non vérifié)
    uint8_t  rbuf[20000];          // données applicatives déchiffrées
    int      rlen, roff;
} tls_t;

// Handshake TLS 1.3 sur 'conn' (socket déjà connectée) pour 'host' (SNI).
// 0 = succès ; -1 = échec (*err = message court ou NULL).
int  tls_handshake(tls_t *t, int conn, const char *host, const char **err);
int  tls_send(tls_t *t, const void *buf, int len);   // chiffre et envoie tout
int  tls_recv(tls_t *t, void *buf, int len);         // >0 / 0=rien / -1=fin
void tls_close(tls_t *t);

#endif
