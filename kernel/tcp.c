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
#define RXBUF 65536          // tampon de réception : plus grand = fenêtre plus large
#define TXBUF 8192
#define TCP_MSS 1400

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
    // --- pilotage NON BLOQUANT (géré par tcp_tick dans la tâche réseau) -------
    bool     nb;             // ouverture non bloquante (sockets ring 3)
    bool     reset;          // RST reçu / erreur fatale
    bool     want_close;     // l'application a demandé la fermeture
    bool     fin_sent;       // notre FIN a été émis
    uint8_t  tx[TXBUF];      // données en attente d'ACK (commencent à snd_una)
    int      txlen;
    uint64_t rexmit_at;      // prochaine échéance de (ré)émission
    int      attempts;       // tentatives consécutives
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

// Émet un segment avec un numéro de séquence explicite (pour la retransmission
// depuis snd_una). La fenêtre annoncée reflète la place libre du tampon RX.
static void tcp_send_seg(conn_t *c, uint8_t flags, uint32_t seq, const uint8_t *data, int dlen) {
    uint8_t seg[1600];
    seg[0]=c->lport>>8; seg[1]=c->lport&0xFF;
    seg[2]=c->rport>>8; seg[3]=c->rport&0xFF;
    seg[4]=seq>>24; seg[5]=seq>>16; seg[6]=seq>>8; seg[7]=seq;
    seg[8]=c->rcv_nxt>>24; seg[9]=c->rcv_nxt>>16; seg[10]=c->rcv_nxt>>8; seg[11]=c->rcv_nxt;
    seg[12]=0x50;                     // data offset 5 (20 octets), pas d'options
    seg[13]=flags;
    int win = RXBUF - c->rxlen; if (win < 0) win = 0; if (win > 65535) win = 65535;
    seg[14]=win>>8; seg[15]=win&0xFF; // fenêtre de réception réelle (flow control)
    seg[16]=0; seg[17]=0;             // checksum
    seg[18]=0; seg[19]=0;             // urgent ptr
    if (dlen) memcpy(seg + 20, data, dlen);
    int total = 20 + dlen;
    uint16_t cs = tcp_checksum(netif.ip, c->rip, seg, total);
    seg[16]=cs>>8; seg[17]=cs&0xFF;
    ipv4_send(c->rip, IP_TCP, seg, total);
}

static void tcp_out(conn_t *c, uint8_t flags, const uint8_t *data, int dlen) {
    tcp_send_seg(c, flags, c->snd_nxt, data, dlen);
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
        // nb = true : une fois acceptée, la connexion est servie en NON BLOQUANT
        // par sshd (tcp_read/tcp_write) ; c'est tcp_tick() qui émet réellement les
        // données bufferisées, et il n'agit QUE sur les connexions 'nb'.
        c->used = true; c->nb = true; c->state = ST_SYN_RCVD; c->passive = true;
        c->rip = src; c->rport = sport; c->lport = dport;
        c->rcv_nxt = seq + 1;
        c->snd_nxt = isn_counter += 0x1000;
        c->snd_una = c->snd_nxt;
        tcp_out(c, TCP_SYN | TCP_ACK, NULL, 0);
        c->snd_nxt++;                       // le SYN consomme un numéro de séquence
        c->rexmit_at = pit_ms() + 300; c->attempts = 0;  // réémission du SYN-ACK si perdu
        return;
    }
    if (!c) return;

    if (flags & TCP_RST) { c->state = ST_CLOSED; c->fin = true; c->reset = true; return; }

    if (c->state == ST_SYN_RCVD && (flags & TCP_ACK)) {
        c->snd_una = ack;
        c->state = ST_ESTABLISHED;
        c->attempts = 0; c->rexmit_at = 0;
        // (les données éventuelles de ce segment sont traitées ci-dessous)
    }

    if (c->state == ST_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_nxt = ack;             // notre SYN consommé
        c->state = ST_ESTABLISHED;
        c->attempts = 0; c->rexmit_at = 0;   // (nb) émettre les données au plus tôt
        tcp_out(c, TCP_ACK, NULL, 0);
        return;
    }

    if (c->state == ST_ESTABLISHED || c->state == ST_CLOSE_WAIT) {
        if (flags & TCP_ACK) {
            uint32_t acked = ack - c->snd_una;
            if ((int32_t)acked > 0) {
                c->snd_una = ack;
                // (nb) libère du tampon d'émission les octets acquittés.
                if (c->nb && c->txlen > 0) {
                    int n = (int)acked; if (n > c->txlen) n = c->txlen;
                    memmove(c->tx, c->tx + n, c->txlen - n);
                    c->txlen -= n;
                    c->attempts = 0; c->rexmit_at = pit_ms() + 600;
                }
            }
        }
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

// Accept NON BLOQUANT (pour la tâche sshd) : ne touche pas le NIC. À appeler
// avec les interruptions masquées (exclusion mutuelle avec la tâche réseau).
int tcp_accept_nb(uint16_t port) {
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].used && conns[i].passive && !conns[i].accepted &&
            conns[i].lport == port && conns[i].state == ST_ESTABLISHED) {
            conns[i].accepted = true;
            return i;
        }
    return -1;
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

