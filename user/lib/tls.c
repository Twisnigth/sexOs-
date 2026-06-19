// =============================================================================
//  user/lib/tls.c -- Client TLS 1.3 (ring 3) : X25519 + ChaCha20-Poly1305-SHA256
// -----------------------------------------------------------------------------
//  Implemente le strict necessaire d'un client HTTPS : handshake 1-RTT, schedule
//  de cles (HKDF), couche d'enregistrement chiffree (AEAD). S'appuie sur les
//  sockets TCP non bloquantes du noyau (sys_tcp_*) et sur Monocypher + SHA-256.
//
//  NON FAIT (assume) : verification du certificat serveur (RSA/ECDSA + X.509 +
//  magasin d'AC). La session est chiffree mais NON authentifiee.
// =============================================================================
#include "monos.h"
#include "tls.h"
#include "crypto.h"        // sha256_ctx / sha256_*  (+ monocypher.h)
#include "x509.h"
#include "rsa.h"
#include "castore.h"

void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
unsigned long strlen(const char *);

// --- Types d'enregistrement / de handshake -----------------------------------
#define CT_CCS 20
#define CT_ALERT 21
#define CT_HANDSHAKE 22
#define CT_APPDATA 23
#define HS_CLIENT_HELLO 1
#define HS_SERVER_HELLO 2
#define HS_ENCRYPTED_EXT 8
#define HS_CERT 11
#define HS_CERT_VERIFY 15
#define HS_FINISHED 20

static int ms(void) { return (int)sys_time_ms(); }

// --- AEAD ChaCha20-Poly1305 (RFC 8439) ---------------------------------------
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i)); }

static void poly_tag(const uint8_t otk[32], const uint8_t *aad, int al,
                     const uint8_t *ct, int cl, uint8_t tag[16]) {
    static const uint8_t zero[16] = {0};
    crypto_poly1305_ctx pc; crypto_poly1305_init(&pc, otk);
    crypto_poly1305_update(&pc, aad, al);
    if (al % 16) crypto_poly1305_update(&pc, zero, 16 - (al % 16));
    crypto_poly1305_update(&pc, ct, cl);
    if (cl % 16) crypto_poly1305_update(&pc, zero, 16 - (cl % 16));
    uint8_t len[16]; le64(len, (uint64_t)al); le64(len + 8, (uint64_t)cl);
    crypto_poly1305_update(&pc, len, 16);
    crypto_poly1305_final(&pc, tag);
}

static void aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, int al, const uint8_t *pt, int pl,
                      uint8_t *ct, uint8_t tag[16]) {
    uint8_t otk[32], z[32]; memset(z, 0, 32);
    crypto_chacha20_ietf(otk, z, 32, key, nonce, 0);        // clé Poly1305 (bloc 0)
    crypto_chacha20_ietf(ct, pt, pl, key, nonce, 1);        // chiffrement (bloc 1+)
    poly_tag(otk, aad, al, ct, pl, tag);
    crypto_wipe(otk, 32);
}

static int aead_open(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, int al, const uint8_t *ct, int cl,
                     const uint8_t tag[16], uint8_t *pt) {
    uint8_t otk[32], z[32]; memset(z, 0, 32);
    crypto_chacha20_ietf(otk, z, 32, key, nonce, 0);
    uint8_t exp[16]; poly_tag(otk, aad, al, ct, cl, exp);
    crypto_wipe(otk, 32);
    if (crypto_verify16(exp, tag) != 0) return -1;          // authentification
    uint8_t k[32]; crypto_chacha20_ietf(otk, z, 32, key, nonce, 0); (void)k;
    crypto_chacha20_ietf(pt, ct, cl, key, nonce, 1);        // déchiffrement
    return 0;
}

// --- HMAC-SHA256 / HKDF (TLS 1.3) --------------------------------------------
static void hmac_sha256(const uint8_t *key, int kl, const uint8_t *msg, int ml, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, 64);
    if (kl > 64) { sha256(key, kl, k); } else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha256_ctx c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, ml); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

