// =============================================================================
//  kernel/dhcp.c -- Client DHCP (obtention IP/masque/passerelle/DNS)
// =============================================================================
#include "net.h"
#include "klib.h"
#include "pit.h"

#define DHCP_MAGIC 0x63825363

static uint32_t xid;

// Construit l'en-tête BOOTP commun dans 'p' (>= 240 octets).
static int build_bootp(uint8_t *p, uint8_t msg_type) {
    memset(p, 0, 300);
    p[0] = 1;                       // op = BOOTREQUEST
    p[1] = 1; p[2] = 6;             // htype Ethernet, hlen 6
    p[4]=xid>>24; p[5]=xid>>16; p[6]=xid>>8; p[7]=xid;
    p[10] = 0x80;                   // flags : broadcast
    memcpy(p + 28, netif.mac.b, 6); // chaddr
    p[236]=DHCP_MAGIC>>24; p[237]=DHCP_MAGIC>>16; p[238]=DHCP_MAGIC>>8; p[239]=DHCP_MAGIC;
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = msg_type;            // type de message
    return o;
}

// Recherche une option DHCP, copie sa valeur, renvoie sa longueur (0 si absente).
static int dhcp_option(const uint8_t *p, int len, uint8_t code, uint8_t *out) {
    int o = 240;
    while (o < len && p[o] != 255) {
        if (p[o] == 0) { o++; continue; }
        uint8_t c = p[o], l = p[o+1];
        if (c == code) { memcpy(out, p + o + 2, l); return l; }
        o += 2 + l;
    }
    return 0;
}

static ip4_t get_ip(const uint8_t *b) { return (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]; }

bool net_dhcp(void) {
    if (!nic_present()) return false;
    xid = (uint32_t)pit_ms() ^ 0x4d4f4e4f;

    uint8_t pkt[300];
    uint8_t opt[64];

    // --- DISCOVER ------------------------------------------------------------
    udp_listen(68);
    int o = build_bootp(pkt, 1);
    pkt[o++] = 55; pkt[o++] = 3; pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;  // demande masque/routeur/DNS
    pkt[o++] = 255;
    udp_send(0xFFFFFFFF, 68, 67, pkt, o);

    uint8_t *resp; int rlen; ip4_t src;
    if (!udp_wait(2000, &src, &resp, &rlen)) { kprintf("[dhcp] pas d'OFFER\n"); return false; }
    ip4_t offered = get_ip(resp + 16);     // yiaddr
    ip4_t server = 0;
    if (dhcp_option(resp, rlen, 54, opt)) server = get_ip(opt);

    // --- REQUEST -------------------------------------------------------------
    udp_listen(68);
    o = build_bootp(pkt, 3);
    pkt[o++] = 50; pkt[o++] = 4;                          // adresse demandée
    pkt[o++]=offered>>24; pkt[o++]=offered>>16; pkt[o++]=offered>>8; pkt[o++]=offered;
    if (server) {
        pkt[o++] = 54; pkt[o++] = 4;
        pkt[o++]=server>>24; pkt[o++]=server>>16; pkt[o++]=server>>8; pkt[o++]=server;
    }
    pkt[o++] = 55; pkt[o++] = 3; pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;
    pkt[o++] = 255;
    udp_send(0xFFFFFFFF, 68, 67, pkt, o);

    if (!udp_wait(2000, &src, &resp, &rlen)) { kprintf("[dhcp] pas d'ACK\n"); return false; }

    netif.ip = get_ip(resp + 16);
    if (dhcp_option(resp, rlen, 1, opt)) netif.mask = get_ip(opt);
    if (dhcp_option(resp, rlen, 3, opt)) netif.gateway = get_ip(opt);
    if (dhcp_option(resp, rlen, 6, opt)) netif.dns = get_ip(opt);

    char a[16], b[16], c[16], d[16];
    ip_to_str(netif.ip, a); ip_to_str(netif.mask, b);
    ip_to_str(netif.gateway, c); ip_to_str(netif.dns, d);
    kprintf("[dhcp] IP %s masque %s passerelle %s DNS %s\n", a, b, c, d);
    return netif.ip != 0;
}
