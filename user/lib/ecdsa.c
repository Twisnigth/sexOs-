// =============================================================================
//  user/lib/ecdsa.c -- Vérification ECDSA (P-256 / P-384), coordonnées jacobiennes
// -----------------------------------------------------------------------------
//  S'appuie sur bigint (modmul / modexp). Données publiques uniquement.
// =============================================================================
#include "ecdsa.h"
#include "bigint.h"

// --- Courbe active -----------------------------------------------------------
typedef struct {
    bn_t P, N, B, GX, GY, THREE;
    uint8_t pm2[64], nm2[64];     // p-2, n-2 (exposants d'inversion de Fermat)
    int pm2len, nm2len, bytes;
} ecurve;

static ecurve C256, C384;
static const ecurve *CV;
static int inited;

static int hexb(const char *h, uint8_t *o) {
    int n = 0;
    while (h[2*n] && h[2*n+1]) {
        int hi = h[2*n], lo = h[2*n+1];
        hi = (hi<='9')?hi-'0':(hi|32)-'a'+10; lo = (lo<='9')?lo-'0':(lo|32)-'a'+10;
        o[n++] = (uint8_t)((hi<<4)|lo);
    }
    return n;
}
static void setbn(bn_t *b, const char *hex) { uint8_t t[80]; int n = hexb(hex, t); bn_from_be(b, t, n); }

static void init_curve(ecurve *c, int bytes, const char *p, const char *n,
                       const char *b, const char *gx, const char *gy,
                       const char *pm2, const char *nm2) {
    c->bytes = bytes;
    setbn(&c->P, p); setbn(&c->N, n); setbn(&c->B, b); setbn(&c->GX, gx); setbn(&c->GY, gy);
    uint8_t t = 3; bn_from_be(&c->THREE, &t, 1);
    c->pm2len = hexb(pm2, c->pm2); c->nm2len = hexb(nm2, c->nm2);
}

static void init(void) {
    if (inited) return;
    init_curve(&C256, 32,
      "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
      "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
      "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
      "ffffffff00000001000000000000000000000000fffffffffffffffffffffffd",
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63254f");
    init_curve(&C384, 48,
      "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
      "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
      "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
      "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
      "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
      "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffd",
      "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52971");
    inited = 1;
}

static int is_zero(const bn_t *a) { for (int i = 0; i < BN_LIMBS; i++) if (a->v[i]) return 0; return 1; }

// --- Arithmétique de corps mod P (courbe active) -----------------------------
static int raw_add(bn_t *r, const bn_t *a, const bn_t *b) {
    uint64_t c = 0;
    for (int i = 0; i < BN_LIMBS; i++) { uint64_t t = (uint64_t)a->v[i] + b->v[i] + c; r->v[i] = (uint32_t)t; c = t >> 32; }
    return (int)c;
}
static void raw_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    uint64_t bw = 0;
    for (int i = 0; i < BN_LIMBS; i++) { uint64_t t = (uint64_t)a->v[i] - b->v[i] - bw; r->v[i] = (uint32_t)t; bw = (t >> 63) & 1; }
}
static void f_add(bn_t *r, const bn_t *a, const bn_t *b) { raw_add(r, a, b); if (bn_cmp(r, &CV->P) >= 0) raw_sub(r, r, &CV->P); }
static void f_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    if (bn_cmp(a, b) >= 0) raw_sub(r, a, b);
    else { bn_t t; raw_add(&t, a, &CV->P); raw_sub(r, &t, b); }
}
static void f_mul(bn_t *r, const bn_t *a, const bn_t *b) { bn_modmul(r, a, b, &CV->P); }
static void f_inv(bn_t *r, const bn_t *a) { bn_modexp(r, a, CV->pm2, CV->pm2len, &CV->P); }

typedef struct { bn_t X, Y, Z; } jpt;
static void j_inf(jpt *p) { bn_zero(&p->X); bn_zero(&p->Y); bn_zero(&p->Z); p->X.v[0]=1; p->Y.v[0]=1; }

static void j_double(jpt *o, const jpt *p) {
    if (is_zero(&p->Z)) { *o = *p; return; }
    bn_t delta, gamma, beta, alpha, t1, t2, X3, Y3, Z3;
    f_mul(&delta, &p->Z, &p->Z); f_mul(&gamma, &p->Y, &p->Y); f_mul(&beta, &p->X, &gamma);
    f_sub(&t1, &p->X, &delta); f_add(&t2, &p->X, &delta); f_mul(&t1, &t1, &t2); f_mul(&alpha, &t1, &CV->THREE);
    f_mul(&X3, &alpha, &alpha);
    f_add(&t1, &beta, &beta); f_add(&t1, &t1, &t1); f_add(&t1, &t1, &t1); f_sub(&X3, &X3, &t1);
    f_add(&t1, &p->Y, &p->Z); f_mul(&t1, &t1, &t1); f_sub(&t1, &t1, &gamma); f_sub(&Z3, &t1, &delta);
    f_add(&t1, &beta, &beta); f_add(&t1, &t1, &t1); f_sub(&t1, &t1, &X3); f_mul(&Y3, &alpha, &t1);
    f_mul(&t2, &gamma, &gamma); f_add(&t2, &t2, &t2); f_add(&t2, &t2, &t2); f_add(&t2, &t2, &t2); f_sub(&Y3, &Y3, &t2);
    o->X = X3; o->Y = Y3; o->Z = Z3;
}