// HKDF-Expand-Label (sortie <= 32 octets -> un seul bloc HMAC).
static void expand_label(const uint8_t secret[32], const char *label,
                         const uint8_t *ctx, int cl, uint8_t *out, int ol) {
    uint8_t info[256]; int o = 0;
    info[o++] = (uint8_t)(ol >> 8); info[o++] = (uint8_t)ol;
    int ll = 6 + (int)strlen(label);
    info[o++] = (uint8_t)ll;
    memcpy(info + o, "tls13 ", 6); o += 6;
    memcpy(info + o, label, strlen(label)); o += (int)strlen(label);
    info[o++] = (uint8_t)cl; if (cl) { memcpy(info + o, ctx, cl); o += cl; }
    info[o] = 0x01;                                          // compteur HKDF
    uint8_t t1[32]; hmac_sha256(secret, 32, info, o + 1, t1);
    memcpy(out, t1, ol);
}

static void derive_secret(const uint8_t secret[32], const char *label,
                          const uint8_t th[32], uint8_t out[32]) {
    expand_label(secret, label, th, 32, out, 32);
}

static void traffic_keys(const uint8_t secret[32], uint8_t key[32], uint8_t iv[12]) {
    expand_label(secret, "key", 0, 0, key, 32);
    expand_label(secret, "iv", 0, 0, iv, 12);
}

static void mk_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (8*i));
}

static void th_snapshot(const sha256_ctx *th, uint8_t out[32]) {
    sha256_ctx c = *th; sha256_final(&c, out);
}

// --- E/S socket : lecture exacte de n octets ---------------------------------
//  Renvoie 0=ok, -1=fermeture, -2=timeout.
static int read_n(int conn, uint8_t *buf, int n, int deadline) {
    int got = 0;
    while (got < n) {
        int r = sys_tcp_recv(conn, buf + got, n - got);
        if (r > 0) got += r;
        else if (r < 0) return -1;
        else { if (ms() > deadline) return -2; sys_yield(); }
    }
    return 0;
}

// Lit un enregistrement complet : hdr[5] + body[*blen]. Codes comme read_n.
static int read_record(int conn, uint8_t *hdr, uint8_t *body, int *blen, int deadline) {
    int r = read_n(conn, hdr, 5, deadline); if (r) return r;
    int len = (hdr[3] << 8) | hdr[4];
    if (len < 0 || len > 18000) return -3;
    r = read_n(conn, body, len, deadline); if (r) return r;
    *blen = len;
    return 0;
}

static void send_all(int conn, const uint8_t *buf, int len) {
    int off = 0, dl = ms() + 8000;
    while (off < len) {
        int n = sys_tcp_send(conn, buf + off, len - off);
        if (n > 0) off += n;
        else if (n < 0) return;
        else { if (ms() > dl) return; sys_yield(); }
    }
}

static void send_plain(int conn, int type, const uint8_t *data, int len) {
    uint8_t hdr[5] = { (uint8_t)type, 0x03, 0x03, (uint8_t)(len >> 8), (uint8_t)len };
    send_all(conn, hdr, 5);
    send_all(conn, data, len);
}

static uint8_t encbuf[18000];
static void send_enc(int conn, int inner_type, const uint8_t *data, int len,
                     const uint8_t key[32], const uint8_t iv[12], uint64_t *seq) {
    int pl = len + 1;                                       // données + type interne
    int total = pl + 16;                                   // + tag
    uint8_t hdr[5] = { CT_APPDATA, 0x03, 0x03, (uint8_t)(total >> 8), (uint8_t)total };
    uint8_t pt[17000];
    memcpy(pt, data, len); pt[len] = (uint8_t)inner_type;
    uint8_t nonce[12]; mk_nonce(nonce, iv, *seq);
    aead_seal(key, nonce, hdr, 5, pt, pl, encbuf, encbuf + pl);
    (*seq)++;
    uint8_t out[5]; memcpy(out, hdr, 5);
    send_all(conn, out, 5);
    send_all(conn, encbuf, total);
}

