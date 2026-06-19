// =============================================================================
//  user/lib/http.c -- Client HTTP/1.1 ring 3 (HTTP + HTTPS via TLS) + redirections
// =============================================================================
#include "monos.h"
#include "http.h"
#include "tls.h"

void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
int strncmp(const char *, const char *, unsigned long);

static int ms(void) { return (int)sys_time_ms(); }

// Reconnaît "a.b.c.d" -> entier (ordre hôte). 0 sinon.
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

static char raw[400000];                          // réponse brute (en-tête + corps)

int  http_last_secure, http_last_verified;
char http_last_vinfo[72];

int http_fetch(const char *url, char *body, int maxbody, int *status, const char **err) {
    if (status) *status = 0;
    if (err) *err = 0;

    char cur[1024]; { int i = 0; for (; url[i] && i < 1023; i++) cur[i] = url[i]; cur[i] = 0; }

    for (int hop = 0; hop < 6; hop++) {
        const char *p = cur;
        int secure = 0;
        if (!strncmp(p, "http://", 7)) p += 7;
        else if (!strncmp(p, "https://", 8)) { p += 8; secure = 1; }

        char host[256]; int hi = 0;
        while (*p && *p != '/' && *p != ':' && hi < 255) host[hi++] = *p++;
        host[hi] = 0;
        int port = secure ? 443 : 80;
        if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0'); }
        const char *path = (*p == '/') ? p : "/";

        // --- Résolution (IP directe ou DNS non bloquant) --------------------
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

        // --- Connexion TCP --------------------------------------------------
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

        // --- TLS si https:// ------------------------------------------------
        static tls_t tls;
        http_last_secure = secure; http_last_verified = 0; http_last_vinfo[0] = 0;
        if (secure) {
            if (tls_handshake(&tls, c, host, err) != 0) { sys_tcp_close(c); return -1; }
            http_last_verified = tls.verified;
            for (int i = 0; i < 72; i++) { http_last_vinfo[i] = tls.verify_info[i]; if (!tls.verify_info[i]) break; }
        }

        // --- Requête GET ----------------------------------------------------
        char req[1200]; int o = 0;
        const char *a = "GET ";                   while (*a) req[o++] = *a++;
        for (const char *q = path; *q; q++) req[o++] = *q;
        a = " HTTP/1.1\r\nHost: ";                 while (*a) req[o++] = *a++;
        for (const char *q = host; *q; q++) req[o++] = *q;
        a = "\r\nUser-Agent: MonOS/2.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        while (*a) req[o++] = *a++;

        if (secure) {
            tls_send(&tls, req, o);
        } else {
            int sent = 0; dl = ms() + 5000;
            while (sent < o) {
                int n = sys_tcp_send(c, req + sent, o - sent);
                if (n > 0) sent += n;
                else if (n < 0) { sys_tcp_close(c); if (err) *err = "envoi echoue"; return -1; }
                else { if (ms() > dl) { sys_tcp_close(c); if (err) *err = "envoi : delai"; return -1; } sys_yield(); }
            }
        }

        // --- Réception jusqu'à fermeture ------------------------------------
        int total = 0; dl = ms() + 15000;
        for (;;) {
            int n = secure ? tls_recv(&tls, raw + total, (int)sizeof(raw) - 1 - total)
                           : sys_tcp_recv(c, raw + total, (int)sizeof(raw) - 1 - total);
            if (n > 0) { total += n; dl = ms() + 15000; if (total >= (int)sizeof(raw) - 1) break; }
            else if (n < 0) break;
            else { if (ms() > dl) break; sys_yield(); }
        }
        raw[total] = 0;
        if (secure) tls_close(&tls); else sys_tcp_close(c);
        if (total == 0) { if (err) *err = "aucune donnee recue"; return -1; }

        // --- Statut + fin d'en-tête -----------------------------------------
        int code = 0;
        { const char *q = raw; while (*q && *q != ' ') q++; while (*q == ' ') q++;
          while (*q >= '0' && *q <= '9') code = code * 10 + (*q++ - '0'); }
        if (status) *status = code;
        int hb = -1;
        for (int i = 0; i + 3 < total; i++)
            if (raw[i]=='\r'&&raw[i+1]=='\n'&&raw[i+2]=='\r'&&raw[i+3]=='\n') { hb = i + 4; break; }
        if (hb < 0) { if (err) *err = "reponse HTTP invalide"; return -1; }

        // --- Redirection (3xx + Location) -----------------------------------
        if ((code==301||code==302||code==303||code==307||code==308)) {
            int li = ci_find(raw, hb, "\nlocation:");
            if (li >= 0) {
                int j = li + 10; while (j < hb && (raw[j]==' ')) j++;
                char loc[1024]; int k = 0;
                while (j < hb && raw[j] != '\r' && raw[j] != '\n' && k < 1023) loc[k++] = raw[j++];
                loc[k] = 0;
                // Construit la prochaine URL (absolue ou relative à la racine).
                if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
                    int i = 0; for (; loc[i] && i < 1023; i++) cur[i] = loc[i]; cur[i] = 0;
                } else {
                    int o2 = 0; const char *sc = secure ? "https://" : "http://";
                    while (*sc) cur[o2++] = *sc++;
                    for (const char *q = host; *q; q++) cur[o2++] = *q;
                    if (loc[0] != '/') cur[o2++] = '/';
                    for (int i = 0; loc[i]; i++) cur[o2++] = loc[i];
                    cur[o2] = 0;
                }
                continue;                          // suit la redirection
            }
        }

        // --- Corps : chunked ou direct --------------------------------------
        int chunked = ci_find(raw, hb, "transfer-encoding: chunked") >= 0;
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
                while (i < total && raw[i] != '\n') i++;
                if (i < total) i++;
                if (!got || sz == 0) break;
                int end = i + sz; if (end > total) end = total;
                while (i < end) { if (bi < maxbody) body[bi++] = raw[i]; i++; }
                while (i < total && raw[i] != '\n') i++;
                if (i < total) i++;
            }
            return bi;
        }
        int blen = total - hb;
        if (blen > maxbody) blen = maxbody;
        memcpy(body, raw + hb, blen);
        return blen;
    }
    if (err) *err = "trop de redirections";
    return -1;
}
