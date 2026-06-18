// =============================================================================
//  kernel/ssh.c -- Client SSH-2 complet
// -----------------------------------------------------------------------------
//  Transport (RFC 4253) + KEX curve25519-sha256 + clé d'hôte ssh-ed25519
//  + chiffre chacha20-poly1305@openssh.com + authentification par mot de passe
//  + canal "session" + exécution distante.
//  S'appuie sur la couche crypto (Monocypher + SHA-256) et la pile TCP.
// =============================================================================
#include "ssh.h"
#include "crypto.h"
#include "klib.h"
#include "pit.h"

#define MSG_DISCONNECT     1
#define MSG_IGNORE         2
#define MSG_DEBUG          4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_KEXINIT        20
#define MSG_NEWKEYS        21
#define MSG_KEX_ECDH_INIT  30
#define MSG_KEX_ECDH_REPLY 31
#define MSG_GLOBAL_REQUEST 80
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_CHANNEL_OPEN     90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA     94
#define MSG_CHANNEL_EXTENDED_DATA 95
#define MSG_CHANNEL_EOF      96
#define MSG_CHANNEL_CLOSE    97
#define MSG_CHANNEL_REQUEST  98
#define MSG_CHANNEL_SUCCESS  99

#define RBUF_SZ 32768
#define PKT_SZ  32768

typedef struct {
    int conn;
    uint8_t rbuf[RBUF_SZ];
    int rpos, rlen;
    bool encrypted;
    uint32_t seq_c2s, seq_s2c;
    uint8_t key_c2s[64], key_s2c[64];
    char v_c[64], v_s[256];
    uint8_t i_c[2048]; int i_c_len;
    uint8_t i_s[2048]; int i_s_len;
    uint8_t session_id[32];
} ssh_t;