// --- ClientHello -------------------------------------------------------------
static int is_name(const char *h) {
    for (const char *p = h; *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '.')) return 1;
    return 0;
}

static int build_client_hello(uint8_t *o, const uint8_t random[32], const uint8_t sid[32],
                              const uint8_t cpub[32], const char *host) {
    int p = 0;
    o[p++] = HS_CLIENT_HELLO; int lenpos = p; p += 3;       // longueur (backpatch)
    o[p++] = 0x03; o[p++] = 0x03;                           // legacy_version TLS1.2
    memcpy(o + p, random, 32); p += 32;
    o[p++] = 32; memcpy(o + p, sid, 32); p += 32;           // legacy_session_id
    o[p++] = 0x00; o[p++] = 0x02; o[p++] = 0x13; o[p++] = 0x03;  // cipher: CHACHA20_POLY1305
    o[p++] = 0x01; o[p++] = 0x00;                           // compression: null

    int extpos = p; p += 2;                                 // longueur des extensions
    // supported_versions (0x002b) -> TLS 1.3
    o[p++]=0x00; o[p++]=0x2b; o[p++]=0x00; o[p++]=0x03; o[p++]=0x02; o[p++]=0x03; o[p++]=0x04;
    // supported_groups (0x000a) -> x25519 (0x001d)
    o[p++]=0x00; o[p++]=0x0a; o[p++]=0x00; o[p++]=0x04; o[p++]=0x00; o[p++]=0x02; o[p++]=0x00; o[p++]=0x1d;
    // signature_algorithms (0x000d) : on offre les schemas courants (non verifies)
    { static const uint8_t sa[] = {0x08,0x04, 0x04,0x03, 0x08,0x07, 0x04,0x01, 0x08,0x05, 0x06,0x01};
      o[p++]=0x00; o[p++]=0x0d; o[p++]=0x00; o[p++]=(uint8_t)(sizeof sa + 2);
      o[p++]=0x00; o[p++]=(uint8_t)sizeof sa; memcpy(o+p, sa, sizeof sa); p += sizeof sa; }
    // key_share (0x0033) : x25519
    o[p++]=0x00; o[p++]=0x33; o[p++]=0x00; o[p++]=0x26; o[p++]=0x00; o[p++]=0x24;
    o[p++]=0x00; o[p++]=0x1d; o[p++]=0x00; o[p++]=0x20; memcpy(o+p, cpub, 32); p += 32;
    // server_name (0x0000) si l'hote est un nom (pas une IP)
    if (is_name(host)) {
        int hl = (int)strlen(host);
        o[p++]=0x00; o[p++]=0x00;
        o[p++]=(uint8_t)((hl+5)>>8); o[p++]=(uint8_t)(hl+5);   // ext len
        o[p++]=(uint8_t)((hl+3)>>8); o[p++]=(uint8_t)(hl+3);   // list len
        o[p++]=0x00;                                           // type host_name
        o[p++]=(uint8_t)(hl>>8); o[p++]=(uint8_t)hl;
        memcpy(o+p, host, hl); p += hl;
    }
    int extlen = p - extpos - 2;
    o[extpos] = (uint8_t)(extlen >> 8); o[extpos+1] = (uint8_t)extlen;
    int blen = p - lenpos - 3;
    o[lenpos] = (uint8_t)(blen >> 16); o[lenpos+1] = (uint8_t)(blen >> 8); o[lenpos+2] = (uint8_t)blen;
    return p;
}

// --- Vérification du certificat ---------------------------------------------
static uint8_t   certder[16384];     // copie stable de la chaîne reçue
static x509_cert g_certs[6];
static int       g_ncerts;

typedef struct { uint8_t second, minute, hour, day, month; uint16_t year; } tls_rtc_t;
static uint64_t now_stamp(void) {
    tls_rtc_t t; sys_rtc(&t);
    return ((((((uint64_t)t.year*100+t.month)*100+t.day)*100+t.hour)*100+t.minute)*100+t.second);
}
static void setinfo(char *d, const char *s) { int i = 0; for (; s[i] && i < 71; i++) d[i] = s[i]; d[i] = 0; }

