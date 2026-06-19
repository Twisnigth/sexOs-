// =============================================================================
//  user/lib/x509.h -- Analyse de certificats X.509 (DER) + vérification (RSA)
// -----------------------------------------------------------------------------
//  Sous-ensemble suffisant pour TLS : extraction de la clé publique RSA, des
//  dates, du SAN (noms DNS), des DN, et vérification de la signature d'un
//  certificat par un autre (chaînage). ECDSA non géré (signalé).
// =============================================================================
#ifndef MONOS_X509_H
#define MONOS_X509_H

#include <stdint.h>
#include "bigint.h"

enum { SIGALG_UNKNOWN = 0, SIGALG_RSA_PKCS1_SHA256, SIGALG_RSA_PSS_SHA256, SIGALG_ECDSA_SHA256 };

typedef struct {
    const uint8_t *tbs;      int tbs_len;     // portion signée (TBSCertificate)
    const uint8_t *sig;      int sig_len;     // valeur de signature
    int            sig_alg;                   // algo de signature du certificat
    int            pub_is_rsa;
    bn_t           pub_n;    const uint8_t *pub_e; int pub_e_len;   // clé publique RSA
    const uint8_t *subject;  int subject_len; // DN (SEQUENCE brute)
    const uint8_t *issuer;   int issuer_len;
    const uint8_t *spki;     int spki_len;    // SubjectPublicKeyInfo brut
    const uint8_t *san;      int san_len;     // SubjectAltName brut (OCTET STRING)
    uint64_t       not_before, not_after;     // AAAAMMJJhhmmss
} x509_cert;

int x509_parse(const uint8_t *der, int len, x509_cert *c);          // 0=ok / -1
int x509_verify_signed_by(const x509_cert *child, const x509_cert *iss); // 1/0
int x509_dn_equal(const uint8_t *a, int al, const uint8_t *b, int bl);   // 1/0
int x509_check_host(const x509_cert *c, const char *host);          // 1/0

#endif
