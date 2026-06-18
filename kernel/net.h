// =============================================================================
//  kernel/net.h -- Pile réseau MonOS : types communs et API
// =============================================================================
#ifndef MONOS_NET_H
#define MONOS_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// --- Adresses ----------------------------------------------------------------
typedef struct { uint8_t b[6]; } mac_t;
typedef uint32_t ip4_t;                 // stockée en ordre hôte (big sur le fil)

#define IP4(a,b,c,d) ((ip4_t)(((a)<<24)|((b)<<16)|((c)<<8)|(d)))

// --- Conversions d'ordre d'octets (le réseau est big-endian) -----------------
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

// --- Protocoles --------------------------------------------------------------
#define ETH_ARP   0x0806
#define ETH_IPV4  0x0800
#define IP_ICMP   1
#define IP_UDP    17
#define IP_TCP    6

// --- Configuration de l'interface (remplie par DHCP) -------------------------
typedef struct {
    mac_t mac;
    ip4_t ip, mask, gateway, dns;
    bool  up;
} netif_t;

extern netif_t netif;

// --- Pilote de carte (interface générique) -----------------------------------
//  Le pilote fournit ces fonctions ; la pile les utilise.
bool nic_present(void);
void nic_send(const void *frame, uint16_t len);
void nic_poll(void);                    // traite les trames reçues
mac_t nic_mac(void);

// --- Cœur de la pile ---------------------------------------------------------
void net_init(void);
void net_rx(const uint8_t *data, uint16_t len);   // appelé par le pilote
uint16_t net_checksum(const void *data, int len); // somme de contrôle Internet

// Construit et envoie une trame Ethernet (dst MAC, type, charge utile).
void eth_send(mac_t dst, uint16_t ethertype, const void *payload, uint16_t len);

// ARP : résout une IP en MAC (avec attente bornée). Renvoie false sur timeout.
bool arp_resolve(ip4_t ip, mac_t *out);
void arp_rx(const uint8_t *data, uint16_t len);

// IPv4 / ICMP / UDP / TCP
void ipv4_rx(const uint8_t *data, uint16_t len);
void ipv4_send(ip4_t dst, uint8_t proto, const void *payload, uint16_t len);
void icmp_rx(ip4_t src, const uint8_t *data, uint16_t len);
void udp_rx(ip4_t src, const uint8_t *data, uint16_t len);
void tcp_rx(ip4_t src, const uint8_t *data, uint16_t len);

// UDP : envoi + attente simple sur un port (un échange à la fois).
void udp_send(ip4_t dst, uint16_t sport, uint16_t dport, const void *payload, uint16_t len);
void udp_listen(uint16_t port);
bool udp_wait(uint32_t timeout_ms, ip4_t *src, uint8_t **data, int *len);

// TCP (API sockets minimale)
int  tcp_connect(ip4_t dst, uint16_t dport);     // renvoie un id de connexion, -1 si échec
int  tcp_send(int conn, const void *data, int len);
int  tcp_recv(int conn, void *buf, int len, uint32_t timeout_ms);
void tcp_close(int conn);

// --- Utilitaires de haut niveau (utilisés par le shell) ----------------------
bool net_dhcp(void);                              // obtient une IP par DHCP
bool net_ping(ip4_t dst, uint32_t *rtt_ms);       // un ping (echo)
bool dns_resolve(const char *name, ip4_t *out);   // résolution DNS
// GET HTTP simple : écrit le corps dans buf (taille max len), renvoie la taille.
int  http_get(const char *host, const char *path, char *buf, int len);

// Formate une IP "a.b.c.d" dans buf (>= 16 octets).
void ip_to_str(ip4_t ip, char *buf);

// Polling avec délai (traite le réseau pendant ~ms millisecondes).
void net_poll_ms(uint32_t ms);

#endif
