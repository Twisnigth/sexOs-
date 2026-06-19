// =============================================================================
//  user/lib/http.c -- Client HTTP/1.1 ring 3 au-dessus des sockets TCP noyau.
// =============================================================================
#include "monos.h"
#include "http.h"

void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
int strncmp(const char *, const char *, unsigned long);

static int ms(void) { return (int)sys_time_ms(); }

// Reconnaît "a.b.c.d" et le convertit en entier (ordre hôte). 0 sinon.
static int parse_ip(const char *h, uint32_t *ip) {
    uint32_t part[4]; int pi = 0, v = 0, dig = 0;
    for (const char *p = h; ; p++) {
        if (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); dig = 1; if (v > 255) return 0; }
        else if (*p == '.' || *p == 0) {
            if (!dig || pi > 3) return 0;
            part[pi++] = v; v = 0; dig = 0;
            if (*p == 0) break;
        } else return 0;
    }
    if (pi != 4) return 0;
    *ip = (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3];
    return 1;
}

// Recherche insensible à la casse de 'needle' dans hay[0..n).
static int ci_find(const char *hay, int n, const char *needle) {
    int m = (int)strlen(needle);
    for (int i = 0; i + m <= n; i++) {
        int k = 0;
        for (; k < m; k++) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == m) return i;
    }
    return -1;
}

static char raw[160000];                 // réponse brute (en-tête + corps)

int http_fetch(const char *url, char *body, int maxbody, int *status, const char **err) {
    if (status) *status = 0;
    if (err) *err = 0;

    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    else if (!strncmp(p, "https://", 8)) { if (err) *err = "HTTPS non supporte (TLS a venir)"; return -1; }

    char host[128]; int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 127) host[hi++] = *p++;
    host[hi] = 0;
    int port = 80;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0'); }
    const char *path = (*p == '/') ? p : "/";

    // --- Résolution (IP directe ou DNS non bloquant) -------------------------
    uint32_t ip;
    if (!parse_ip(host, &ip)) {
        int dl = ms() + 8000;
        for (;;) {
            int r = sys_dns_resolve(host, &ip);
            if (r == 1) break;
            if (r < 0) { if (err) *err = "DNS : nom introuvable"; return -1; }
            if (ms() > dl) { if (err) *err = "DNS : delai depasse"; return -1; }
            sys_yield();
        }
    }

    // --- Connexion TCP (non bloquante) ---------------------------------------
    int c = sys_tcp_open(ip, port);
    if (c < 0) { if (err) *err = "aucune socket libre"; return -1; }
    int dl = ms() + 8000;
    for (;;) {
        int s = sys_tcp_state(c);
        if (s == 1) break;
        if (s < 0) { sys_tcp_close(c); if (err) *err = "connexion refusee"; return -1; }
        if (ms() > dl) { sys_tcp_close(c); if (err) *err = "connexion : delai depasse"; return -1; }
        sys_yield();
    }

    // --- Requête GET ---------------------------------------------------------
    char req[700]; int o = 0;
    const char *a = "GET ";                       while (*a) req[o++] = *a++;
    for (const char *q = path; *q; q++) req[o++] = *q;
    a = " HTTP/1.1\r\nHost: ";                     while (*a) req[o++] = *a++;
    for (const char *q = host; *q; q++) req[o++] = *q;
    a = "\r\nUser-Agent: MonOS/2.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    while (*a) req[o++] = *a++;

    int sent = 0; dl = ms() + 5000;
    while (sent < o) {
        int n = sys_tcp_send(c, req + sent, o - sent);
        if (n > 0) { sent += n; }
        else if (n < 0) { sys_tcp_close(c); if (err) *err = "envoi echoue"; return -1; }
        else { if (ms() > dl) { sys_tcp_close(c); if (err) *err = "envoi : delai"; return -1; } sys_yield(); }
    }

    // --- Réception jusqu'à fermeture par le pair -----------------------------
    int total = 0; dl = ms() + 15000;
    for (;;) {
        int n = sys_tcp_recv(c, raw + total, (int)sizeof(raw) - 1 - total);
        if (n > 0) { total += n; dl = ms() + 15000; if (total >= (int)sizeof(raw) - 1) break; }
        else if (n < 0) break;                    // pair a fermé : fin de réponse
        else { if (ms() > dl) break; sys_yield(); }
    }
    raw[total] = 0;
    sys_tcp_close(c);
    if (total == 0) { if (err) *err = "aucune donnee recue"; return -1; }

    // --- Code de statut ------------------------------------------------------
    int code = 0;
    { const char *q = raw; while (*q && *q != ' ') q++; while (*q == ' ') q++;
      while (*q >= '0' && *q <= '9') code = code * 10 + (*q++ - '0'); }
    if (status) *status = code;

    // --- Séparation en-tête / corps ------------------------------------------
    int hb = -1;
    for (int i = 0; i + 3 < total; i++)
        if (raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n') { hb = i + 4; break; }
    if (hb < 0) { if (err) *err = "reponse HTTP invalide"; return -1; }

    int chunked = ci_find(raw, hb, "transfer-encoding: chunked") >= 0;

    // --- Corps : décodage chunked ou copie directe ---------------------------
    if (chunked) {
        int bi = 0, i = hb;
        while (i < total) {
            int sz = 0, got = 0;
            while (i < total && raw[i] != '\r') {
                char ch = raw[i]; int d;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                else break;
                sz = sz * 16 + d; got = 1; i++;
            }
            while (i < total && raw[i] != '\n') i++;   // fin de ligne de taille
            if (i < total) i++;
            if (!got || sz == 0) break;
            int end = i + sz; if (end > total) end = total;
            while (i < end) { if (bi < maxbody) body[bi++] = raw[i]; i++; }
            while (i < total && raw[i] != '\n') i++;    // CRLF de fin de chunk
            if (i < total) i++;
        }
        return bi;
    }
    int blen = total - hb;
    if (blen > maxbody) blen = maxbody;
    memcpy(body, raw + hb, blen);
    return blen;
}
