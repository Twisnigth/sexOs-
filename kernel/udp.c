// =============================================================================
//  kernel/udp.c -- UDP (datagrammes) + attente simple sur un port
// =============================================================================
#include "net.h"
#include "klib.h"
#include "pit.h"

// Une seule attente en cours à la fois (suffit pour DHCP/DNS séquentiels).
static struct {
    uint16_t port;
    ip4_t    src;
    uint16_t sport;
    uint8_t  buf[1500];
    int      len;
    volatile bool ready;
} waiter;

void udp_listen(uint16_t port) { waiter.port = port; waiter.ready = false; }

bool udp_wait(uint32_t timeout_ms, ip4_t *src, uint8_t **data, int *len) {
    uint64_t end = pit_ms() + timeout_ms;
    while (pit_ms() < end) {
        nic_poll();
        if (waiter.ready) {
            if (src) *src = waiter.src;
            if (data) *data = waiter.buf;
            if (len) *len = waiter.len;
            waiter.ready = false;
            return true;
        }
    }
    return false;
}

void udp_send(ip4_t dst, uint16_t sport, uint16_t dport, const void *payload, uint16_t len) {
    uint8_t pkt[1500];
    uint16_t total = 8 + len;
    pkt[0]=sport>>8; pkt[1]=sport&0xFF;
    pkt[2]=dport>>8; pkt[3]=dport&0xFF;
    pkt[4]=total>>8; pkt[5]=total&0xFF;
    pkt[6]=0; pkt[7]=0;                       // checksum 0 (optionnelle en IPv4)
    memcpy(pkt + 8, payload, len);
    ipv4_send(dst, IP_UDP, pkt, total);
}

void udp_rx(ip4_t src, const uint8_t *d, uint16_t len) {
    if (len < 8) return;
    uint16_t dport = (d[2] << 8) | d[3];
    if (dport != waiter.port) return;
    uint16_t ulen = (d[4] << 8) | d[5];
    if (ulen < 8 || ulen > len) ulen = len;
    int plen = ulen - 8;
    if (plen > (int)sizeof(waiter.buf)) plen = sizeof(waiter.buf);
    memcpy(waiter.buf, d + 8, plen);
    waiter.len = plen;
    waiter.src = src;
    waiter.sport = (d[0] << 8) | d[1];
    waiter.ready = true;
}
