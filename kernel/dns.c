// =============================================================================
//  kernel/dns.c -- Résolveur DNS (requêtes A sur UDP)
// -----------------------------------------------------------------------------
//  Deux interfaces :
//   - dns_resolve()  : BLOQUANTE (boot/shell, sonde le NIC) ;
//   - dns_query()    : NON BLOQUANTE, pilotée par la tâche réseau (dns_tick).
// =============================================================================
#include "net.h"
#include "klib.h"

// Construit une requête DNS « A » pour 'name' dans q[]. Renvoie la longueur.
static int dns_build(const char *name, uint8_t *q, uint16_t id) {
    int o = 0;
    q[o++]=id>>8; q[o++]=id&0xFF;
    q[o++]=0x01; q[o++]=0x00;            // flags : récursion désirée
    q[o++]=0; q[o++]=1;                  // QDCOUNT = 1
    q[o++]=0; q[o++]=0;                  // ANCOUNT
    q[o++]=0; q[o++]=0;                  // NSCOUNT
    q[o++]=0; q[o++]=0;                  // ARCOUNT
    int label = o++;
    int n = 0;
    for (const char *p = name; ; p++) {
        if (*p == '.' || *p == 0) {
            q[label] = n;
            if (*p == 0) { q[o++] = 0; break; }
            label = o++; n = 0;
        } else { q[o++] = *p; n++; }
    }
    q[o++]=0; q[o++]=1;                  // QTYPE = A
    q[o++]=0; q[o++]=1;                  // QCLASS = IN
    return o;
}

// Extrait la première adresse A d'une réponse DNS. Renvoie true si trouvée.
static bool dns_parse(const uint8_t *resp, int rlen, ip4_t *out) {
    if (rlen < 12) return false;
    int ancount = (resp[6] << 8) | resp[7];
    if (ancount < 1) return false;
    int p = 12;
    while (p < rlen && resp[p]) {                 // saute le QNAME
        if ((resp[p] & 0xC0) == 0xC0) { p += 2; goto after_q; }
        p += resp[p] + 1;
    }
    p++;
after_q:
    p += 4;                                       // QTYPE + QCLASS
    for (int a = 0; a < ancount && p + 12 <= rlen; a++) {
        if ((resp[p] & 0xC0) == 0xC0) p += 2;     // nom compressé
        else { while (p < rlen && resp[p]) p += resp[p] + 1; p++; }
        uint16_t type = (resp[p] << 8) | resp[p+1];
        uint16_t rdlen = (resp[p+8] << 8) | resp[p+9];
        int rdata = p + 10;
        if (type == 1 && rdlen == 4) {            // enregistrement A
            *out = (resp[rdata]<<24)|(resp[rdata+1]<<16)|(resp[rdata+2]<<8)|resp[rdata+3];
            return true;
        }
        p = rdata + rdlen;
    }
    return false;
}

// --- DNS bloquant (boot/shell) -----------------------------------------------
bool dns_resolve(const char *name, ip4_t *out) {
    if (!netif.dns) return false;
    uint8_t q[300];
    int o = dns_build(name, q, 0x1234);
    uint16_t sport = 0xC000 | (0x1234 & 0xFFF);
    udp_listen(sport);
    udp_send(netif.dns, sport, 53, q, o);
    uint8_t *resp; int rlen; ip4_t src;
    if (!udp_wait(3000, &src, &resp, &rlen)) return false;
    return dns_parse(resp, rlen, out);
}

// --- DNS non bloquant (tâche réseau) -----------------------------------------
static struct {
    bool     active, done, ok;
    char     name[128];
    uint8_t  q[300];
    int      qlen;
    uint16_t sport;
    ip4_t    ip;
    uint64_t deadline;
    int      attempts;
} dq;

extern uint64_t pit_ms(void);

static void dns_start(const char *name) {
    int i = 0; for (; name[i] && i < (int)sizeof(dq.name) - 1; i++) dq.name[i] = name[i];
    dq.name[i] = 0;
    dq.active = true; dq.done = false; dq.ok = false; dq.ip = 0;
    dq.attempts = 0; dq.deadline = 0;             // émission au prochain tick
    if (!netif.dns) { dq.done = true; dq.ok = false; return; }
    dq.qlen = dns_build(dq.name, dq.q, 0x1234);
    dq.sport = 0xC000 | (0x1234 & 0xFFF);
    udp_listen(dq.sport);
}

void dns_tick(void) {
    if (!dq.active || dq.done) return;
    ip4_t src; uint8_t *resp; int rlen;
    if (udp_take(dq.sport, &src, &resp, &rlen)) {
        if (dns_parse(resp, rlen, &dq.ip)) { dq.done = true; dq.ok = true; return; }
    }
    if (pit_ms() >= dq.deadline) {
        if (dq.attempts >= 4) { dq.done = true; dq.ok = false; return; }
        udp_send(netif.dns, dq.sport, 53, dq.q, dq.qlen);
        dq.deadline = pit_ms() + 1000;
        dq.attempts++;
    }
}

int dns_query(const char *name, ip4_t *out) {
    if (!dq.active || strcmp(dq.name, name) != 0) dns_start(name);
    if (!dq.done) return 0;                        // en cours
    if (dq.ok) { *out = dq.ip; return 1; }
    return -1;                                     // échec / timeout
}
