// =============================================================================
//  kernel/ipv4.c -- IPv4 + ICMP (ping)
// =============================================================================
#include "net.h"
#include "klib.h"
#include "pit.h"

static uint16_t ip_id = 1;

// Détermine le prochain saut (destination locale ou passerelle) et envoie.
void ipv4_send(ip4_t dst, uint8_t proto, const void *payload, uint16_t len) {
    uint8_t pkt[1500];
    uint16_t total = 20 + len;
    pkt[0] = 0x45;                       // version 4, IHL 5
    pkt[1] = 0;                          // TOS
    pkt[2] = total >> 8; pkt[3] = total & 0xFF;
    pkt[4] = ip_id >> 8; pkt[5] = ip_id & 0xFF; ip_id++;
    pkt[6] = 0x40; pkt[7] = 0;           // flags: don't fragment
    pkt[8] = 64;                         // TTL
    pkt[9] = proto;
    pkt[10] = 0; pkt[11] = 0;            // checksum (calculée ensuite)
    pkt[12]=netif.ip>>24; pkt[13]=netif.ip>>16; pkt[14]=netif.ip>>8; pkt[15]=netif.ip;
    pkt[16]=dst>>24; pkt[17]=dst>>16; pkt[18]=dst>>8; pkt[19]=dst;
    uint16_t cs = net_checksum(pkt, 20);
    pkt[10] = cs >> 8; pkt[11] = cs & 0xFF;
    memcpy(pkt + 20, payload, len);

    // Diffusion : trame Ethernet broadcast directe (pas d'ARP).
    if (dst == 0xFFFFFFFF) {
        mac_t bcast = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};
        eth_send(bcast, ETH_IPV4, pkt, total);
        return;
    }
    ip4_t nexthop = ((dst & netif.mask) == (netif.ip & netif.mask)) ? dst : netif.gateway;
    mac_t mac;
    // ARP NON bloquant : sur défaut de cache, la trame est abandonnée (une
    // requête ARP est émise) ; TCP/DNS la rejoueront via leur retransmission.
    // Indispensable : la tâche réseau émet avec le minuteur masqué et ne peut
    // donc pas attendre activement la réponse ARP.
    if (!arp_lookup(nexthop, &mac)) return;
    eth_send(mac, ETH_IPV4, pkt, total);
}

void ipv4_rx(const uint8_t *d, uint16_t len) {
    if (len < 20) return;
    // Le champ "total length" borne la charge utile : on ignore ainsi le
    // remplissage Ethernet des petites trames (sinon pris pour des données).
    uint16_t total = (d[2] << 8) | d[3];
    if (total >= 20 && total < len) len = total;
    int ihl = (d[0] & 0x0F) * 4;
    uint8_t proto = d[9];
    ip4_t src = (d[12]<<24)|(d[13]<<16)|(d[14]<<8)|d[15];
    ip4_t dst = (d[16]<<24)|(d[17]<<16)|(d[18]<<8)|d[19];
    if (netif.ip && dst != netif.ip && dst != 0xFFFFFFFF) return;   // pas pour nous
    const uint8_t *payload = d + ihl;
    uint16_t plen = len - ihl;
    if (proto == IP_ICMP)      icmp_rx(src, payload, plen);
    else if (proto == IP_UDP)  udp_rx(src, payload, plen);
    else if (proto == IP_TCP)  tcp_rx(src, payload, plen);
}

// --- ICMP --------------------------------------------------------------------
static volatile bool ping_got;
static uint16_t ping_id, ping_seq;

void icmp_rx(ip4_t src, const uint8_t *d, uint16_t len) {
    if (len < 8) return;
    uint8_t type = d[0];
    if (type == 8) {                     // echo request -> on répond
        uint8_t reply[1500];
        if (len > 1500) len = 1500;
        memcpy(reply, d, len);
        reply[0] = 0;                    // echo reply
        reply[2] = 0; reply[3] = 0;
        uint16_t cs = net_checksum(reply, len);
        reply[2] = cs >> 8; reply[3] = cs & 0xFF;
        ipv4_send(src, IP_ICMP, reply, len);
    } else if (type == 0) {              // echo reply
        uint16_t id = (d[4] << 8) | d[5];
        uint16_t seq = (d[6] << 8) | d[7];
        if (id == ping_id && seq == ping_seq) ping_got = true;
    }
}

// --- Ping NON BLOQUANT (pour la commande « ping » en ring 3) -----------------
//  L'envoi ne sonde pas le NIC (ipv4_send se contente d'émettre) ; la réponse
//  est captée par icmp_rx, exécuté dans la tâche réseau. L'appelant (ring 3)
//  interroge icmp_ping_got() en cédant le CPU.
void icmp_ping_send(ip4_t dst) {
    static uint16_t seq = 0;
    ping_id = 0xAB00; ping_seq = ++seq; ping_got = false;
    uint8_t p[40];
    p[0] = 8; p[1] = 0; p[2] = 0; p[3] = 0;
    p[4] = ping_id >> 8; p[5] = ping_id & 0xFF;
    p[6] = ping_seq >> 8; p[7] = ping_seq & 0xFF;
    for (int i = 8; i < 40; i++) p[i] = (uint8_t)i;
    uint16_t cs = net_checksum(p, 40); p[2] = cs >> 8; p[3] = cs & 0xFF;
    ipv4_send(dst, IP_ICMP, p, 40);
}
bool icmp_ping_got(void) { return ping_got; }

bool net_ping(ip4_t dst, uint32_t *rtt_ms) {
    static uint16_t seq = 0;
    ping_id = 0xAB00; ping_seq = ++seq; ping_got = false;
    uint8_t p[40];
    p[0] = 8; p[1] = 0; p[2] = 0; p[3] = 0;          // type echo, code 0
    p[4] = ping_id >> 8; p[5] = ping_id & 0xFF;
    p[6] = ping_seq >> 8; p[7] = ping_seq & 0xFF;
    for (int i = 8; i < 40; i++) p[i] = (uint8_t)i;
    uint16_t cs = net_checksum(p, 40);
    p[2] = cs >> 8; p[3] = cs & 0xFF;

    uint64_t t0 = pit_ms();
    ipv4_send(dst, IP_ICMP, p, 40);
    uint64_t end = t0 + 1000;
    while (pit_ms() < end) {
        nic_poll();
        if (ping_got) { if (rtt_ms) *rtt_ms = (uint32_t)(pit_ms() - t0); return true; }
    }
    return false;
}
