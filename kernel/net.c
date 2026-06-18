// =============================================================================
//  kernel/net.c -- Cœur de la pile : Ethernet, ARP, dispatch, utilitaires
// =============================================================================
#include "net.h"
#include "klib.h"
#include "pit.h"

netif_t netif;
extern bool e1000_init(void);

// --- Initialisation ----------------------------------------------------------
void net_init(void) {
    memset(&netif, 0, sizeof(netif));
    if (!e1000_init()) return;
    netif.mac = nic_mac();
    netif.up = true;
    kprintf("[net] pile reseau prete\n");
}

void net_poll_ms(uint32_t ms) {
    uint64_t end = pit_ms() + ms;
    do { nic_poll(); } while (pit_ms() < end);
}

// --- Somme de contrôle Internet ----------------------------------------------
uint16_t net_checksum(const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void ip_to_str(ip4_t ip, char *buf) {
    char n[8]; int p = 0;
    for (int i = 3; i >= 0; i--) {
        utoa((ip >> (i * 8)) & 0xFF, n, 10);
        for (int k = 0; n[k]; k++) buf[p++] = n[k];
        if (i) buf[p++] = '.';
    }
    buf[p] = 0;
}

// --- Ethernet ----------------------------------------------------------------
static const mac_t MAC_BCAST = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};

void eth_send(mac_t dst, uint16_t ethertype, const void *payload, uint16_t len) {
    uint8_t frame[1600];
    memcpy(frame, dst.b, 6);
    memcpy(frame + 6, netif.mac.b, 6);
    frame[12] = ethertype >> 8;
    frame[13] = ethertype & 0xFF;
    if (len > 1500) len = 1500;
    memcpy(frame + 14, payload, len);
    uint16_t total = 14 + len;
    if (total < 60) { memset(frame + total, 0, 60 - total); total = 60; }  // padding minimal
    nic_send(frame, total);
}

void net_rx(const uint8_t *data, uint16_t len) {
    if (len < 14) return;
    uint16_t ethertype = (data[12] << 8) | data[13];
    if (ethertype == ETH_ARP)       arp_rx(data + 14, len - 14);
    else if (ethertype == ETH_IPV4) ipv4_rx(data + 14, len - 14);
}

// --- ARP ---------------------------------------------------------------------
#define ARP_CACHE 16
static struct { ip4_t ip; mac_t mac; bool valid; } arp_cache[ARP_CACHE];

static void arp_set(int i, ip4_t ip, mac_t mac) {
    arp_cache[i].ip = ip; arp_cache[i].mac = mac; arp_cache[i].valid = true;
}
static void arp_cache_put(ip4_t ip, mac_t mac) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { arp_cache[i].mac = mac; return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!arp_cache[i].valid) { arp_set(i, ip, mac); return; }
    arp_set(0, ip, mac);
}
static bool arp_cache_get(ip4_t ip, mac_t *out) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { *out = arp_cache[i].mac; return true; }
    return false;
}

static void arp_send(uint16_t oper, mac_t target_mac, ip4_t target_ip) {
    uint8_t p[28];
    p[0]=0; p[1]=1;                 // htype Ethernet
    p[2]=0x08; p[3]=0x00;           // ptype IPv4
    p[4]=6; p[5]=4;                 // hlen, plen
    p[6]=oper>>8; p[7]=oper&0xFF;
    memcpy(p+8, netif.mac.b, 6);    // sender mac
    p[14]=netif.ip>>24; p[15]=netif.ip>>16; p[16]=netif.ip>>8; p[17]=netif.ip;
    memcpy(p+18, target_mac.b, 6);  // target mac
    p[24]=target_ip>>24; p[25]=target_ip>>16; p[26]=target_ip>>8; p[27]=target_ip;
    eth_send(oper == 1 ? MAC_BCAST : target_mac, ETH_ARP, p, 28);
}

void arp_rx(const uint8_t *d, uint16_t len) {
    if (len < 28) return;
    uint16_t oper = (d[6] << 8) | d[7];
    ip4_t spa = (d[14]<<24)|(d[15]<<16)|(d[16]<<8)|d[17];
    ip4_t tpa = (d[24]<<24)|(d[25]<<16)|(d[26]<<8)|d[27];
    mac_t sha; memcpy(sha.b, d+8, 6);
    arp_cache_put(spa, sha);
    if (oper == 1 && tpa == netif.ip && netif.ip)   // requête pour nous -> réponse
        arp_send(2, sha, spa);
}

bool arp_resolve(ip4_t ip, mac_t *out) {
    if (arp_cache_get(ip, out)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(1, MAC_BCAST, ip);
        uint64_t end = pit_ms() + 250;
        while (pit_ms() < end) {
            nic_poll();
            if (arp_cache_get(ip, out)) return true;
        }
    }
    return false;
}
