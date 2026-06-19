// =============================================================================
//  user/lib/ecdsa.c -- Vérification ECDSA P-256 (coordonnées jacobiennes)
// -----------------------------------------------------------------------------
//  S'appuie sur bigint (modmul / modexp). Ne traite que des données publiques
//  (clé, signature) : pas de contrainte de temps constant.
// =============================================================================
#include "ecdsa.h"
#include "bigint.h"

// --- Paramètres de la courbe (gros-boutiste) ---------------------------------
static const uint8_t P_BE[32] = {
 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t N_BE[32] = {
 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
 0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const uint8_t B_BE[32] = {
 0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
 0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
static const uint8_t GX_BE[32] = {
 0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
 0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t GY_BE[32] = {
 0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
 0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
// p-2 et n-2 (exposants d'inversion par petit théorème de Fermat).
static const uint8_t PM2_BE[32] = {
 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
 0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfd};
static const uint8_t NM2_BE[32] = {
 0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
 0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x4f};

static bn_t P, N, B, GX, GY, THREE;
static int inited;
static void init(void) {
    if (inited) return;
    bn_from_be(&P, P_BE, 32); bn_from_be(&N, N_BE, 32); bn_from_be(&B, B_BE, 32);
    bn_from_be(&GX, GX_BE, 32); bn_from_be(&GY, GY_BE, 32);
    uint8_t t = 3; bn_from_be(&THREE, &t, 1);
    inited = 1;
}

static int is_zero(const bn_t *a) { for (int i = 0; i < BN_LIMBS; i++) if (a->v[i]) return 0; return 1; }

// --- Arithmétique de corps mod P --------------------------------------------
static int raw_add(bn_t *r, const bn_t *a, const bn_t *b) {     // renvoie la retenue
    uint64_t c = 0;
    for (int i = 0; i < BN_LIMBS; i++) { uint64_t t = (uint64_t)a->v[i] + b->v[i] + c; r->v[i] = (uint32_t)t; c = t >> 32; }
    return (int)c;
}
static void raw_sub(bn_t *r, const bn_t *a, const bn_t *b) {    // suppose a >= b
    uint64_t bw = 0;
    for (int i = 0; i < BN_LIMBS; i++) { uint64_t t = (uint64_t)a->v[i] - b->v[i] - bw; r->v[i] = (uint32_t)t; bw = (t >> 63) & 1; }
}
static void f_add(bn_t *r, const bn_t *a, const bn_t *b) { raw_add(r, a, b); if (bn_cmp(r, &P) >= 0) raw_sub(r, r, &P); }
static void f_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    if (bn_cmp(a, b) >= 0) raw_sub(r, a, b);
    else { bn_t t; raw_add(&t, a, &P); raw_sub(r, &t, b); }
}
static void f_mul(bn_t *r, const bn_t *a, const bn_t *b) { bn_modmul(r, a, b, &P); }
static void f_inv(bn_t *r, const bn_t *a) { bn_modexp(r, a, PM2_BE, 32, &P); }

// --- Points en coordonnées jacobiennes (x=X/Z^2, y=Y/Z^3) --------------------
typedef struct { bn_t X, Y, Z; } jpt;

static void j_zero(jpt *p) { bn_zero(&p->X); bn_zero(&p->Y); bn_zero(&p->Z); p->X.v[0] = 1; p->Y.v[0] = 1; }  // infini (Z=0)

static void j_double(jpt *o, const jpt *p) {
    if (is_zero(&p->Z)) { *o = *p; return; }
    bn_t delta, gamma, beta, alpha, t1, t2, X3, Y3, Z3;
    f_mul(&delta, &p->Z, &p->Z);                 // Z^2
    f_mul(&gamma, &p->Y, &p->Y);                 // Y^2
    f_mul(&beta, &p->X, &gamma);                 // X*gamma
    f_sub(&t1, &p->X, &delta); f_add(&t2, &p->X, &delta);
    f_mul(&t1, &t1, &t2); f_mul(&alpha, &t1, &THREE);   // 3(X-d)(X+d)
    f_mul(&X3, &alpha, &alpha);                  // alpha^2
    f_add(&t1, &beta, &beta); f_add(&t1, &t1, &t1); f_add(&t1, &t1, &t1);  // 8*beta
    f_sub(&X3, &X3, &t1);
    f_add(&t1, &p->Y, &p->Z); f_mul(&t1, &t1, &t1); f_sub(&t1, &t1, &gamma); f_sub(&Z3, &t1, &delta);  // (Y+Z)^2-g-d
    f_add(&t1, &beta, &beta); f_add(&t1, &t1, &t1); f_sub(&t1, &t1, &X3);   // 4beta - X3
    f_mul(&Y3, &alpha, &t1);
    f_mul(&t2, &gamma, &gamma); f_add(&t2, &t2, &t2); f_add(&t2, &t2, &t2); f_add(&t2, &t2, &t2);  // 8 gamma^2
    f_sub(&Y3, &Y3, &t2);
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
    if (bn_cmp(&U1, &U2) == 0) {
        if (bn_cmp(&S1, &S2) != 0) { j_zero(o); bn_zero(&o->Z); return; }  // P + (-P) = infini
        j_double(o, p); return;
    }
    f_sub(&H, &U2, &U1);
    f_add(&I, &H, &H); f_mul(&I, &I, &I);        // (2H)^2
    f_mul(&J, &H, &I);
    f_sub(&r, &S2, &S1); f_add(&r, &r, &r);      // 2(S2-S1)
    f_mul(&X3, &r, &r); f_mul(&t, &U1, &I); f_add(&t, &t, &t); // 2*U1*I
    f_sub(&X3, &X3, &J); f_sub(&X3, &X3, &t);
    f_mul(&t, &U1, &I); f_sub(&t, &t, &X3); f_mul(&Y3, &r, &t);
    f_mul(&t, &S1, &J); f_add(&t, &t, &t); f_sub(&Y3, &Y3, &t);
    f_add(&Z3, &p->Z, &q->Z); f_mul(&Z3, &Z3, &Z3); f_sub(&Z3, &Z3, &Z1Z1); f_sub(&Z3, &Z3, &Z2Z2); f_mul(&Z3, &Z3, &H);
    o->X = X3; o->Y = Y3; o->Z = Z3;
}

// R = k * P  (k en octets gros-boutistes, 32 octets)
static void j_mul(jpt *o, const uint8_t *k, int klen, const jpt *p) {
    jpt r; j_zero(&r); bn_zero(&r.Z);            // infini
    for (int i = 0; i < klen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            j_double(&r, &r);
            if ((k[i] >> bit) & 1) j_add(&r, &r, p);
        }
    }
    *o = r;
}

// Réduit a (< 2N) mod N : un seul retrait conditionnel.
static void mod_n(bn_t *r, const bn_t *a) { if (bn_cmp(a, &N) >= 0) raw_sub(r, a, &N); else *r = *a; }

// DER : lit un INTEGER (saute l'octet 0x00 de tête éventuel). 0 si erreur.
static int der_int(const uint8_t **p, const uint8_t *end, bn_t *out) {
    if (*p >= end || *(*p)++ != 0x02) return 0;
    if (*p >= end) return 0;
    int len = *(*p)++;
    if (len & 0x80) { int nb = len & 0x7f; if (nb != 1 || *p >= end) return 0; len = *(*p)++; }
    if (*p + len > end || len <= 0) return 0;
    if (bn_from_be(out, *p, len) != 0) return 0;
    *p += len;
    return 1;
}

int ecdsa_p256_verify(const uint8_t Qx[32], const uint8_t Qy[32],
                      const uint8_t *sig_der, int sig_len, const uint8_t hash[32]) {
    init();
    // Parse DER SEQUENCE { r, s }.
    const uint8_t *p = sig_der, *end = sig_der + sig_len;
    if (p >= end || *p++ != 0x30) return 0;
    int slen = *p++;
    if (slen & 0x80) { int nb = slen & 0x7f; if (nb != 1 || p >= end) return 0; slen = *p++; }
    if (p + slen > end) return 0;
    end = p + slen;
    bn_t r, s;
    if (!der_int(&p, end, &r) || !der_int(&p, end, &s)) return 0;

    // r, s dans [1, n-1] ?
    if (is_zero(&r) || is_zero(&s) || bn_cmp(&r, &N) >= 0 || bn_cmp(&s, &N) >= 0) return 0;

    uint8_t zb[32]; for (int i = 0; i < 32; i++) zb[i] = hash[i];
    bn_t z; bn_from_be(&z, zb, 32); { bn_t t; mod_n(&t, &z); z = t; }

    bn_t w; bn_modexp(&w, &s, NM2_BE, 32, &N);   // s^-1 mod n
    bn_t u1, u2;
    bn_modmul(&u1, &z, &w, &N);
    bn_modmul(&u2, &r, &w, &N);

    jpt G, Q, R1, R2, R;
    G.X = GX; G.Y = GY; bn_zero(&G.Z); G.Z.v[0] = 1;
    bn_from_be(&Q.X, Qx, 32); bn_from_be(&Q.Y, Qy, 32); bn_zero(&Q.Z); Q.Z.v[0] = 1;

    uint8_t u1b[32], u2b[32]; bn_to_be(&u1, u1b, 32); bn_to_be(&u2, u2b, 32);
    j_mul(&R1, u1b, 32, &G);
    j_mul(&R2, u2b, 32, &Q);
    j_add(&R, &R1, &R2);
    if (is_zero(&R.Z)) return 0;

    // x affine = X / Z^2  mod p
    bn_t zi, zi2, x;
    f_inv(&zi, &R.Z); f_mul(&zi2, &zi, &zi); f_mul(&x, &R.X, &zi2);

    bn_t v; mod_n(&v, &x);                        // v = x mod n
    return bn_cmp(&v, &r) == 0;
}
