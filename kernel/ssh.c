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
    uint8_t i_c[4096]; int i_c_len;
    uint8_t i_s[4096]; int i_s_len;
    uint8_t session_id[32];
} ssh_t;

// --- Octets gros-boutiens ----------------------------------------------------
static uint32_t rd32(const uint8_t *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

// --- E/S réseau NON BLOQUANTES (tâche noyau sshd) ----------------------------
//  On utilise les sockets non bloquantes (tcp_read/tcp_write) et on cède le CPU
//  par 'hlt' en attendant : la tâche réseau tourne alors et remplit les tampons.
//  L'accès aux connexions se fait sous 'cli' (exclusion avec la tâche réseau).
static int ssh_net_read(int conn, uint8_t *buf, int len, uint64_t deadline) {
    for (;;) {
        __asm__ volatile ("cli");
        int n = tcp_read(conn, buf, len);
        __asm__ volatile ("sti");
        if (n != 0) return n;                          // données (>0) ou fermé (-1)
        if (pit_ms() > deadline) return 0;             // délai dépassé
        __asm__ volatile ("hlt");                      // laisse tourner la tâche réseau
    }
}
static bool ssh_net_write(int conn, const uint8_t *buf, int len) {
    int off = 0; uint64_t dl = pit_ms() + 8000;
    while (off < len) {
        __asm__ volatile ("cli");
        int n = tcp_write(conn, buf + off, len - off);
        __asm__ volatile ("sti");
        if (n > 0) { off += n; dl = pit_ms() + 8000; }
        else if (n < 0) return false;
        else { if (pit_ms() > dl) return false; __asm__ volatile ("hlt"); }
    }
    return true;
}

// --- Lecture bufferisée TCP --------------------------------------------------
static bool ssh_fill(ssh_t *s, int want) {
    uint64_t end = pit_ms() + 15000;
    while (s->rlen - s->rpos < want) {
        if (s->rpos > 0) { memmove(s->rbuf, s->rbuf + s->rpos, s->rlen - s->rpos); s->rlen -= s->rpos; s->rpos = 0; }
        if (s->rlen >= RBUF_SZ) return false;
        int n = ssh_net_read(s->conn, s->rbuf + s->rlen, RBUF_SZ - s->rlen, end);
        if (n > 0) s->rlen += n;
        else return false;                             // fermé ou délai dépassé
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

// --- Tampons de travail (HORS PILE) ------------------------------------------
//  La pile d'une tâche noyau ne fait que 16 Kio, or un paquet SSH peut atteindre
//  PKT_SZ. On place donc les gros tampons en mémoire statique. Une SEULE session
//  est traitée à la fois (la tâche sshd est séquentielle), donc le partage est
//  sûr : pas de réentrance entre l'envoi et la réception d'un même paquet.
static uint8_t snd_b1[PKT_SZ], snd_b2[PKT_SZ], snd_b3[PKT_SZ], snd_b4[PKT_SZ];
static uint8_t rcv_b1[PKT_SZ], rcv_b2[PKT_SZ], rcv_b3[PKT_SZ];

// --- Chiffre chacha20-poly1305@openssh.com -----------------------------------
static void chacha_nonce(uint32_t seq, uint8_t nonce[8]) {
    nonce[0]=nonce[1]=nonce[2]=nonce[3]=0;
    nonce[4]=seq>>24; nonce[5]=seq>>16; nonce[6]=seq>>8; nonce[7]=seq;
}

// --- Envoi/réception d'un paquet (clair avant NEWKEYS, chiffré après) --------
static bool ssh_send(ssh_t *s, const uint8_t *payload, int plen) {
    if (!s->encrypted) {
        uint8_t *pkt = snd_b1;
        int pad = 8 - ((4 + 1 + plen) % 8); if (pad < 4) pad += 8;
        int packet_len = 1 + plen + pad;
        wr32(pkt, packet_len); pkt[4] = pad;
        memcpy(pkt + 5, payload, plen); csprng_bytes(pkt + 5 + plen, pad);
        s->seq_c2s++;
        return ssh_net_write(s->conn, pkt, 5 + plen + pad);
    }
    uint8_t *K2 = s->key_c2s, *K1 = s->key_c2s + 32;
    uint8_t nonce[8]; chacha_nonce(s->seq_c2s, nonce);
    int pad = 8 - ((1 + plen) % 8); if (pad < 4) pad += 8;
    int packet_len = 1 + plen + pad;
    uint8_t *plain = snd_b1; plain[0] = pad; memcpy(plain + 1, payload, plen); csprng_bytes(plain + 1 + plen, pad);
    uint8_t lenb[4]; wr32(lenb, packet_len);
    uint8_t enclen[4]; crypto_chacha20_djb(enclen, lenb, 4, K1, nonce, 0);
    uint8_t polykey[32], zeros[32]; memset(zeros, 0, 32);
    crypto_chacha20_djb(polykey, zeros, 32, K2, nonce, 0);
    uint8_t *ct = snd_b2; crypto_chacha20_djb(ct, plain, packet_len, K2, nonce, 1);
    uint8_t *macin = snd_b3; memcpy(macin, enclen, 4); memcpy(macin + 4, ct, packet_len);
    uint8_t mac[16]; crypto_poly1305(mac, macin, 4 + packet_len, polykey);
    uint8_t *wire = snd_b4; memcpy(wire, enclen, 4); memcpy(wire + 4, ct, packet_len); memcpy(wire + 4 + packet_len, mac, 16);
    s->seq_c2s++;
    return ssh_net_write(s->conn, wire, 4 + packet_len + 16);
}
static bool ssh_recv(ssh_t *s, uint8_t *payload, int *plen) {
    if (!s->encrypted) {
        uint8_t lenb[4]; if (!ssh_readn(s, lenb, 4)) return false;
        uint32_t packet_len = rd32(lenb);
        if (packet_len < 2 || packet_len > PKT_SZ) return false;
        uint8_t *buf = rcv_b1; if (!ssh_readn(s, buf, packet_len)) return false;
        int pad = buf[0]; *plen = packet_len - pad - 1; if (*plen < 0) return false;
        memcpy(payload, buf + 1, *plen); s->seq_s2c++; return true;
    }
    uint8_t *K2 = s->key_s2c, *K1 = s->key_s2c + 32;
    uint8_t nonce[8]; chacha_nonce(s->seq_s2c, nonce);
    uint8_t enclen[4]; if (!ssh_readn(s, enclen, 4)) return false;
    uint8_t lenb[4]; crypto_chacha20_djb(lenb, enclen, 4, K1, nonce, 0);
    uint32_t packet_len = rd32(lenb);
    if (packet_len < 8 || packet_len > PKT_SZ) return false;
    uint8_t *ct = rcv_b1; if (!ssh_readn(s, ct, packet_len)) return false;
    uint8_t mac[16]; if (!ssh_readn(s, mac, 16)) return false;
    uint8_t polykey[32], zeros[32]; memset(zeros, 0, 32);
    crypto_chacha20_djb(polykey, zeros, 32, K2, nonce, 0);
    uint8_t *macin = rcv_b2; memcpy(macin, enclen, 4); memcpy(macin + 4, ct, packet_len);
    uint8_t calc[16]; crypto_poly1305(calc, macin, 4 + packet_len, polykey);
    if (crypto_verify16(calc, mac) != 0) return false;
    uint8_t *plain = rcv_b3; crypto_chacha20_djb(plain, ct, packet_len, K2, nonce, 1);
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
    strcpy(s->v_c, "SSH-2.0-sexOs_1.0");
    char hello[80]; strcpy(hello, s->v_c); strcat(hello, "\r\n");
    ssh_net_write(s->conn, (uint8_t*)hello, strlen(hello));
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
        if (t==MSG_IGNORE || t==MSG_DEBUG || t==MSG_GLOBAL_REQUEST ||
            t==MSG_USERAUTH_BANNER || t==7 /* EXT_INFO */) continue;
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
    int plen; static uint8_t p[PKT_SZ];     // hors pile (16 Kio seulement)
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

// =============================================================================
//  SERVEUR SSH
// =============================================================================
#include "vfs.h"
#include "users.h"
#include "rtc.h"
#include "pmm.h"

static uint8_t host_sk[64], host_pk[32];
static bool server_ready;

void ssh_server_init(void) {
    uint8_t seed[32];
    csprng_bytes(seed, 32);
    crypto_ed25519_key_pair(host_sk, host_pk, seed);
    tcp_listen_port(22);
    server_ready = true;
    kprintf("[sshd] cle d'hote ed25519 generee, ecoute sur le port 22\n");
}

const uint8_t *ssh_host_pubkey(void) { return host_pk; }

// --- Mini-shell pour les sessions distantes (sortie vers un tampon) ----------
static const user_t *find_user(const char *name) {
    for (int i = 0; i < users_count(); i++)
        if (strcmp(users_get(i)->name, name) == 0) return users_get(i);
    return NULL;
}

// =============================================================================
//  AUTHENTIFICATION PAR CLÉ PUBLIQUE (ed25519) — base64 + authorized_keys
// =============================================================================
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode 'inlen' octets en base64 (sans saut de ligne). Renvoie la longueur.
static int b64_encode(const uint8_t *in, int inlen, char *out, int outmax) {
    int o = 0;
    for (int i = 0; i < inlen; i += 3) {
        int n = inlen - i; uint32_t v = in[i] << 16;
        if (n > 1) v |= in[i+1] << 8;
        if (n > 2) v |= in[i+2];
        if (o + 4 >= outmax) break;
        out[o++] = B64[(v>>18)&63];
        out[o++] = B64[(v>>12)&63];
        out[o++] = (n > 1) ? B64[(v>>6)&63] : '=';
        out[o++] = (n > 2) ? B64[v&63]      : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;                                   // '=' ou caractère ignoré
}

// Décode du base64 (ignore espaces et '='). Renvoie le nombre d'octets, -1 si erreur.
static int b64_decode(const char *in, int inlen, uint8_t *out, int outmax) {
    int o = 0; uint32_t buf = 0; int bits = 0;
    for (int i = 0; i < inlen; i++) {
        int v = b64_val(in[i]);
        if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= outmax) return -1; out[o++] = (buf >> bits) & 0xFF; }
    }
    return o;
}

// Extrait la clé ed25519 brute (32 o) d'un blob SSH "ssh-ed25519"+clé.
static bool ssh_blob_to_key(const uint8_t *blob, int blen, uint8_t key[32]) {
    if (blen < 4) return false;
    uint32_t t1 = rd32(blob); if (t1 != 11 || blen < 4 + 11 + 4 + 32) return false;
    if (memcmp(blob + 4, "ssh-ed25519", 11) != 0) return false;
    uint32_t klen = rd32(blob + 4 + 11); if (klen != 32) return false;
    memcpy(key, blob + 4 + 11 + 4, 32);
    return true;
}

// Chemin du authorized_keys d'un utilisateur dans buf : /home/<user>/.ssh/authorized_keys
static void authk_path(const char *user, char *buf, int max) {
    const user_t *u = find_user(user);
    const char *home = (u && u->home[0]) ? u->home : "/";
    int o = 0;
    for (const char *p = home; *p && o < max - 1; p++) buf[o++] = *p;
    const char *suf = "/.ssh/authorized_keys";
    for (const char *p = suf; *p && o < max - 1; p++) buf[o++] = *p;
    buf[o] = 0;
}

// Vérifie qu'une clé publique figure dans le authorized_keys de l'utilisateur.
//  Format des lignes : "ssh-ed25519 <base64> [commentaire]".
static bool ssh_key_authorized(const char *user, const uint8_t key[32]) {
    char path[160]; authk_path(user, path, sizeof(path));
    vfs_node_t *f = vfs_resolve(path);
    if (!f || f->type != VFS_FILE || !f->data) return false;

    const char *txt = (const char *)f->data; size_t n = f->size;
    size_t i = 0;
    while (i < n) {
        while (i < n && (txt[i]=='\n' || txt[i]=='\r' || txt[i]==' ' || txt[i]=='\t')) i++;
        size_t ls = i; while (i < n && txt[i] != '\n') i++;
        size_t le = i;                                   // [ls, le) = une ligne
        // type
        size_t a = ls; while (a < le && txt[a] != ' ') a++;
        if (a - ls != 11 || memcmp(txt + ls, "ssh-ed25519", 11) != 0) continue;
        while (a < le && txt[a] == ' ') a++;
        size_t bs = a; while (a < le && txt[a] != ' ') a++;   // [bs, a) = base64
        uint8_t blob[128];
        int bl = b64_decode(txt + bs, (int)(a - bs), blob, sizeof(blob));
        uint8_t k[32];
        if (bl > 0 && ssh_blob_to_key(blob, bl, k) && memcmp(k, key, 32) == 0) return true;
    }
    return false;
}

// Ajoute une ligne authorized_keys (déjà au format "ssh-ed25519 <b64> [cmt]").
//  Crée /home/<user>/.ssh/authorized_keys au besoin. Renvoie true si ajouté.
static bool ssh_authk_add_line(const char *user, const char *line) {
    const user_t *u = find_user(user);
    const char *home = (u && u->home[0]) ? u->home : "/";
    vfs_node_t *hd = vfs_resolve(home); if (!hd) return false;
    vfs_node_t *ssh = vfs_lookup(hd, ".ssh");
    if (!ssh) ssh = vfs_create(hd, ".ssh", VFS_DIR);
    if (!ssh) return false;
    vfs_node_t *f = vfs_lookup(ssh, "authorized_keys");
    if (!f) f = vfs_create(ssh, "authorized_keys", VFS_FILE);
    if (!f) return false;
    size_t off = f->size;
    if (off > 0 && f->data && f->data[off-1] != '\n') {     // assure un saut de ligne
        char nl = '\n'; vfs_write(f, off, &nl, 1); off += 1;
    }
    vfs_write(f, off, line, (size_t)strlen(line));
    char nl = '\n'; vfs_write(f, f->size, &nl, 1);
    return true;
}

// Construit le blob SSH d'une clé publique ed25519 : string("ssh-ed25519")+string(pk).
static int ssh_pubkey_blob(const uint8_t pk[32], uint8_t *out) {
    int o=0; o=put_str(out,o,"ssh-ed25519"); o=put_bytes(out,o,pk,32); return o;
}

// Empreinte OpenSSH "SHA256:<base64 sans '='>" de la clé publique pk.
static void ssh_fingerprint(const uint8_t pk[32], char *out, int max) {
    uint8_t blob[64]; int bl=ssh_pubkey_blob(pk,blob);
    uint8_t h[32]; sha256(blob,bl,h);
    char b64[64]; int n=b64_encode(h,32,b64,sizeof(b64));
    while (n>0 && b64[n-1]=='=') b64[--n]=0;                 // OpenSSH retire le bourrage
    int o=0; const char *pre="SHA256:";
    for (const char *q=pre; *q && o<max-1; q++) out[o++]=*q;
    for (int i=0;i<n && o<max-1;i++) out[o++]=b64[i];
    out[o]=0;
}

// Exporte une clé privée ed25519 au format "OPENSSH PRIVATE KEY" (non chiffrée),
//  prête à être enregistrée côté client (ex: ~/.ssh/id_ed25519). Renvoie la taille.
static int ssh_export_privkey(const uint8_t sk[64], const uint8_t pk[32],
                              const char *comment, char *out, int outmax) {
    static uint8_t bin[512]; int b=0;
    const char magic[] = "openssh-key-v1";                  // 14 + '\0'
    memcpy(bin+b, magic, 15); b+=15;
    b=put_str((uint8_t*)bin,b,"none");                      // chiffrement
    b=put_str((uint8_t*)bin,b,"none");                      // kdf
    wr32(bin+b,0); b+=4;                                    // kdfoptions vide
    wr32(bin+b,1); b+=4;                                    // 1 clé
    uint8_t pub[64]; int pl=ssh_pubkey_blob(pk,pub);
    b=put_bytes(bin,b,pub,pl);                              // blob clé publique
    // Section privée (sera elle-même une string).
    static uint8_t priv[256]; int pv=0;
    uint32_t chk = 0x53455821;                              // entier de contrôle (x2)
    wr32(priv+pv,chk); pv+=4; wr32(priv+pv,chk); pv+=4;
    pv=put_str(priv,pv,"ssh-ed25519");
    pv=put_bytes(priv,pv,pk,32);
    pv=put_bytes(priv,pv,sk,64);                            // seed||pub (64 o)
    pv=put_str(priv,pv,comment);
    int pad=1; while (pv % 8 != 0) priv[pv++]=pad++;        // bourrage 1,2,3,...
    b=put_bytes(bin,b,priv,pv);
    // Encadrement PEM, base64 sur 70 colonnes.
    char body[1024]; int bn=b64_encode((uint8_t*)bin,b,body,sizeof(body));
    int o=0; const char *hdr="-----BEGIN OPENSSH PRIVATE KEY-----\n";
    for (const char *q=hdr; *q && o<outmax-1; q++) out[o++]=*q;
    for (int i=0;i<bn && o<outmax-2;) { out[o++]=body[i++]; if (i%70==0) out[o++]='\n'; }
    if (bn%70!=0 && o<outmax-1) out[o++]='\n';
    const char *ftr="-----END OPENSSH PRIVATE KEY-----\n";
    for (const char *q=ftr; *q && o<outmax-1; q++) out[o++]=*q;
    out[o]=0; return o;
}

// Texte de la clé publique d'hôte + empreinte SHA256 (affichage local/distant).
int ssh_hostkey_text(char *out, int max) {
    uint8_t blob[64]; int bl=ssh_pubkey_blob(host_pk,blob);
    char b64[128]; b64_encode(blob,bl,b64,sizeof(b64));
    char fp[80]; ssh_fingerprint(host_pk,fp,sizeof(fp));
    int o=0;
    for (const char *q="ssh-ed25519 "; *q && o<max-1; q++) out[o++]=*q;
    for (int i=0;b64[i] && o<max-1;i++) out[o++]=b64[i];
    if (o<max-1) out[o++]='\n';
    for (const char *q="empreinte : "; *q && o<max-1; q++) out[o++]=*q;
    for (int i=0;fp[i] && o<max-1;i++) out[o++]=fp[i];
    if (o<max-1) out[o++]='\n';
    out[o]=0; return o;
}

// Génère une paire ed25519, ajoute la clé publique aux clés autorisées de 'user'
//  et écrit (clé publique + clé privée PEM) dans out. Renvoie la taille.
int ssh_keygen_text(const char *user, char *out, int max) {
    uint8_t seed[32], sk[64], pk[32];
    csprng_bytes(seed,32);
    crypto_ed25519_key_pair(sk,pk,seed);
    uint8_t blob[64]; int bl=ssh_pubkey_blob(pk,blob);
    char b64[128]; b64_encode(blob,bl,b64,sizeof(b64));
    char line[160]; int lo=0;
    for (const char *q="ssh-ed25519 "; *q; q++) line[lo++]=*q;
    for (int i=0;b64[i];i++) line[lo++]=b64[i];
    for (const char *q=" sexos-keygen"; *q; q++) line[lo++]=*q;
    line[lo]=0;
    if (user) ssh_authk_add_line(user, line);
    int o=0;
    for (const char *q="cle publique (ajoutee a authorized_keys) :\n"; *q && o<max-1; q++) out[o++]=*q;
    for (int i=0;line[i] && o<max-1;i++) out[o++]=line[i];
    for (const char *q="\n\ncle privee (enregistrez-la cote client, ex: ~/.ssh/id_ed25519) :\n"; *q && o<max-1; q++) out[o++]=*q;
    o += ssh_export_privkey(sk, pk, "sexos-keygen", out+o, max-o);
    out[o]=0; return o;
}

static int shell_exec_one(const char *user, const char *line, char *out, int outmax) {
    int n = 0;
    #define OUT(s) do { for (const char *q=(s); *q && n<outmax-1; q++) out[n++]=*q; } while(0)
    const user_t *u = find_user(user);
    vfs_node_t *cwd = vfs_resolve(u ? u->home : "/"); if (!cwd) cwd = vfs_root();

    char cmd[64]; int i = 0;
    while (line[i] && line[i] != ' ' && i < 63) { cmd[i] = line[i]; i++; }
    cmd[i] = 0;
    const char *arg = line + i; while (*arg == ' ') arg++;

    if (strcmp(cmd, "echo") == 0) { OUT(arg); OUT("\n"); }
    else if (strcmp(cmd, "whoami") == 0) { OUT(user); OUT("\n"); }
    else if (strcmp(cmd, "pwd") == 0) { char p[256]; vfs_path(cwd,p,sizeof(p)); OUT(p); OUT("\n"); }
    else if (strcmp(cmd, "uname") == 0) { OUT("sexOs 2.0 x86_64\n"); }
    else if (strcmp(cmd, "date") == 0) { rtc_time_t t; rtc_now(&t); char b[24]; rtc_format(&t,b); OUT(b); OUT("\n"); }
    else if (strcmp(cmd, "id") == 0) { OUT("user="); OUT(user); OUT(u && u->is_admin ? " (admin)\n" : " (standard)\n"); }
    else if (strcmp(cmd, "help") == 0) { OUT("commandes: echo ls cat pwd whoami id uname date sysinfo about help\n"
        "           hostkey pubkey pubkey-add ssh-keygen\n"); }
    else if (strcmp(cmd, "about") == 0) { OUT("sexOs v2 -- shell SSH distant\n  D\n  |\n  |\n  8\n"); }
    else if (strcmp(cmd, "sysinfo") == 0) {
        char b[24]; OUT("Memoire: "); utoa(pmm_total_bytes()/(1024*1024),b,10); OUT(b); OUT(" Mio\n");
    }
    else if (strcmp(cmd, "ls") == 0) {
        vfs_node_t *d = (arg[0]=='/') ? vfs_resolve(arg) : (arg[0] ? vfs_lookup(cwd,arg) : cwd);
        if (!d) OUT("ls: introuvable\n");
        else for (vfs_node_t *c=d->children; c; c=c->next) { OUT(c->name); if (c->type==VFS_DIR) OUT("/"); OUT("\n"); }
    }
    else if (strcmp(cmd, "cat") == 0) {
        vfs_node_t *f = (arg[0]=='/') ? vfs_resolve(arg) : vfs_lookup(cwd,arg);
        if (!f || f->type!=VFS_FILE) OUT("cat: introuvable\n");
        else for (size_t k=0;k<f->size && n<outmax-1;k++) out[n++]=f->data[k];
    }
    else if (strcmp(cmd, "hostkey") == 0) {
        // Clé publique d'hôte (à ajouter au known_hosts) + empreinte SHA256.
        static char hk[256]; ssh_hostkey_text(hk, sizeof hk); OUT(hk);
    }
    else if (strcmp(cmd, "pubkey") == 0 || strcmp(cmd, "authorized-keys") == 0) {
        // Liste les clés autorisées de l'utilisateur.
        char path[160]; authk_path(user,path,sizeof(path));
        vfs_node_t *f = vfs_resolve(path);
        if (!f || f->type!=VFS_FILE || f->size==0) OUT("aucune cle autorisee\n");
        else for (size_t k=0;k<f->size && n<outmax-1;k++) out[n++]=f->data[k];
    }
    else if (strcmp(cmd, "pubkey-add") == 0) {
        // Enregistre une clé publique : pubkey-add ssh-ed25519 <base64> [commentaire]
        bool ok=false;
        if (strncmp(arg,"ssh-ed25519 ",12)==0) {
            const char *b=arg+12; while (*b==' ') b++;
            const char *e=b; while (*e && *e!=' ') e++;
            uint8_t blob[128]; int bl=b64_decode(b,(int)(e-b),blob,sizeof(blob));
            uint8_t k[32];
            if (bl>0 && ssh_blob_to_key(blob,bl,k)) ok=ssh_authk_add_line(user,arg);
        }
        OUT(ok ? "cle ajoutee\n" : "cle invalide (attendu: ssh-ed25519 <base64>)\n");
    }
    else if (strcmp(cmd, "ssh-keygen") == 0) {
        // Genere une paire ed25519, ajoute la publique aux cles autorisees et
        // imprime la cle privee (a enregistrer cote client comme identite).
        static char kb[1024]; ssh_keygen_text(user, kb, sizeof kb); OUT(kb);
    }
    else if (cmd[0]) { OUT(cmd); OUT(": commande inconnue\n"); }
    out[n] = 0;
    #undef OUT
    return n;
}

// Exécute une ligne en découpant sur ';' (plusieurs commandes).
static int shell_exec(const char *user, const char *line, char *out, int outmax) {
    int n = 0;
    char buf[512];
    const char *p = line;
    while (*p && n < outmax - 1) {
        int i = 0;
        while (*p && *p != ';' && i < 511) buf[i++] = *p++;
        buf[i] = 0;
        if (*p == ';') p++;
        char *c = buf; while (*c == ' ') c++;
        if (*c) n += shell_exec_one(user, c, out + n, outmax - n);
    }
    out[n] = 0;
    return n;
}

// --- Handshake côté serveur --------------------------------------------------
static int ssh_server_handshake(ssh_t *s) {
    strcpy(s->v_s, "SSH-2.0-sexOs_1.0");
    char hello[80]; strcpy(hello, s->v_s); strcat(hello, "\r\n");
    if (!ssh_net_write(s->conn, (uint8_t*)hello, strlen(hello))) { kprintf("[sshd] envoi version echec\n"); return -1; }
    if (!ssh_readline(s, s->v_c, sizeof(s->v_c))) { kprintf("[sshd] lecture version client echec\n"); return -1; }
    kprintf("[sshd] client : %s\n", s->v_c);

    uint8_t kx[2048]; int o=0;
    kx[o++]=MSG_KEXINIT; csprng_bytes(kx+o,16); o+=16;
    o=put_str(kx,o,"curve25519-sha256"); o=put_str(kx,o,"ssh-ed25519");
    o=put_str(kx,o,"chacha20-poly1305@openssh.com"); o=put_str(kx,o,"chacha20-poly1305@openssh.com");
    o=put_str(kx,o,""); o=put_str(kx,o,""); o=put_str(kx,o,"none"); o=put_str(kx,o,"none");
    o=put_str(kx,o,""); o=put_str(kx,o,""); kx[o++]=0; wr32(kx+o,0); o+=4;
    memcpy(s->i_s,kx,o); s->i_s_len=o;
    ssh_send(s,kx,o);

    int plen;
    if (!ssh_recv(s,s->i_c,&plen) || s->i_c[0]!=MSG_KEXINIT) { kprintf("[sshd] KEXINIT client echec\n"); return -1; }
    s->i_c_len=plen;

    uint8_t reply[1024];
    if (!ssh_recv(s,reply,&plen) || reply[0]!=MSG_KEX_ECDH_INIT) { kprintf("[sshd] ECDH_INIT echec (t=%d)\n", reply[0]); return -1; }
    uint32_t qc_len=rd32(reply+1); if (qc_len!=32) return -1;
    uint8_t q_c[32]; memcpy(q_c, reply+5, 32);

    uint8_t e_priv[32], q_s[32];
    csprng_bytes(e_priv,32); crypto_x25519_public_key(q_s,e_priv);
    uint8_t K[32]; crypto_x25519(K,e_priv,q_c);

    // Blob de clé d'hôte K_S = string("ssh-ed25519") + string(host_pk).
    uint8_t ksb[128]; int kl=0; kl=put_str(ksb,kl,"ssh-ed25519"); kl=put_bytes(ksb,kl,host_pk,32);

    // Hash d'échange.
    uint8_t H[32]; sha256_ctx hc; sha256_init(&hc);
    sha_str(&hc,(uint8_t*)s->v_c,strlen(s->v_c));
    sha_str(&hc,(uint8_t*)s->v_s,strlen(s->v_s));
    sha_str(&hc,s->i_c,s->i_c_len); sha_str(&hc,s->i_s,s->i_s_len);
    sha_str(&hc,ksb,kl); sha_str(&hc,q_c,32); sha_str(&hc,q_s,32);
    sha_mpint(&hc,K,32); sha256_final(&hc,H);
    memcpy(s->session_id,H,32);

    // Signature de H avec notre clé d'hôte.
    uint8_t sig[64]; crypto_ed25519_sign(sig, host_sk, H, 32);
    uint8_t sb[128]; int sl=0; sl=put_str(sb,sl,"ssh-ed25519"); sl=put_bytes(sb,sl,sig,64);

    // KEX_ECDH_REPLY.
    o=0; reply[o++]=MSG_KEX_ECDH_REPLY;
    o=put_bytes(reply,o,ksb,kl); o=put_bytes(reply,o,q_s,32); o=put_bytes(reply,o,sb,sl);
    ssh_send(s,reply,o);

    // Dérivation : serveur envoie avec 'D', reçoit avec 'C'.
    uint8_t Kmp[40]; int kml=enc_mpint(Kmp,K,32);
    derive_key(Kmp,kml,H,'D',s->session_id,s->key_c2s,64);   // notre envoi
    derive_key(Kmp,kml,H,'C',s->session_id,s->key_s2c,64);   // notre réception

    uint8_t nk=MSG_NEWKEYS; ssh_send(s,&nk,1);
    if (!ssh_recv(s,reply,&plen) || reply[0]!=MSG_NEWKEYS) { kprintf("[sshd] NEWKEYS echec\n"); return -1; }
    s->encrypted=true;
    kprintf("[sshd] transport chiffre etabli\n");
    return 0;
}

// --- Session serveur (auth + canal + exec) -----------------------------------
static void ssh_server_session(ssh_t *s) {
    if (ssh_server_handshake(s) != 0) { kprintf("[sshd] handshake echec\n"); return; }

    static uint8_t p[PKT_SZ]; int plen;     // hors pile (16 Kio seulement)
    // Service request.
    if (!ssh_recv_useful(s,p,&plen) || p[0]!=MSG_SERVICE_REQUEST) return;
    uint8_t acc[64]; int o=0; acc[o++]=MSG_SERVICE_ACCEPT; o=put_str(acc,o,"ssh-userauth");
    ssh_send(s,acc,o);

    // Authentification : mot de passe ET clé publique (ed25519).
    //  Méthodes annoncées sur échec : "publickey,password".
    #define MSG_USERAUTH_PK_OK 60
    char user[64] = "";
    bool authed = false;
    for (int tries=0; tries<8 && !authed; tries++) {
        if (!ssh_recv_useful(s,p,&plen) || p[0]!=MSG_USERAUTH_REQUEST) return;
        int q=1; uint32_t ul=rd32(p+q); q+=4; int un=ul<63?ul:63; memcpy(user,p+q,un); user[un]=0; q+=ul;
        uint32_t sl2=rd32(p+q); q+=4+sl2;                       // service
        uint32_t ml=rd32(p+q); q+=4; char method[32]; int mn=ml<31?ml:31; memcpy(method,p+q,mn); method[mn]=0; q+=ml;

        if (strcmp(method,"password")==0) {
            q+=1;                                               // bool FALSE
            uint32_t pl=rd32(p+q); q+=4; char pass[128]; int pn=pl<127?pl:127; memcpy(pass,p+q,pn); pass[pn]=0;
            if (users_authenticate(user, pass)) authed = true;
            kprintf("[sshd] mot de passe : %s\n", authed ? "OK" : "refuse");
        } else if (strcmp(method,"publickey")==0) {
            uint8_t have_sig = p[q]; q++;
            uint32_t al=rd32(p+q); q+=4; const uint8_t *algo=p+q; q+=al;       // nom d'algo
            uint32_t kl=rd32(p+q); q+=4; const uint8_t *blob=p+q; q+=kl;       // blob clé publique
            int signed_end = q;                                  // fin des données signées
            uint8_t key[32];
            bool ised = (al==11 && memcmp(algo,"ssh-ed25519",11)==0);
            bool keyok = ised && ssh_blob_to_key(blob, kl, key);

            if (!have_sig) {
                // Phase 1 (sondage) : la clé est-elle acceptable ?
                if (keyok && find_user(user) && ssh_key_authorized(user, key)) {
                    uint8_t r[128]; o=0; r[o++]=MSG_USERAUTH_PK_OK;
                    o=put_bytes(r,o,algo,al); o=put_bytes(r,o,blob,kl);
                    ssh_send(s,r,o);
                    continue;                                    // on attend la signature
                }
                // sinon -> FAILURE plus bas
            } else {
                // Phase 2 : vérification de la signature.
                uint32_t sgl=rd32(p+q); q+=4; const uint8_t *sigblob=p+q;      // string signature
                // sigblob = string("ssh-ed25519") + string(rawsig[64])
                uint8_t rawsig[64]; bool sigok=false;
                if (sgl>=4+11+4+64) {
                    uint32_t st=rd32(sigblob);
                    if (st==11 && memcmp(sigblob+4,"ssh-ed25519",11)==0) {
                        uint32_t rsl=rd32(sigblob+4+11);
                        if (rsl==64) { memcpy(rawsig, sigblob+4+11+4, 64); sigok=true; }
                    }
                }
                // Données signées = string(session_id) + paquet de requête (jusqu'à
                // la clé incluse, c.-à-d. p[0..signed_end)).
                static uint8_t signdata[2048];
                int so=0; wr32(signdata+so,32); so+=4; memcpy(signdata+so,s->session_id,32); so+=32;
                if (signed_end <= (int)sizeof(signdata)-so) {
                    memcpy(signdata+so, p, signed_end); so+=signed_end;
                    if (sigok && keyok && find_user(user) && ssh_key_authorized(user,key) &&
                        crypto_ed25519_check(rawsig, key, signdata, so)==0)
                        authed = true;
                }
                kprintf("[sshd] cle publique : %s\n", authed ? "OK" : "refuse");
            }
        }

        uint8_t r[64]; o=0;
        if (authed) { r[o++]=MSG_USERAUTH_SUCCESS; }
        else { r[o++]=MSG_USERAUTH_FAILURE; o=put_str(r,o,"publickey,password"); r[o++]=0; }
        ssh_send(s,r,o);
    }
    if (!authed) { kprintf("[sshd] auth refusee\n"); return; }
    kprintf("[sshd] %s authentifie\n", user);

    // Canal + requêtes.
    uint32_t client_ch = 0;
    for (int guard=0; guard<64; guard++) {
        if (!ssh_recv_useful(s,p,&plen)) return;
        uint8_t t = p[0];
        if (t == MSG_CHANNEL_OPEN) {
            // string "session", u32 sender, u32 window, u32 maxpkt
            uint32_t tl=rd32(p+1); int q=5+tl;
            client_ch = rd32(p+q);
            uint8_t r[64]; o=0; r[o++]=MSG_CHANNEL_OPEN_CONFIRMATION;
            wr32(r+o,client_ch); o+=4; wr32(r+o,0); o+=4;       // notre canal 0
            wr32(r+o,0x100000); o+=4; wr32(r+o,0x4000); o+=4;
            ssh_send(s,r,o);
        } else if (t == MSG_CHANNEL_REQUEST) {
            uint32_t rl=rd32(p+5); char rt[32]; int rn=rl<31?rl:31; memcpy(rt,p+9,rn); rt[rn]=0;
            int q=9+rl; uint8_t want_reply=p[q]; q++;
            if (strcmp(rt,"exec")==0) {
                uint32_t cl=rd32(p+q); q+=4; char command[512]; int cn=cl<511?cl:511; memcpy(command,p+q,cn); command[cn]=0;
                if (want_reply) { uint8_t r[16]; o=0; r[o++]=MSG_CHANNEL_SUCCESS; wr32(r+o,client_ch); o+=4; ssh_send(s,r,o); }
                static char obuf[8192];
                int n = shell_exec(user, command, obuf, sizeof(obuf));
                // CHANNEL_DATA
                static uint8_t d[8300]; o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)obuf,n);
                ssh_send(s,d,o);
                // exit-status
                o=0; d[o++]=MSG_CHANNEL_REQUEST; wr32(d+o,client_ch); o+=4; o=put_str(d,o,"exit-status"); d[o++]=0; wr32(d+o,0); o+=4;
                ssh_send(s,d,o);
                // EOF + CLOSE
                o=0; d[o++]=MSG_CHANNEL_EOF; wr32(d+o,client_ch); o+=4; ssh_send(s,d,o);
                o=0; d[o++]=MSG_CHANNEL_CLOSE; wr32(d+o,client_ch); o+=4; ssh_send(s,d,o);
                return;
            } else if (strcmp(rt,"shell")==0) {
                if (want_reply) { uint8_t r[16]; o=0; r[o++]=MSG_CHANNEL_SUCCESS; wr32(r+o,client_ch); o+=4; ssh_send(s,r,o); }
                // Shell interactif minimal : invite, lecture ligne, execution.
                const char *banner="sexOs shell distant. Tapez 'help'.\r\n";
                static uint8_t d[8300];
                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)banner,strlen(banner)); ssh_send(s,d,o);
                char linebuf[512]; int ll=0;
                const char *prompt="sexos$ ";
                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)prompt,strlen(prompt)); ssh_send(s,d,o);
                for (int g=0; g<100000; g++) {
                    if (!ssh_recv(s,p,&plen)) return;
                    if (p[0]==MSG_CHANNEL_DATA) {
                        uint32_t dl=rd32(p+5);
                        for (uint32_t k=0;k<dl;k++) {
                            char ch=p[9+k];
                            if (ch=='\r' || ch=='\n') {
                                linebuf[ll]=0;
                                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)"\r\n",2); ssh_send(s,d,o);
                                if (strcmp(linebuf,"exit")==0) {
                                    o=0; d[o++]=MSG_CHANNEL_CLOSE; wr32(d+o,client_ch); o+=4; ssh_send(s,d,o); return;
                                }
                                static char ob[8192]; int n=shell_exec(user,linebuf,ob,sizeof(ob));
                                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)ob,n); ssh_send(s,d,o);
                                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)prompt,strlen(prompt)); ssh_send(s,d,o);
                                ll=0;
                            } else if (ch==0x7f || ch==8) {     // backspace
                                if (ll>0){ ll--; o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; o=put_bytes(d,o,(uint8_t*)"\b \b",3); ssh_send(s,d,o); }
                            } else if (ll<511) {
                                linebuf[ll++]=ch;               // écho
                                o=0; d[o++]=MSG_CHANNEL_DATA; wr32(d+o,client_ch); o+=4; d[o++]=0;d[o++]=0;d[o++]=0;d[o++]=1; d[o++]=ch; ssh_send(s,d,o);
                            }
                        }
                    } else if (p[0]==MSG_CHANNEL_EOF || p[0]==MSG_CHANNEL_CLOSE) return;
                }
                return;
            } else {
                if (want_reply) { uint8_t r[16]; o=0; r[o++]=MSG_CHANNEL_SUCCESS; wr32(r+o,client_ch); o+=4; ssh_send(s,r,o); }
            }
        } else if (t == MSG_CHANNEL_CLOSE || t == MSG_DISCONNECT) {
            return;
        }
    }
}

// --- Tâche NOYAU sshd : accepte et sert les connexions (modèle non bloquant) --
//  Tourne en parallèle de la tâche réseau : elle pompe le NIC, sshd lit/écrit
//  via les sockets non bloquantes en cédant le CPU ('hlt') entre deux.
void sshd_run(void) {
    static ssh_t s;
    for (;;) {
        __asm__ volatile ("cli");
        int conn = server_ready ? tcp_accept_nb(22) : -1;
        __asm__ volatile ("sti");
        if (conn < 0) { __asm__ volatile ("hlt"); continue; }
        kprintf("[sshd] connexion entrante\n");
        memset(&s, 0, sizeof(s));
        s.conn = conn;
        ssh_server_session(&s);
        // Fermeture GRACIEUSE : on demande la fermeture (tcp_shutdown) au lieu de
        // tcp_close. tcp_tick videra d'abord le tampon d'émission (les données du
        // canal, exit-status, EOF, CLOSE) PUIS émettra le FIN. Un tcp_close brutal
        // enverrait le FIN immédiatement et jetterait les octets non encore émis.
        __asm__ volatile ("cli"); tcp_shutdown(conn); __asm__ volatile ("sti");
        kprintf("[sshd] session terminee\n");
    }
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
