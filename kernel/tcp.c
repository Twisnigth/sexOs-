// =============================================================================
//  kernel/tcp.c -- TCP (client) : handshake, données, ACK, retransmission, FIN
// -----------------------------------------------------------------------------
//  Implémentation pragmatique mais correcte pour les usages client (HTTP, SSH).
//  Plusieurs connexions simultanées possibles (table). Pas de réassemblage des
//  segments hors-ordre (on s'appuie sur l'ordre, suffisant via SLIRP/QEMU).
// =============================================================================
#include "net.h"
#include "klib.h"
#include "pit.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum { ST_CLOSED, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTABLISHED, ST_CLOSE_WAIT };

#define MAX_CONN 8
#define MAX_LISTEN 4
#define RXBUF 16384

typedef struct {
    bool     used;
    int      state;
    ip4_t    rip;
    uint16_t lport, rport;
    uint32_t snd_nxt, snd_una, rcv_nxt;
    uint8_t  rx[RXBUF];
    int      rxlen;
    bool     fin;
    bool     passive;        // connexion issue d'un listen
    bool     accepted;       // déjà remise à l'application
} conn_t;

static conn_t conns[MAX_CONN];
static uint16_t listen_ports[MAX_LISTEN];
static uint16_t next_port = 49152;
static uint32_t isn_counter = 0x12345678;

static bool is_listening(uint16_t port) {
    for (int i = 0; i < MAX_LISTEN; i++) if (listen_ports[i] == port) return true;
    return false;
}

static conn_t *find_conn(ip4_t rip, uint16_t rport, uint16_t lport) {
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used && conns[i].rip == rip &&
            conns[i].rport == rport && conns[i].lport == lport) return &conns[i];
    return NULL;
}

// Somme de contrôle TCP avec pseudo-en-tête IPv4.
static uint16_t tcp_checksum(ip4_t src, ip4_t dst, const uint8_t *seg, int len) {
    uint8_t tmp[1600];
    int o = 0;
    tmp[o++]=src>>24; tmp[o++]=src>>16; tmp[o++]=src>>8; tmp[o++]=src;
    tmp[o++]=dst>>24; tmp[o++]=dst>>16; tmp[o++]=dst>>8; tmp[o++]=dst;
    tmp[o++]=0; tmp[o++]=IP_TCP; tmp[o++]=len>>8; tmp[o++]=len&0xFF;
    memcpy(tmp + o, seg, len); o += len;
    return net_checksum(tmp, o);
}

static void tcp_out(conn_t *c, uint8_t flags, const uint8_t *data, int dlen) {
    uint8_t seg[1600];
    seg[0]=c->lport>>8; seg[1]=c->lport&0xFF;
    seg[2]=c->rport>>8; seg[3]=c->rport&0xFF;
    seg[4]=c->snd_nxt>>24; seg[5]=c->snd_nxt>>16; seg[6]=c->snd_nxt>>8; seg[7]=c->snd_nxt;
    seg[8]=c->rcv_nxt>>24; seg[9]=c->rcv_nxt>>16; seg[10]=c->rcv_nxt>>8; seg[11]=c->rcv_nxt;
    seg[12]=0x50;                     // data offset 5 (20 octets), pas d'options
    seg[13]=flags;
    seg[14]=0x20; seg[15]=0x00;       // window 8192
    seg[16]=0; seg[17]=0;             // checksum
    seg[18]=0; seg[19]=0;             // urgent ptr
    if (dlen) memcpy(seg + 20, data, dlen);
    int total = 20 + dlen;
    uint16_t cs = tcp_checksum(netif.ip, c->rip, seg, total);
    seg[16]=cs>>8; seg[17]=cs&0xFF;
    ipv4_send(c->rip, IP_TCP, seg, total);
}