// =============================================================================
//  API SOCKETS NON BLOQUANTE (appelée depuis les syscalls, IF=0)
// -----------------------------------------------------------------------------
//  Ces fonctions ne touchent JAMAIS le NIC ni le minuteur : elles ne font que
//  manipuler l'état/les tampons des connexions. Toute l'émission est faite par
//  tcp_tick(), exécutée dans la tâche réseau. Les deux s'excluent (IF=0).
// =============================================================================
int tcp_open(ip4_t dst, uint16_t dport) {
    int idx = -1;
    for (int i = 0; i < MAX_CONN; i++) if (!conns[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    conn_t *c = &conns[idx];
    memset(c, 0, sizeof(*c));
    c->used = true; c->nb = true; c->state = ST_SYN_SENT;
    c->rip = dst; c->rport = dport; c->lport = next_port++;
    c->snd_nxt = isn_counter += 0x1000;
    c->snd_una = c->snd_nxt;
    c->rexmit_at = 0; c->attempts = 0;   // SYN émis au prochain tick
    return idx;
}

int tcp_state(int id) {
    if (id < 0 || id >= MAX_CONN || !conns[id].used) return -1;
    conn_t *c = &conns[id];
    if (c->reset) return -1;
    if (c->state == ST_SYN_SENT || c->state == ST_SYN_RCVD) return 0;
    if (c->state == ST_ESTABLISHED) return 1;
    return 2;                            // CLOSE_WAIT / CLOSED : le pair a fermé
}

int tcp_write(int id, const void *data, int len) {
    if (id < 0 || id >= MAX_CONN || !conns[id].used) return -1;
    conn_t *c = &conns[id];
    if (c->reset) return -1;
    if (c->state != ST_ESTABLISHED) return 0;     // pas encore prêt -> réessayer
    int space = TXBUF - c->txlen;
    if (len > space) len = space;
    if (len <= 0) return 0;
    memcpy(c->tx + c->txlen, data, len);
    c->txlen += len;
    c->rexmit_at = 0;                              // émettre au prochain tick
    return len;
}

int tcp_read(int id, void *buf, int len) {
    if (id < 0 || id >= MAX_CONN || !conns[id].used) return -1;
    conn_t *c = &conns[id];
    if (c->rxlen > 0) {
        int win_before = RXBUF - c->rxlen;
        int n = c->rxlen < len ? c->rxlen : len;
        memcpy(buf, c->rx, n);
        memmove(c->rx, c->rx + n, c->rxlen - n);
        c->rxlen -= n;
        // Mise à jour de fenêtre : en vidant le tampon, la fenêtre de réception se
        // rouvre. Sans ACK proactif, le pair (qui s'était arrêté sur fenêtre
        // pleine) attendrait sa sonde de fenêtre nulle (plusieurs secondes) avant
        // de reprendre -> longs blocages. On le prévient dès qu'on libère de la
        // place (au moins ~2 segments) et que la connexion est établie.
        int win_after = RXBUF - c->rxlen;
        if (c->state == ST_ESTABLISHED && win_after - win_before >= 2 * TCP_MSS)
            tcp_out(c, TCP_ACK, NULL, 0);
        return n;
    }
    if (c->reset) return -1;
    if (c->fin) return -1;               // pair fermé et tampon vidé -> fin de flux
    return 0;                            // rien pour l'instant
}

void tcp_shutdown(int id) {
    if (id < 0 || id >= MAX_CONN || !conns[id].used) return;
    conns[id].want_close = true;
    conns[id].rexmit_at = 0;
}

// Retransmissions / timeouts / émission différée. Appelée par la tâche réseau.
void tcp_tick(void) {
    uint64_t now = pit_ms();
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = &conns[i];
        if (!c->used || !c->nb) continue;

        if (c->state == ST_SYN_RCVD) {
            // Ouverture passive : réémet le SYN-ACK tant que l'ACK final tarde.
            if (now >= c->rexmit_at) {
                if (c->attempts >= 7) { c->reset = true; c->state = ST_CLOSED; continue; }
                tcp_send_seg(c, TCP_SYN | TCP_ACK, c->snd_una, NULL, 0);
                c->rexmit_at = now + 300; c->attempts++;
            }
            continue;
        }

        if (c->state == ST_SYN_SENT) {
            if (now >= c->rexmit_at) {
                if (c->attempts >= 7) { c->reset = true; c->state = ST_CLOSED; continue; }
                tcp_send_seg(c, TCP_SYN, c->snd_una, NULL, 0);
                // Le 1er SYN est souvent perdu le temps de résoudre l'ARP de la
                // passerelle (résolu en ~ms). On réémet vite les 2 premières fois
                // (150 ms) au lieu d'attendre 600 ms, puis on revient à 600 ms.
                c->rexmit_at = now + (c->attempts < 2 ? 150 : 600); c->attempts++;
            }
            continue;
        }

        if (c->state == ST_ESTABLISHED) {
            if (!c->fin_sent) c->snd_nxt = c->snd_una + c->txlen;
            if (c->txlen > 0) {
                if (now >= c->rexmit_at) {
                    if (c->attempts >= 10) { c->reset = true; continue; }
                    int off = 0;
                    while (off < c->txlen) {
                        int s = c->txlen - off; if (s > TCP_MSS) s = TCP_MSS;
                        tcp_send_seg(c, TCP_PSH | TCP_ACK, c->snd_una + off, c->tx + off, s);
                        off += s;
                    }
                    c->rexmit_at = now + 600; c->attempts++;
                }
            } else if (c->want_close && !c->fin_sent) {
                tcp_send_seg(c, TCP_FIN | TCP_ACK, c->snd_nxt, NULL, 0);
                c->fin_sent = true; c->used = false;   // fermeture côté client
            }
        } else if (c->state == ST_CLOSE_WAIT) {
            if (c->want_close) {
                if (!c->fin_sent) { tcp_send_seg(c, TCP_FIN | TCP_ACK, c->snd_nxt, NULL, 0); c->fin_sent = true; }
                c->used = false;
            }
        }
    }
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
