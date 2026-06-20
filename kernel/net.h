// =============================================================================
//  kernel/net.h -- Pile réseau sexOs : types communs et API
// =============================================================================
#ifndef SEXOS_NET_H
#define SEXOS_NET_H

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
// ARP NON BLOQUANT : renvoie le MAC si en cache, sinon émet une requête et
// renvoie false (l'appelant laissera la retransmission rejouer la trame).
bool arp_lookup(ip4_t ip, mac_t *out);
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
bool udp_take(uint16_t port, ip4_t *src, uint8_t **data, int *len);   // non bloquant

// TCP (API sockets minimale)
int  tcp_connect(ip4_t dst, uint16_t dport);     // renvoie un id de connexion, -1 si échec
int  tcp_send(int conn, const void *data, int len);
int  tcp_recv(int conn, void *buf, int len, uint32_t timeout_ms);
void tcp_close(int conn);
void tcp_listen_port(uint16_t port);             // ouverture passive (serveur)
int  tcp_accept(uint16_t port, uint32_t timeout_ms);
int  tcp_accept_nb(uint16_t port);               // accept non bloquant (sans nic_poll)

// --- Utilitaires de haut niveau (utilisés par le shell) ----------------------
bool net_dhcp(void);                              // obtient une IP par DHCP
bool net_ping(ip4_t dst, uint32_t *rtt_ms);       // un ping (echo)
bool dns_resolve(const char *name, ip4_t *out);   // résolution DNS
// GET HTTP simple : écrit le corps dans buf (taille max len), renvoie la taille.
int  http_get(const char *host, const char *path, char *buf, int len);
// GET HTTP (ip:port/chemin) : renvoie le CORPS (sans en-tête) dans buf.
int  http_download(ip4_t ip, uint16_t port, const char *host, const char *path,
                   char *buf, int max);

// Formate une IP "a.b.c.d" dans buf (>= 16 octets).
void ip_to_str(ip4_t ip, char *buf);

// Polling avec délai (traite le réseau pendant ~ms millisecondes).
void net_poll_ms(uint32_t ms);

// =============================================================================
//  SERVICE RÉSEAU + SOCKETS NON BLOQUANTES (modèle multi-processus ring 3)
// -----------------------------------------------------------------------------
//  La tâche réseau (net_task_run) est le SEUL propriétaire du NIC après le
//  démarrage : elle pompe les trames reçues et fait avancer TCP/DNS sur minuteur.
//  Les applications ring 3 utilisent l'API NON BLOQUANTE ci-dessous via des
//  appels système : aucune ne bloque le système, aucune ne sonde le NIC.
// =============================================================================
void net_task_run(void);                 // boucle de la tâche réseau (ne revient pas)
void tcp_tick(void);                     // retransmissions / délais (appelée par la tâche)
void dns_tick(void);                     // idem pour les requêtes DNS

// TCP client non bloquant.
int  tcp_open(ip4_t dst, uint16_t dport);    // ouverture active -> id (>=0) ou -1
int  tcp_state(int id);                      // 0=connexion 1=etabli 2=ferme(pair) -1=erreur
int  tcp_write(int id, const void *data, int len);  // bufferise -> octets acceptés / -1
int  tcp_read(int id, void *buf, int len);          // -> octets / 0=rien / -1=fermé
void tcp_shutdown(int id);                   // demande la fermeture (FIN)

// DNS non bloquant : 1=résolu (*out), 0=en cours, -1=échec. Relance si 'name' change.
int  dns_query(const char *name, ip4_t *out);

#endif