// Analyse le message Certificate (TLS 1.3) et remplit g_certs.
static void parse_certificate(const uint8_t *body, int n) {
    g_ncerts = 0;
    if (n > (int)sizeof(certder)) return;
    memcpy(certder, body, n);
    int o = 0;
    if (o >= n) return;
    int ctxlen = certder[o]; o += 1 + ctxlen;            // certificate_request_context
    if (o + 3 > n) return;
    int listlen = (certder[o]<<16)|(certder[o+1]<<8)|certder[o+2]; o += 3;
    int listend = o + listlen; if (listend > n) listend = n;
    while (o + 3 <= listend && g_ncerts < 6) {
        int clen = (certder[o]<<16)|(certder[o+1]<<8)|certder[o+2]; o += 3;
        if (o + clen > listend) break;
        if (x509_parse(certder + o, clen, &g_certs[g_ncerts]) == 0) g_ncerts++;
        o += clen;
        if (o + 2 > listend) break;
        int extlen = (certder[o]<<8)|certder[o+1]; o += 2 + extlen;
    }
}

// Vérifie la signature CertificateVerify avec la clé de la feuille (RSA).
static int cert_verify_sig(int scheme, const uint8_t *sig, int siglen, const uint8_t th_cert[32]) {
    if (g_ncerts < 1 || !g_certs[0].pub_is_rsa) return 0;
    uint8_t content[64 + 33 + 1 + 32]; int o = 0;
    for (int i = 0; i < 64; i++) content[o++] = 0x20;
    const char *ctx = "TLS 1.3, server CertificateVerify";
    for (const char *p = ctx; *p; p++) content[o++] = *p;
    content[o++] = 0x00;
    memcpy(content + o, th_cert, 32); o += 32;
    uint8_t h[32]; sha256(content, o, h);
    bn_t *n = &g_certs[0].pub_n; const uint8_t *e = g_certs[0].pub_e; int el = g_certs[0].pub_e_len;
    if (scheme == 0x0804) return rsa_verify_pss_sha256(n, e, el, sig, siglen, h);   // rsa_pss_rsae_sha256
    if (scheme == 0x0401) return rsa_verify_pkcs1_sha256(n, e, el, sig, siglen, h); // rsa_pkcs1_sha256
    return 0;                                            // ECDSA / SHA-384 : non géré
}

// Vérifie la chaîne : hôte, dates, signatures, racine de confiance.
static int verify_chain(const char *host, char *info) {
    if (g_ncerts < 1) { setinfo(info, "aucun certificat"); return 0; }
    uint64_t now = now_stamp();
    x509_cert *leaf = &g_certs[0];
    if (now < leaf->not_before || now > leaf->not_after) { setinfo(info, "certificat hors periode de validite"); return 0; }
    if (!x509_check_host(leaf, host)) { setinfo(info, "nom d'hote non couvert par le certificat"); return 0; }
    for (int i = 0; i < g_ncerts - 1; i++) {
        if (!x509_dn_equal(g_certs[i].issuer, g_certs[i].issuer_len, g_certs[i+1].subject, g_certs[i+1].subject_len)) { setinfo(info, "chaine rompue (emetteur != sujet)"); return 0; }
        if (!x509_verify_signed_by(&g_certs[i], &g_certs[i+1])) { setinfo(info, "signature de chaine invalide"); return 0; }
    }
    x509_cert *top = &g_certs[g_ncerts - 1];
    for (int j = 0; j < castore_count(); j++) {
        int rl; const uint8_t *rd = castore_der(j, &rl);
        x509_cert root;
        if (x509_parse(rd, rl, &root) != 0) continue;
        if (x509_dn_equal(top->issuer, top->issuer_len, root.subject, root.subject_len) &&
            x509_verify_signed_by(top, &root)) { setinfo(info, "certificat verifie (chaine de confiance OK)"); return 1; }
    }
    setinfo(info, "autorite racine inconnue (non verifie)");
    return 0;
}