// --- Octets gros-boutiens ----------------------------------------------------
static uint32_t rd32(const uint8_t *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

// --- Lecture bufferisée TCP --------------------------------------------------
static bool ssh_fill(ssh_t *s, int want) {
    uint64_t end = pit_ms() + 8000;
    while (s->rlen - s->rpos < want) {
        if (s->rpos > 0) { memmove(s->rbuf, s->rbuf + s->rpos, s->rlen - s->rpos); s->rlen -= s->rpos; s->rpos = 0; }
        if (s->rlen >= RBUF_SZ) return false;
        int n = tcp_recv(s->conn, s->rbuf + s->rlen, RBUF_SZ - s->rlen, 2000);
        if (n > 0) s->rlen += n;
        else if (n < 0 || pit_ms() > end) return false;
    }
    return true;
}
static bool ssh_readn(ssh_t *s, uint8_t *out, int n) {
    if (!ssh_fill(s, n)) return false;
    memcpy(out, s->rbuf + s->rpos, n); s->rpos += n; return true;
}
static bool ssh_readline(ssh_t *s, char *out, int max) {
    int i = 0;
    for (;;) {
        if (!ssh_fill(s, 1)) return false;
        char c = (char)s->rbuf[s->rpos++];
        if (c == '\n') break;
        if (c != '\r' && i < max - 1) out[i++] = c;
    }
    out[i] = 0; return true;
}

// --- Chiffre chacha20-poly1305@openssh.com -----------------------------------
static void chacha_nonce(uint32_t seq, uint8_t nonce[8]) {
    nonce[0]=nonce[1]=nonce[2]=nonce[3]=0;
    nonce[4]=seq>>24; nonce[5]=seq>>16; nonce[6]=seq>>8; nonce[7]=seq;
}

// --- Envoi/réception d'un paquet (clair avant NEWKEYS, chiffré après) --------
static bool ssh_send(ssh_t *s, const uint8_t *payload, int plen) {
    if (!s->encrypted) {
        uint8_t pkt[PKT_SZ];
        int pad = 8 - ((4 + 1 + plen) % 8); if (pad < 4) pad += 8;
        int packet_len = 1 + plen + pad;
        wr32(pkt, packet_len); pkt[4] = pad;
        memcpy(pkt + 5, payload, plen); csprng_bytes(pkt + 5 + plen, pad);
        s->seq_c2s++;
        return tcp_send(s->conn, pkt, 5 + plen + pad) > 0;
    }
    uint8_t *K2 = s->key_c2s, *K1 = s->key_c2s + 32;
    uint8_t nonce[8]; chacha_nonce(s->seq_c2s, nonce);
    int pad = 8 - ((1 + plen) % 8); if (pad < 4) pad += 8;
    int packet_len = 1 + plen + pad;
    uint8_t plain[PKT_SZ]; plain[0] = pad; memcpy(plain + 1, payload, plen); csprng_bytes(plain + 1 + plen, pad);
    uint8_t lenb[4]; wr32(lenb, packet_len);
    uint8_t enclen[4]; crypto_chacha20_djb(enclen, lenb, 4, K1, nonce, 0);
    uint8_t polykey[32], zeros[32]; memset(zeros, 0, 32);
    crypto_chacha20_djb(polykey, zeros, 32, K2, nonce, 0);
    uint8_t ct[PKT_SZ]; crypto_chacha20_djb(ct, plain, packet_len, K2, nonce, 1);
    uint8_t macin[PKT_SZ]; memcpy(macin, enclen, 4); memcpy(macin + 4, ct, packet_len);
    uint8_t mac[16]; crypto_poly1305(mac, macin, 4 + packet_len, polykey);
    uint8_t wire[PKT_SZ]; memcpy(wire, enclen, 4); memcpy(wire + 4, ct, packet_len); memcpy(wire + 4 + packet_len, mac, 16);
    s->seq_c2s++;
    return tcp_send(s->conn, wire, 4 + packet_len + 16) > 0;
}
static bool ssh_recv(ssh_t *s, uint8_t *payload, int *plen) {
    if (!s->encrypted) {
        uint8_t lenb[4]; if (!ssh_readn(s, lenb, 4)) return false;
        uint32_t packet_len = rd32(lenb);
        if (packet_len < 2 || packet_len > PKT_SZ) return false;
        uint8_t buf[PKT_SZ]; if (!ssh_readn(s, buf, packet_len)) return false;
        int pad = buf[0]; *plen = packet_len - pad - 1; if (*plen < 0) return false;
        memcpy(payload, buf + 1, *plen); s->seq_s2c++; return true;
    }
    uint8_t *K2 = s->key_s2c, *K1 = s->key_s2c + 32;
    uint8_t nonce[8]; chacha_nonce(s->seq_s2c, nonce);
    uint8_t enclen[4]; if (!ssh_readn(s, enclen, 4)) return false;
    uint8_t lenb[4]; crypto_chacha20_djb(lenb, enclen, 4, K1, nonce, 0);
    uint32_t packet_len = rd32(lenb);
    if (packet_len < 8 || packet_len > PKT_SZ) return false;
    uint8_t ct[PKT_SZ]; if (!ssh_readn(s, ct, packet_len)) return false;
    uint8_t mac[16]; if (!ssh_readn(s, mac, 16)) return false;
    uint8_t polykey[32], zeros[32]; memset(zeros, 0, 32);
    crypto_chacha20_djb(polykey, zeros, 32, K2, nonce, 0);
    uint8_t macin[PKT_SZ]; memcpy(macin, enclen, 4); memcpy(macin + 4, ct, packet_len);
    uint8_t calc[16]; crypto_poly1305(calc, macin, 4 + packet_len, polykey);
    if (crypto_verify16(calc, mac) != 0) return false;
    uint8_t plain[PKT_SZ]; crypto_chacha20_djb(plain, ct, packet_len, K2, nonce, 1);
    int pad = plain[0]; *plen = packet_len - pad - 1; if (*plen < 0) return false;
    memcpy(payload, plain + 1, *plen); s->seq_s2c++; return true;
}

// --- Construction de chaînes -------------------------------------------------
static int put_str(uint8_t *b, int o, const char *s) { int n=strlen(s); wr32(b+o,n); memcpy(b+o+4,s,n); return o+4+n; }
static int put_bytes(uint8_t *b, int o, const uint8_t *d, int n) { wr32(b+o,n); memcpy(b+o+4,d,n); return o+4+n; }

static void sha_str(sha256_ctx *h, const uint8_t *d, int n) { uint8_t l[4]; wr32(l,n); sha256_update(h,l,4); sha256_update(h,d,n); }
static void sha_mpint(sha256_ctx *h, const uint8_t *d, int n) {
    int i=0; while (i<n && d[i]==0) i++;
    int len=n-i, pad=(len>0 && (d[i]&0x80))?1:0;
    uint8_t l[4]; wr32(l,len+pad); sha256_update(h,l,4);
    if (pad){uint8_t z=0; sha256_update(h,&z,1);}
    if (len) sha256_update(h,d+i,len);
}
static int enc_mpint(uint8_t *out, const uint8_t *d, int n) {
    int i=0; while (i<n && d[i]==0) i++;
    int len=n-i, pad=(len>0 && (d[i]&0x80))?1:0;
    wr32(out, len+pad); int o=4;
    if (pad) out[o++]=0;
    memcpy(out+o, d+i, len); return o+len;
}

static void derive_key(const uint8_t *Kmp, int kml, const uint8_t H[32], char letter,
                       const uint8_t sid[32], uint8_t *out, int outlen) {
    sha256_ctx h; uint8_t blk[32];
    sha256_init(&h); sha256_update(&h,Kmp,kml); sha256_update(&h,H,32);
    sha256_update(&h,(uint8_t*)&letter,1); sha256_update(&h,sid,32); sha256_final(&h,blk);
    int c = outlen<32?outlen:32; memcpy(out,blk,c);
    while (c<outlen) {
        sha256_init(&h); sha256_update(&h,Kmp,kml); sha256_update(&h,H,32);
        sha256_update(&h,out,c); sha256_final(&h,blk);
        int n=(outlen-c<32)?outlen-c:32; memcpy(out+c,blk,n); c+=n;
    }
}

// --- Handshake (versions + KEXINIT + KEX + clé d'hôte + NEWKEYS) -------------
static int ssh_handshake(ssh_t *s) {
    strcpy(s->v_c, "SSH-2.0-MonOS_1.0");
    char hello[80]; strcpy(hello, s->v_c); strcat(hello, "\r\n");
    tcp_send(s->conn, hello, strlen(hello));
    if (!ssh_readline(s, s->v_s, sizeof(s->v_s))) return -1;
    kprintf("[ssh] serveur : %s\n", s->v_s);

    uint8_t kx[2048]; int o=0;
    kx[o++]=MSG_KEXINIT; csprng_bytes(kx+o,16); o+=16;
    o=put_str(kx,o,"curve25519-sha256");
    o=put_str(kx,o,"ssh-ed25519");
    o=put_str(kx,o,"chacha20-poly1305@openssh.com");
    o=put_str(kx,o,"chacha20-poly1305@openssh.com");
    o=put_str(kx,o,""); o=put_str(kx,o,"");
    o=put_str(kx,o,"none"); o=put_str(kx,o,"none");
    o=put_str(kx,o,""); o=put_str(kx,o,"");
    kx[o++]=0; wr32(kx+o,0); o+=4;
    memcpy(s->i_c,kx,o); s->i_c_len=o;
    ssh_send(s,kx,o);

    int plen;
    if (!ssh_recv(s,s->i_s,&plen) || s->i_s[0]!=MSG_KEXINIT) return -1;
    s->i_s_len=plen;

    uint8_t e_priv[32], q_c[32];
    csprng_bytes(e_priv,32); crypto_x25519_public_key(q_c,e_priv);
    uint8_t init[64]; o=0; init[o++]=MSG_KEX_ECDH_INIT; o=put_bytes(init,o,q_c,32);
    ssh_send(s,init,o);

    uint8_t reply[4096];
    if (!ssh_recv(s,reply,&plen) || reply[0]!=MSG_KEX_ECDH_REPLY) return -1;
    int p=1;
    uint32_t ks_len=rd32(reply+p); p+=4; uint8_t *k_s=reply+p; p+=ks_len;
    uint32_t qs_len=rd32(reply+p); p+=4; uint8_t *q_s=reply+p; p+=qs_len;
    uint32_t sig_len=rd32(reply+p); p+=4; uint8_t *sig_blob=reply+p; p+=sig_len;
    if (qs_len!=32) return -1;

    uint8_t K[32]; crypto_x25519(K,e_priv,q_s);
    uint8_t H[32]; sha256_ctx hc; sha256_init(&hc);
    sha_str(&hc,(uint8_t*)s->v_c,strlen(s->v_c));
    sha_str(&hc,(uint8_t*)s->v_s,strlen(s->v_s));
    sha_str(&hc,s->i_c,s->i_c_len); sha_str(&hc,s->i_s,s->i_s_len);
    sha_str(&hc,k_s,ks_len); sha_str(&hc,q_c,32); sha_str(&hc,q_s,32);
    sha_mpint(&hc,K,32); sha256_final(&hc,H);
    memcpy(s->session_id,H,32);

    int kp=0; uint32_t t1=rd32(k_s+kp); kp+=4+t1;
    uint32_t hkpub_len=rd32(k_s+kp); kp+=4; uint8_t *hostpub=k_s+kp;
    int sp=0; uint32_t t2=rd32(sig_blob+sp); sp+=4+t2;
    uint32_t sg_len=rd32(sig_blob+sp); sp+=4; uint8_t *sig=sig_blob+sp;
    if (hkpub_len!=32 || sg_len!=64) return -1;
    if (crypto_ed25519_check(sig,hostpub,H,32)!=0) { kprintf("[ssh] signature hote INVALIDE\n"); return -1; }
    kprintf("[ssh] signature d'hote verifiee\n");

    uint8_t Kmp[40]; int kml=enc_mpint(Kmp,K,32);
    derive_key(Kmp,kml,H,'C',s->session_id,s->key_c2s,64);
    derive_key(Kmp,kml,H,'D',s->session_id,s->key_s2c,64);

    uint8_t nk=MSG_NEWKEYS; ssh_send(s,&nk,1);
    if (!ssh_recv(s,reply,&plen) || reply[0]!=MSG_NEWKEYS) return -1;
    s->encrypted=true;
    kprintf("[ssh] transport chiffre etabli\n");
    return 0;
}

// Saute les messages transport non pertinents (IGNORE/DEBUG/GLOBAL_REQUEST...).
static bool ssh_recv_useful(ssh_t *s, uint8_t *payload, int *plen) {
    for (int i = 0; i < 16; i++) {
        if (!ssh_recv(s, payload, plen)) return false;
        uint8_t t = payload[0];
        if (t==MSG_IGNORE || t==MSG_DEBUG || t==MSG_GLOBAL_REQUEST || t==MSG_USERAUTH_BANNER) continue;
        return true;
    }
    return false;
}

// --- Authentification par mot de passe ---------------------------------------
static int ssh_auth_password(ssh_t *s, const char *user, const char *password) {
    uint8_t req[512]; int o=0;
    req[o++]=MSG_SERVICE_REQUEST; o=put_str(req,o,"ssh-userauth");
    ssh_send(s,req,o);
    int plen; uint8_t p[512];
    if (!ssh_recv_useful(s,p,&plen) || p[0]!=MSG_SERVICE_ACCEPT) return -1;

    o=0; req[o++]=MSG_USERAUTH_REQUEST;
    o=put_str(req,o,user);
    o=put_str(req,o,"ssh-connection");
    o=put_str(req,o,"password");
    req[o++]=0;
    o=put_str(req,o,password);
    ssh_send(s,req,o);
    if (!ssh_recv_useful(s,p,&plen)) return -1;
    if (p[0]==MSG_USERAUTH_SUCCESS) { kprintf("[ssh] authentifie (mot de passe)\n"); return 0; }
    kprintf("[ssh] authentification refusee\n");
    return -1;
}

// --- Exécution distante (canal session + exec) -------------------------------
//  Renvoie le nombre d'octets de sortie écrits dans out.
int ssh_client_exec(ip4_t ip, uint16_t port, const char *user, const char *password,
                    const char *command, char *out, int outmax) {
    static ssh_t s; memset(&s,0,sizeof(s));
    s.conn = tcp_connect(ip, port);
    if (s.conn < 0) { kprintf("[ssh] connexion echec\n"); return -1; }
    if (ssh_handshake(&s) != 0) { tcp_close(s.conn); return -1; }
    if (ssh_auth_password(&s, user, password) != 0) { tcp_close(s.conn); return -1; }

    // Ouverture du canal "session".
    uint8_t req[1024]; int o=0;
    req[o++]=MSG_CHANNEL_OPEN; o=put_str(req,o,"session");
    wr32(req+o,0); o+=4;             // notre n° de canal
    wr32(req+o,0x100000); o+=4;      // fenêtre initiale
    wr32(req+o,0x4000); o+=4;        // taille max de paquet
    ssh_send(&s,req,o);
    int plen; uint8_t p[PKT_SZ];
    if (!ssh_recv_useful(&s,p,&plen) || p[0]!=MSG_CHANNEL_OPEN_CONFIRMATION) { kprintf("[ssh] ouverture canal echec\n"); tcp_close(s.conn); return -1; }
    uint32_t remote_ch = rd32(p+5);

    // Demande "exec".
    o=0; req[o++]=MSG_CHANNEL_REQUEST; wr32(req+o,remote_ch); o+=4;
    o=put_str(req,o,"exec"); req[o++]=1; o=put_str(req,o,command);
    ssh_send(&s,req,o);

    // Lecture de la sortie.
    int total=0;
    for (int guard=0; guard<2000; guard++) {
        if (!ssh_recv(&s,p,&plen)) break;
        uint8_t t=p[0];
        if (t==MSG_CHANNEL_DATA) {
            uint32_t dlen=rd32(p+5);
            int n=(int)dlen; if (total+n>outmax-1) n=outmax-1-total;
            if (n>0){ memcpy(out+total,p+9,n); total+=n; }
        } else if (t==MSG_CHANNEL_EXTENDED_DATA) {
            uint32_t dlen=rd32(p+9);
            int n=(int)dlen; if (total+n>outmax-1) n=outmax-1-total;
            if (n>0){ memcpy(out+total,p+13,n); total+=n; }
        } else if (t==MSG_CHANNEL_EOF || t==MSG_CHANNEL_CLOSE) {
            break;
        }
        // (CHANNEL_REQUEST exit-status, WINDOW_ADJUST, SUCCESS : ignorés)
    }
    out[total]=0;
    tcp_close(s.conn);
    return total;
}

// --- Jalon de test : handshake seul ------------------------------------------
int ssh_handshake_test(ip4_t ip, uint16_t port) {
    static ssh_t s; memset(&s,0,sizeof(s));
    s.conn = tcp_connect(ip, port);
    if (s.conn < 0) { kprintf("[ssh] connexion echec\n"); return -1; }
    int r = ssh_handshake(&s);
    tcp_close(s.conn);
    return r;
}
