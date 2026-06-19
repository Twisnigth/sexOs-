// =============================================================================
//  user/lib/x509.c -- Analyse DER / X.509 + vérification de signature (RSA)
// =============================================================================
#include "x509.h"
#include "rsa.h"
#include "crypto.h"

unsigned long strlen(const char *);

// --- Lecteur DER : un TLV (tag, longueur, valeur) ----------------------------
//  Avance *p après la valeur. Renvoie 0 en cas d'erreur (pointeurs invalides).
static int der_read(const uint8_t **p, const uint8_t *end, int *tag,
                    const uint8_t **val, int *vlen) {
    if (*p >= end) return 0;
    *tag = *(*p)++;
    if (*p >= end) return 0;
    int len = *(*p)++;
    if (len & 0x80) {
        int nb = len & 0x7F;
        if (nb == 0 || nb > 4) return 0;
        len = 0;
        for (int i = 0; i < nb; i++) { if (*p >= end) return 0; len = (len << 8) | *(*p)++; }
    }
    if (len < 0 || *p + len > end) return 0;
    *val = *p; *vlen = len; *p += len;
    return 1;
}

// Entre dans une SEQUENCE/SET et renvoie le curseur sur le contenu.
static int der_enter(const uint8_t **p, const uint8_t *end, int want_tag,
                     const uint8_t **inner, const uint8_t **inner_end) {
    int tag, vlen; const uint8_t *val;
    if (!der_read(p, end, &tag, &val, &vlen)) return 0;
    if (want_tag >= 0 && tag != want_tag) return 0;
    *inner = val; *inner_end = val + vlen;
    return 1;
}

// --- OIDs -------------------------------------------------------------------
static int oid_eq(const uint8_t *o, int ol, const uint8_t *ref, int rl) {
    if (ol != rl) return 0;
    for (int i = 0; i < ol; i++) if (o[i] != ref[i]) return 0;
    return 1;
}
static const uint8_t OID_RSA_PKCS1_SHA256[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_RSA_PSS[]          = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a};
static const uint8_t OID_ECDSA_SHA256[]     = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_RSA_ENC[]          = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const uint8_t OID_SAN[]              = {0x55,0x1d,0x11};   // 2.5.29.17