void tcp_rx(ip4_t src, const uint8_t *d, uint16_t len) {
    if (len < 20) return;
    uint16_t sport=(d[0]<<8)|d[1], dport=(d[2]<<8)|d[3];
    uint32_t seq=(d[4]<<24)|(d[5]<<16)|(d[6]<<8)|d[7];
    uint32_t ack=(d[8]<<24)|(d[9]<<16)|(d[10]<<8)|d[11];
    int doff=((d[12]>>4)&0xF)*4;
    uint8_t flags=d[13];
    conn_t *c = find_conn(src, sport, dport);

    // Ouverture passive : SYN vers un port en écoute -> SYN-ACK.
    if (!c && (flags & TCP_SYN) && !(flags & TCP_ACK) && is_listening(dport)) {
        int idx = -1;
        for (int i = 0; i < MAX_CONN; i++) if (!conns[i].used) { idx = i; break; }
        if (idx < 0) return;
        c = &conns[idx];
        memset(c, 0, sizeof(*c));
        c->used = true; c->state = ST_SYN_RCVD; c->passive = true;
        c->rip = src; c->rport = sport; c->lport = dport;
        c->rcv_nxt = seq + 1;
        c->snd_nxt = isn_counter += 0x1000;
        c->snd_una = c->snd_nxt;
        tcp_out(c, TCP_SYN | TCP_ACK, NULL, 0);
        c->snd_nxt++;                       // le SYN consomme un numéro de séquence
        return;
    }
    if (!c) return;

    if (flags & TCP_RST) { c->state = ST_CLOSED; c->fin = true; return; }

    if (c->state == ST_SYN_RCVD && (flags & TCP_ACK)) {
        c->snd_una = ack;
        c->state = ST_ESTABLISHED;
        // (les données éventuelles de ce segment sont traitées ci-dessous)
    }

    if (c->state == ST_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_nxt = ack;             // notre SYN consommé
        c->state = ST_ESTABLISHED;
        tcp_out(c, TCP_ACK, NULL, 0);
        return;
    }

    if (c->state == ST_ESTABLISHED || c->state == ST_CLOSE_WAIT) {
        if (flags & TCP_ACK) c->snd_una = ack;
        int plen = len - doff;
        if (plen > 0 && seq == c->rcv_nxt) {
            if (c->rxlen + plen > RXBUF) plen = RXBUF - c->rxlen;
            if (plen > 0) { memcpy(c->rx + c->rxlen, d + doff, plen); c->rxlen += plen; c->rcv_nxt += plen; }
            tcp_out(c, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            c->rcv_nxt++;
            c->fin = true;
            c->state = ST_CLOSE_WAIT;
            tcp_out(c, TCP_ACK, NULL, 0);
        }
    }
}

void tcp_listen_port(uint16_t port) {
    for (int i = 0; i < MAX_LISTEN; i++)
        if (listen_ports[i] == 0 || listen_ports[i] == port) { listen_ports[i] = port; return; }
}

// Attend une nouvelle connexion entrante sur 'port'. Renvoie son id (-1 sinon).
int tcp_accept(uint16_t port, uint32_t timeout_ms) {
    uint64_t end = pit_ms() + timeout_ms;
    for (;;) {
        nic_poll();
        for (int i = 0; i < MAX_CONN; i++)
            if (conns[i].used && conns[i].passive && !conns[i].accepted &&
                conns[i].lport == port && conns[i].state == ST_ESTABLISHED) {
                conns[i].accepted = true;
                return i;
            }
        if (pit_ms() >= end) return -1;
    }
}

int tcp_connect(ip4_t dst, uint16_t dport) {
    int idx = -1;
    for (int i = 0; i < MAX_CONN; i++) if (!conns[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    conn_t *c = &conns[idx];
    memset(c, 0, sizeof(*c));
    c->used = true; c->state = ST_SYN_SENT;
    c->rip = dst; c->rport = dport; c->lport = next_port++;
    c->snd_nxt = isn_counter += 0x1000;
    c->snd_una = c->snd_nxt;

    for (int attempt = 0; attempt < 5; attempt++) {
        tcp_out(c, TCP_SYN, NULL, 0);
        uint64_t end = pit_ms() + 500;
        while (pit_ms() < end) {
            nic_poll();
            if (c->state == ST_ESTABLISHED) return idx;
        }
    }
    c->used = false;
    return -1;
}

int tcp_send(int conn, const void *data, int len) {
    if (conn < 0 || conn >= MAX_CONN || !conns[conn].used) return -1;
    conn_t *c = &conns[conn];
    if (c->state != ST_ESTABLISHED) return -1;
    uint32_t target = c->snd_nxt + len;            // ACK attendu après ces données
    for (int attempt = 0; attempt < 5; attempt++) {
        tcp_out(c, TCP_PSH | TCP_ACK, data, len);  // seq = snd_nxt (inchangé)
        uint64_t end = pit_ms() + 500;
        while (pit_ms() < end) {
            nic_poll();
            if ((int32_t)(c->snd_una - target) >= 0) { c->snd_nxt = target; return len; }
        }
    }
    c->snd_nxt = target;                            // best effort
    return len;
}

int tcp_recv(int conn, void *buf, int len, uint32_t timeout_ms) {
    if (conn < 0 || conn >= MAX_CONN || !conns[conn].used) return -1;
    conn_t *c = &conns[conn];
    uint64_t end = pit_ms() + timeout_ms;
    while (pit_ms() < end) {
        nic_poll();
        if (c->rxlen > 0) {
            int n = c->rxlen < len ? c->rxlen : len;
            memcpy(buf, c->rx, n);
            memmove(c->rx, c->rx + n, c->rxlen - n);
            c->rxlen -= n;
            return n;
        }
        if (c->fin) return 0;          // connexion fermée par le pair
    }
    return 0;
}

void tcp_close(int conn) {
    if (conn < 0 || conn >= MAX_CONN || !conns[conn].used) return;
    conn_t *c = &conns[conn];
    if (c->state == ST_ESTABLISHED || c->state == ST_CLOSE_WAIT) {
        tcp_out(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
    }
    c->used = false;
}

// --- HTTP GET (hôte:port/chemin) : renvoie le CORPS, taille dans *body_len ---
//  'ip_or_dns' : si parse_ip échoue, on résout par DNS.
int http_download(ip4_t ip, uint16_t port, const char *host, const char *path,
                  char *buf, int max) {
    int conn = tcp_connect(ip, port);
    if (conn < 0) return -1;

    char req[640]; int o = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int i = 0; i < 5; i++) for (const char *p = parts[i]; *p; p++) req[o++] = *p;
    tcp_send(conn, req, o);

    static char raw[262144];
    int total = 0;
    while (total < (int)sizeof(raw) - 1) {
        int n = tcp_recv(conn, raw + total, sizeof(raw) - 1 - total, 5000);
        if (n <= 0) break;
        total += n;
    }
    raw[total] = 0;
    tcp_close(conn);

    // Sépare l'en-tête du corps (\r\n\r\n).
    int body = 0;
    for (int i = 0; i + 3 < total; i++)
        if (raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n') { body = i + 4; break; }
    int blen = total - body;
    if (blen < 0) blen = 0;
    if (blen > max) blen = max;
    memcpy(buf, raw + body, blen);
    return blen;
}

// --- HTTP GET simple (port 80, réponse complète) -----------------------------
int http_get(const char *host, const char *path, char *buf, int len) {
    ip4_t ip;
    if (!dns_resolve(host, &ip)) return -1;
    int conn = tcp_connect(ip, 80);
    if (conn < 0) return -1;

    char req[512];
    int o = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int i = 0; i < 5; i++) for (const char *p = parts[i]; *p; p++) req[o++] = *p;
    tcp_send(conn, req, o);

    int total = 0;
    while (total < len - 1) {
        int n = tcp_recv(conn, buf + total, len - 1 - total, 4000);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;
    tcp_close(conn);
    return total;
}