// --- Handshake ---------------------------------------------------------------
static uint8_t recbody[18000];
static uint8_t hsacc[20000];

int tls_handshake(tls_t *t, int conn, const char *host, const char **err) {
    t->conn = conn; t->established = 0; t->eof = 0; t->rlen = t->roff = 0;
    t->verified = 0; t->verify_info[0] = 0;
    g_ncerts = 0;
    int dl = ms() + 12000;
    if (err) *err = 0;
    uint8_t th_cert[32]; int have_th_cert = 0, cv_ok = 0;

    // 1. clés X25519 + aléas
    uint8_t rnd[96]; sys_random(rnd, 96);
    uint8_t priv[32], cpub[32];
    memcpy(priv, rnd, 32);
    crypto_x25519_public_key(cpub, priv);

    // 2. ClientHello
    uint8_t ch[1024];
    int chlen = build_client_hello(ch, rnd + 32, rnd + 64, cpub, host);
    sha256_ctx th; sha256_init(&th); sha256_update(&th, ch, chlen);
    send_plain(conn, CT_HANDSHAKE, ch, chlen);

    // 3. ServerHello (enregistrement clair type 22 ; on ignore un CCS)
    uint8_t hdr[5]; int blen;
    for (;;) {
        int r = read_record(conn, hdr, recbody, &blen, dl);
        if (r) { if (err) *err = "pas de ServerHello (timeout/fermeture)"; return -1; }
        if (hdr[0] == CT_CCS) continue;
        if (hdr[0] == CT_ALERT) { if (err) *err = "alerte TLS pendant ServerHello"; return -1; }
        if (hdr[0] == CT_HANDSHAKE) break;
        if (err) *err = "type d'enregistrement inattendu";
        return -1;
    }
    if (blen < 38 || recbody[0] != HS_SERVER_HELLO) { if (err) *err = "ServerHello invalide"; return -1; }
    int shlen = (recbody[1] << 16) | (recbody[2] << 8) | recbody[3];
    sha256_update(&th, recbody, 4 + shlen);                 // transcript += SH

    // Parcourt SH pour extraire la clé publique serveur (key_share x25519).
    uint8_t spub[32]; int have_spub = 0;
    {
        int p = 4 + 2 + 32;                                // hs hdr + version + random
        int sidl = recbody[p++]; p += sidl;                // session_id echo
        p += 2;                                            // cipher_suite
        p += 1;                                            // compression
        int extlen = (recbody[p] << 8) | recbody[p+1]; p += 2;
        int end = p + extlen;
        while (p + 4 <= end) {
            int et = (recbody[p] << 8) | recbody[p+1];
            int el = (recbody[p+2] << 8) | recbody[p+3]; p += 4;
            if (et == 0x0033 && el >= 36) {                // key_share
                // group(2) + len(2) + key
                int kl = (recbody[p+2] << 8) | recbody[p+3];
                if (kl == 32) { memcpy(spub, recbody + p + 4, 32); have_spub = 1; }
            }
            p += el;
        }
    }
    if (!have_spub) { if (err) *err = "serveur sans key_share x25519 (TLS1.3/ChaCha requis)"; return -1; }

    // 4. schedule de clés (handshake)
    uint8_t shared[32]; crypto_x25519(shared, priv, spub);
    uint8_t zeros[32]; memset(zeros, 0, 32);
    uint8_t empty_hash[32]; sha256("", 0, empty_hash);
    uint8_t early[32]; hmac_sha256(zeros, 32, zeros, 32, early);
    uint8_t derived[32]; expand_label(early, "derived", empty_hash, 32, derived, 32);
    uint8_t hs_secret[32]; hmac_sha256(derived, 32, shared, 32, hs_secret);
    uint8_t th_chsh[32]; th_snapshot(&th, th_chsh);
    uint8_t c_hs[32], s_hs[32];
    derive_secret(hs_secret, "c hs traffic", th_chsh, c_hs);
    derive_secret(hs_secret, "s hs traffic", th_chsh, s_hs);
    uint8_t chs_key[32], chs_iv[12], shs_key[32], shs_iv[12];
    traffic_keys(c_hs, chs_key, chs_iv);
    traffic_keys(s_hs, shs_key, shs_iv);
    uint64_t shs_seq = 0, chs_seq = 0;

    // 5. flight chiffré : EncryptedExtensions, Certificate, CertificateVerify, Finished
    int hslen = 0, done = 0;
    while (!done) {
        int r = read_record(conn, hdr, recbody, &blen, dl);
        if (r) { if (err) *err = "flight chiffré incomplet"; return -1; }
        if (hdr[0] == CT_CCS) continue;
        if (hdr[0] != CT_APPDATA) { if (err) *err = "enregistrement non chiffré inattendu"; return -1; }
        int ctlen = blen - 16; if (ctlen < 0) { if (err) *err = "enregistrement court"; return -1; }
        uint8_t nonce[12]; mk_nonce(nonce, shs_iv, shs_seq++);
        static uint8_t pt[18000];
        if (aead_open(shs_key, nonce, hdr, 5, recbody, ctlen, recbody + ctlen, pt) != 0) {
            if (err) *err = "dechiffrement handshake echoue";
            return -1;
        }
        int pl = ctlen; while (pl > 0 && pt[pl-1] == 0) pl--;
        if (pl == 0) continue;
        int itype = pt[pl-1]; int ilen = pl - 1;
        if (itype == CT_ALERT) { if (err) *err = "alerte TLS (handshake)"; return -1; }
        if (itype != CT_HANDSHAKE) continue;
        if (hslen + ilen > (int)sizeof(hsacc)) { if (err) *err = "flight trop grand"; return -1; }
        memcpy(hsacc + hslen, pt, ilen); hslen += ilen;

        // Parse les messages handshake complets accumulés.
        int q = 0;
        while (q + 4 <= hslen) {
            int mt = hsacc[q];
            int ml = (hsacc[q+1] << 16) | (hsacc[q+2] << 8) | hsacc[q+3];
            if (q + 4 + ml > hslen) break;                  // message incomplet
            if (mt == HS_FINISHED) {
                uint8_t sfk[32]; expand_label(s_hs, "finished", 0, 0, sfk, 32);
                uint8_t h1[32]; th_snapshot(&th, h1);       // CH..CertVerify
                uint8_t exp[32]; hmac_sha256(sfk, 32, h1, 32, exp);
                if (ml != 32 || crypto_verify32(exp, hsacc + q + 4) != 0) {
                    if (err) *err = "Finished serveur invalide";
                    return -1;
                }
                sha256_update(&th, hsacc + q, 4 + ml);      // transcript += server Finished
                done = 1; q += 4 + ml; break;
            } else if (mt == HS_CERT) {
                parse_certificate(hsacc + q + 4, ml);
                sha256_update(&th, hsacc + q, 4 + ml);
                th_snapshot(&th, th_cert); have_th_cert = 1; // transcript CH..Certificate
                q += 4 + ml;
            } else if (mt == HS_CERT_VERIFY) {
                if (have_th_cert && ml >= 4) {
                    int scheme = (hsacc[q+4] << 8) | hsacc[q+5];
                    int slen = (hsacc[q+6] << 8) | hsacc[q+7];
                    if (8 + slen <= 4 + ml)
                        cv_ok = cert_verify_sig(scheme, hsacc + q + 8, slen, th_cert);
                }
                sha256_update(&th, hsacc + q, 4 + ml);
                q += 4 + ml;
            } else {
                sha256_update(&th, hsacc + q, 4 + ml);      // EncryptedExtensions
                q += 4 + ml;
            }
        }
        // Décale le reliquat non parsé en tête.
        if (q > 0) { memcpy(hsacc, hsacc + q, hslen - q); hslen -= q; }
    }

    // 6. secrets applicatifs
    uint8_t derived2[32]; expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
    uint8_t master[32]; hmac_sha256(derived2, 32, zeros, 32, master);
    uint8_t th_sf[32]; th_snapshot(&th, th_sf);            // CH..server Finished
    uint8_t c_ap[32], s_ap[32];
    derive_secret(master, "c ap traffic", th_sf, c_ap);
    derive_secret(master, "s ap traffic", th_sf, s_ap);
    traffic_keys(c_ap, t->cw_key, t->cw_iv);
    traffic_keys(s_ap, t->sr_key, t->sr_iv);
    t->cw_seq = 0; t->sr_seq = 0;

    // 7. client Finished (chiffré avec les clés handshake client)
    uint8_t cfk[32]; expand_label(c_hs, "finished", 0, 0, cfk, 32);
    uint8_t vd[32]; hmac_sha256(cfk, 32, th_sf, 32, vd);
    uint8_t fin[36]; fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32; memcpy(fin + 4, vd, 32);
    send_enc(conn, CT_HANDSHAKE, fin, 36, chs_key, chs_iv, &chs_seq);

    // Vérification du certificat (rapportée, non bloquante) : preuve de clé
    // (CertificateVerify) + chaîne de confiance + hôte + dates.
    if (!cv_ok) { setinfo(t->verify_info, "preuve de cle serveur invalide/non geree"); t->verified = 0; }
    else t->verified = verify_chain(host, t->verify_info);

    t->established = 1;
    return 0;
}