static int sigalg_from_oid(const uint8_t *o, int ol) {
    if (oid_eq(o, ol, OID_RSA_PKCS1_SHA256, sizeof OID_RSA_PKCS1_SHA256)) return SIGALG_RSA_PKCS1_SHA256;
    if (oid_eq(o, ol, OID_RSA_PSS, sizeof OID_RSA_PSS)) return SIGALG_RSA_PSS_SHA256;
    if (oid_eq(o, ol, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) return SIGALG_ECDSA_SHA256;
    return SIGALG_UNKNOWN;
}

// AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters ANY OPTIONAL }
static int read_algid(const uint8_t **p, const uint8_t *end, int *alg) {
    const uint8_t *in, *ie;
    if (!der_enter(p, end, 0x30, &in, &ie)) return 0;
    int tag, vlen; const uint8_t *val;
    if (!der_read(&in, ie, &tag, &val, &vlen) || tag != 0x06) return 0;   // OID
    *alg = sigalg_from_oid(val, vlen);
    return 1;
}

static int d2(const uint8_t *q) { return (q[0]-'0')*10 + (q[1]-'0'); }

// Convertit UTCTime / GeneralizedTime en AAAAMMJJhhmmss.
static uint64_t parse_time(int tag, const uint8_t *s, int n) {
    int i; uint64_t year;
    if (tag == 0x17) {                       // UTCTime : YY...
        int yy = d2(s); year = (yy < 50 ? 2000 : 1900) + yy; i = 2;
    } else {                                  // GeneralizedTime : YYYY...
        year = (uint64_t)d2(s) * 100 + d2(s + 2); i = 4;
    }
    int mo = d2(s+i), da = d2(s+i+2), ho = d2(s+i+4), mi = d2(s+i+6), se = d2(s+i+8);
    (void)n;
    return ((((year*100+mo)*100+da)*100+ho)*100+mi)*100+se;
}

// --- Analyse d'un certificat -------------------------------------------------
int x509_parse(const uint8_t *der, int len, x509_cert *c) {
    for (int i = 0; i < (int)sizeof(*c); i++) ((uint8_t*)c)[i] = 0;
    const uint8_t *p = der, *end = der + len, *cin, *cend;
    if (!der_enter(&p, end, 0x30, &cin, &cend)) return -1;     // Certificate

    // tbsCertificate (on garde ses octets bruts, signature comprise).
    const uint8_t *tbs_start = cin;
    const uint8_t *tin, *tend;
    { const uint8_t *q = cin; if (!der_enter(&q, cend, 0x30, &tin, &tend)) return -1;
      c->tbs = tbs_start; c->tbs_len = (int)(tend - tbs_start); }

    // signatureAlgorithm + signatureValue (après le TBS).
    { const uint8_t *q = tend; int alg;
      if (!read_algid(&q, cend, &alg)) return -1;
      c->sig_alg = alg;
      int tag, vlen; const uint8_t *val;
      if (!der_read(&q, cend, &tag, &val, &vlen) || tag != 0x03) return -1;   // BIT STRING
      c->sig = val + 1; c->sig_len = vlen - 1;     // saute l'octet "unused bits"
    }

    // Parcourt le TBS.
    const uint8_t *t = tin;
    int tag, vlen; const uint8_t *val;
    // version [0] EXPLICIT optionnelle
    if (t < tend && t[0] == 0xA0) { if (!der_read(&t, tend, &tag, &val, &vlen)) return -1; }
    if (!der_read(&t, tend, &tag, &val, &vlen)) return -1;     // serialNumber
    { const uint8_t *q = t; int a; if (!read_algid(&q, tend, &a)) return -1; t = q; }  // signature algid
    // issuer (Name = SEQUENCE)
    { const uint8_t *s = t; if (!der_read(&t, tend, &tag, &val, &vlen) || tag != 0x30) return -1;
      c->issuer = s; c->issuer_len = (int)(t - s); }
    // validity
    { const uint8_t *vin, *vend; if (!der_enter(&t, tend, 0x30, &vin, &vend)) return -1;
      if (!der_read(&vin, vend, &tag, &val, &vlen)) return -1;
      c->not_before = parse_time(tag, val, vlen);
      if (!der_read(&vin, vend, &tag, &val, &vlen)) return -1;
      c->not_after  = parse_time(tag, val, vlen); }
    // subject (Name)
    { const uint8_t *s = t; if (!der_read(&t, tend, &tag, &val, &vlen) || tag != 0x30) return -1;
      c->subject = s; c->subject_len = (int)(t - s); }
    // subjectPublicKeyInfo
    { const uint8_t *s = t; const uint8_t *kin, *kend;
      if (!der_enter(&t, tend, 0x30, &kin, &kend)) return -1;
      c->spki = s; c->spki_len = (int)(t - s);
      int alg;                                       // algorithme de la clé
      { const uint8_t *ain, *aend; if (!der_enter(&kin, kend, 0x30, &ain, &aend)) return -1;
        if (!der_read(&ain, aend, &tag, &val, &vlen) || tag != 0x06) return -1;
        alg = oid_eq(val, vlen, OID_RSA_ENC, sizeof OID_RSA_ENC); }
      if (!der_read(&kin, kend, &tag, &val, &vlen) || tag != 0x03) return -1;   // BIT STRING
      if (alg) {
        c->pub_is_rsa = 1;
        const uint8_t *rin = val + 1, *rend = val + vlen;      // RSAPublicKey
        const uint8_t *sin, *send;
        if (!der_enter(&rin, rend, 0x30, &sin, &send)) return -1;
        if (!der_read(&sin, send, &tag, &val, &vlen) || tag != 0x02) return -1; // modulus
        if (bn_from_be(&c->pub_n, val, vlen) != 0) return -1;
        if (!der_read(&sin, send, &tag, &val, &vlen) || tag != 0x02) return -1; // exponent
        c->pub_e = val; c->pub_e_len = vlen;
      }
    }
    // extensions [3] EXPLICIT (optionnel) : on cherche le SAN.
    while (t < tend) {
        const uint8_t *s = t;
        if (!der_read(&t, tend, &tag, &val, &vlen)) break;
        if (tag == 0xA3) {                            // [3] extensions
            const uint8_t *ein = val, *eend = val + vlen;
            const uint8_t *sin, *send;
            if (!der_enter(&ein, eend, 0x30, &sin, &send)) break;   // SEQUENCE OF Extension
            while (sin < send) {
                const uint8_t *xin, *xend;
                if (!der_enter(&sin, send, 0x30, &xin, &xend)) break;
                if (!der_read(&xin, xend, &tag, &val, &vlen) || tag != 0x06) continue;
                int is_san = oid_eq(val, vlen, OID_SAN, sizeof OID_SAN);
                // critical BOOLEAN optionnel
                if (xin < xend && xin[0] == 0x01) der_read(&xin, xend, &tag, &val, &vlen);
                if (!der_read(&xin, xend, &tag, &val, &vlen) || tag != 0x04) continue; // OCTET STRING
                if (is_san) { c->san = val; c->san_len = vlen; }
            }
        }
        (void)s;
    }
    return 0;
}

int x509_dn_equal(const uint8_t *a, int al, const uint8_t *b, int bl) {
    if (al != bl) return 0;
    for (int i = 0; i < al; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int x509_verify_signed_by(const x509_cert *child, const x509_cert *iss) {
    if (!iss->pub_is_rsa) return 0;                   // émetteur non-RSA : non géré
    uint8_t h[32]; sha256(child->tbs, child->tbs_len, h);
    if (child->sig_alg == SIGALG_RSA_PKCS1_SHA256)
        return rsa_verify_pkcs1_sha256(&iss->pub_n, iss->pub_e, iss->pub_e_len, child->sig, child->sig_len, h);
    if (child->sig_alg == SIGALG_RSA_PSS_SHA256)
        return rsa_verify_pss_sha256(&iss->pub_n, iss->pub_e, iss->pub_e_len, child->sig, child->sig_len, h);
    return 0;                                          // ECDSA / inconnu
}

// Compare un motif SAN (ex. "*.exemple.fr") à 'host' (insensible à la casse).
static int host_match(const uint8_t *pat, int pl, const char *host) {
    int hl = (int)strlen(host);
    // joker en tête : "*.suffixe"
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = host; while (*dot && *dot != '.') dot++;
        if (*dot != '.') return 0;
        int sl = pl - 1;                              // ".suffixe"
        int rl = hl - (int)(dot - host);
        if (rl != sl) return 0;
        for (int i = 0; i < sl; i++) {
            char a = pat[1 + i], b = dot[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
        return 1;
    }
    if (pl != hl) return 0;
    for (int i = 0; i < pl; i++) {
        char a = pat[i], b = host[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

// Reconnaît "a.b.c.d" -> 4 octets. 1 si IPv4, 0 sinon.
static int parse_ipv4(const char *h, uint8_t ip[4]) {
    int part = 0, v = 0, dig = 0;
    for (const char *p = h; ; p++) {
        if (*p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); dig = 1; if (v > 255) return 0; }
        else if (*p == '.' || *p == 0) {
            if (!dig || part > 3) return 0;
            ip[part++] = (uint8_t)v; v = 0; dig = 0;
            if (*p == 0) break;
        } else return 0;
    }
    return part == 4;
}

int x509_check_host(const x509_cert *c, const char *host) {
    if (c->san_len <= 0) return 0;
    uint8_t ip[4]; int is_ip = parse_ipv4(host, ip);
    const uint8_t *p = c->san, *end = c->san + c->san_len, *in, *iend;
    if (!der_enter(&p, end, 0x30, &in, &iend)) return 0;   // GeneralNames SEQUENCE
    while (in < iend) {
        int tag, vlen; const uint8_t *val;
        if (!der_read(&in, iend, &tag, &val, &vlen)) break;
        if (!is_ip && tag == 0x82) {                  // [2] dNSName
            if (host_match(val, vlen, host)) return 1;
        } else if (is_ip && tag == 0x87 && vlen == 4) {   // [7] iPAddress
            if (val[0]==ip[0] && val[1]==ip[1] && val[2]==ip[2] && val[3]==ip[3]) return 1;
        }
    }
    return 0;
}
