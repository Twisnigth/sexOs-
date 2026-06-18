// =============================================================================
//  kernel/dns.c -- Résolveur DNS (requêtes A sur UDP)
// =============================================================================
#include "net.h"
#include "klib.h"

bool dns_resolve(const char *name, ip4_t *out) {
    if (!netif.dns) return false;

    uint8_t q[300];
    int o = 0;
    uint16_t id = 0x1234;
    q[o++]=id>>8; q[o++]=id&0xFF;
    q[o++]=0x01; q[o++]=0x00;            // flags : récursion désirée
    q[o++]=0; q[o++]=1;                  // QDCOUNT = 1
    q[o++]=0; q[o++]=0;                  // ANCOUNT
    q[o++]=0; q[o++]=0;                  // NSCOUNT
    q[o++]=0; q[o++]=0;                  // ARCOUNT

    // QNAME : suite de labels longueur+texte, terminé par 0.
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

    uint16_t sport = 0xC000 | (id & 0xFFF);
    udp_listen(sport);
    udp_send(netif.dns, sport, 53, q, o);

    uint8_t *resp; int rlen; ip4_t src;
    if (!udp_wait(3000, &src, &resp, &rlen)) return false;
    if (rlen < 12) return false;

    int ancount = (resp[6] << 8) | resp[7];
    if (ancount < 1) return false;

    // Saute l'en-tête (12) puis la question (QNAME + 4).
    int p = 12;
    while (p < rlen && resp[p]) {
        if ((resp[p] & 0xC0) == 0xC0) { p += 2; goto after_q; }
        p += resp[p] + 1;
    }
    p++;                                  // octet 0 final du QNAME
after_q:
    p += 4;                               // QTYPE + QCLASS

    // Parcourt les réponses.
    for (int a = 0; a < ancount && p + 12 <= rlen; a++) {
        if ((resp[p] & 0xC0) == 0xC0) p += 2;       // nom compressé
        else { while (p < rlen && resp[p]) p += resp[p] + 1; p++; }
        uint16_t type = (resp[p] << 8) | resp[p+1];
        uint16_t rdlen = (resp[p+8] << 8) | resp[p+9];
        int rdata = p + 10;
        if (type == 1 && rdlen == 4) {              // enregistrement A
            *out = (resp[rdata]<<24)|(resp[rdata+1]<<16)|(resp[rdata+2]<<8)|resp[rdata+3];
            return true;
        }
        p = rdata + rdlen;
    }
    return false;
}