int tls_send(tls_t *t, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    int off = 0;
    while (off < len) {
        int chunk = len - off; if (chunk > 16000) chunk = 16000;
        send_enc(t->conn, CT_APPDATA, p + off, chunk, t->cw_key, t->cw_iv, &t->cw_seq);
        off += chunk;
    }
    return len;
}

int tls_recv(tls_t *t, void *buf, int len) {
    if (t->roff < t->rlen) {
        int n = t->rlen - t->roff; if (n > len) n = len;
        memcpy(buf, t->rbuf + t->roff, n); t->roff += n; return n;
    }
    if (t->eof) return -1;
    uint8_t hdr[5]; int blen; int dl = ms() + 10000;
    int r = read_record(t->conn, hdr, recbody, &blen, dl);
    if (r == -2) return 0;                                  // rien pour l'instant
    if (r) { t->eof = 1; return -1; }
    if (hdr[0] == CT_CCS) return 0;
    if (hdr[0] == CT_ALERT) { t->eof = 1; return -1; }
    int ctlen = blen - 16; if (ctlen < 0) { t->eof = 1; return -1; }
    uint8_t nonce[12]; mk_nonce(nonce, t->sr_iv, t->sr_seq);
    if (aead_open(t->sr_key, nonce, hdr, 5, recbody, ctlen, recbody + ctlen, t->rbuf) != 0) {
        t->eof = 1; return -1;
    }
    t->sr_seq++;
    int pl = ctlen; while (pl > 0 && t->rbuf[pl-1] == 0) pl--;
    if (pl == 0) return 0;
    int itype = t->rbuf[pl-1]; int ilen = pl - 1;
    if (itype == CT_ALERT) { t->eof = 1; return -1; }       // close_notify inclus
    if (itype == CT_HANDSHAKE) return 0;                    // NewSessionTicket : ignoré
    if (itype != CT_APPDATA) return 0;
    t->rlen = ilen; t->roff = 0;
    int n = ilen; if (n > len) n = len;
    memcpy(buf, t->rbuf, n); t->roff = n;
    return n;
}

void tls_close(tls_t *t) {
    if (t->established) {
        uint8_t alert[2] = { 1, 0 };                       // close_notify (warning)
        send_enc(t->conn, CT_ALERT, alert, 2, t->cw_key, t->cw_iv, &t->cw_seq);
    }
    sys_tcp_close(t->conn);
}