static void j_add(jpt *o, const jpt *p, const jpt *q) {
    if (is_zero(&p->Z)) { *o = *q; return; }
    if (is_zero(&q->Z)) { *o = *p; return; }
    bn_t Z1Z1, Z2Z2, U1, U2, S1, S2, H, I, J, r, t, X3, Y3, Z3;
    f_mul(&Z1Z1, &p->Z, &p->Z); f_mul(&Z2Z2, &q->Z, &q->Z);
    f_mul(&U1, &p->X, &Z2Z2); f_mul(&U2, &q->X, &Z1Z1);
    f_mul(&S1, &p->Y, &q->Z); f_mul(&S1, &S1, &Z2Z2);
    f_mul(&S2, &q->Y, &p->Z); f_mul(&S2, &S2, &Z1Z1);
    if (bn_cmp(&U1, &U2) == 0) { if (bn_cmp(&S1, &S2) != 0) { j_inf(o); bn_zero(&o->Z); return; } j_double(o, p); return; }
    f_sub(&H, &U2, &U1);
    f_add(&I, &H, &H); f_mul(&I, &I, &I); f_mul(&J, &H, &I);
    f_sub(&r, &S2, &S1); f_add(&r, &r, &r);
    f_mul(&X3, &r, &r); f_mul(&t, &U1, &I); f_add(&t, &t, &t); f_sub(&X3, &X3, &J); f_sub(&X3, &X3, &t);
    f_mul(&t, &U1, &I); f_sub(&t, &t, &X3); f_mul(&Y3, &r, &t);
    f_mul(&t, &S1, &J); f_add(&t, &t, &t); f_sub(&Y3, &Y3, &t);
    f_add(&Z3, &p->Z, &q->Z); f_mul(&Z3, &Z3, &Z3); f_sub(&Z3, &Z3, &Z1Z1); f_sub(&Z3, &Z3, &Z2Z2); f_mul(&Z3, &Z3, &H);
    o->X = X3; o->Y = Y3; o->Z = Z3;
}

static void j_mul(jpt *o, const uint8_t *k, int klen, const jpt *p) {
    jpt r; j_inf(&r); bn_zero(&r.Z);
    for (int i = 0; i < klen; i++)
        for (int bit = 7; bit >= 0; bit--) { j_double(&r, &r); if ((k[i] >> bit) & 1) j_add(&r, &r, p); }
    *o = r;
}

static void mod_n(bn_t *r, const bn_t *a) { if (bn_cmp(a, &CV->N) >= 0) raw_sub(r, a, &CV->N); else *r = *a; }

static int der_int(const uint8_t **p, const uint8_t *end, bn_t *out) {
    if (*p >= end || *(*p)++ != 0x02) return 0;
    if (*p >= end) return 0;
    int len = *(*p)++;
    if (len & 0x80) { int nb = len & 0x7f; if (nb != 1 || *p >= end) return 0; len = *(*p)++; }
    if (*p + len > end || len <= 0) return 0;
    if (bn_from_be(out, *p, len) != 0) return 0;
    *p += len; return 1;
}

static int verify(const ecurve *cv, const uint8_t *Qx, const uint8_t *Qy,
                  const uint8_t *sig_der, int sig_len, const uint8_t *hash, int hashlen) {
    init(); CV = cv;
    const uint8_t *p = sig_der, *end = sig_der + sig_len;
    if (p >= end || *p++ != 0x30) return 0;
    int slen = *p++;
    if (slen & 0x80) { int nb = slen & 0x7f; if (nb != 1 || p >= end) return 0; slen = *p++; }
    if (p + slen > end) return 0;
    end = p + slen;
    bn_t r, s;
    if (!der_int(&p, end, &r) || !der_int(&p, end, &s)) return 0;
    if (is_zero(&r) || is_zero(&s) || bn_cmp(&r, &cv->N) >= 0 || bn_cmp(&s, &cv->N) >= 0) return 0;

    bn_t z; bn_from_be(&z, hash, hashlen); { bn_t t; mod_n(&t, &z); z = t; }
    bn_t w; bn_modexp(&w, &s, cv->nm2, cv->nm2len, &cv->N);
    bn_t u1, u2; bn_modmul(&u1, &z, &w, &cv->N); bn_modmul(&u2, &r, &w, &cv->N);

    jpt G, Q, R1, R2, R;
    G.X = cv->GX; G.Y = cv->GY; bn_zero(&G.Z); G.Z.v[0] = 1;
    bn_from_be(&Q.X, Qx, cv->bytes); bn_from_be(&Q.Y, Qy, cv->bytes); bn_zero(&Q.Z); Q.Z.v[0] = 1;
    uint8_t u1b[64], u2b[64]; bn_to_be(&u1, u1b, cv->bytes); bn_to_be(&u2, u2b, cv->bytes);
    j_mul(&R1, u1b, cv->bytes, &G); j_mul(&R2, u2b, cv->bytes, &Q); j_add(&R, &R1, &R2);
    if (is_zero(&R.Z)) return 0;
    bn_t zi, zi2, x; f_inv(&zi, &R.Z); f_mul(&zi2, &zi, &zi); f_mul(&x, &R.X, &zi2);
    bn_t v; mod_n(&v, &x);
    return bn_cmp(&v, &r) == 0;
}

int ecdsa_p256_verify(const uint8_t Qx[32], const uint8_t Qy[32],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[32]) {
    init(); return verify(&C256, Qx, Qy, sig_der, sig_len, hash, 32);
}
int ecdsa_p384_verify(const uint8_t Qx[48], const uint8_t Qy[48],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[48]) {
    init(); return verify(&C384, Qx, Qy, sig_der, sig_len, hash, 48);
}
