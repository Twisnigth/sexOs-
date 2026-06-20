// =============================================================================
//  kernel/httpd.c -- Serveur web minimal (HTTP/1.0)
// -----------------------------------------------------------------------------
//  Tache noyau analogue a sshd : ecoute le port 80, sert les fichiers du VFS
//  (racine = /home/user). Une requete par connexion, puis fermeture. E/S non
//  bloquantes par-dessus la pile TCP (la tache reseau possede le NIC).
// =============================================================================
#include "httpd.h"
#include "net.h"
#include "vfs.h"
#include "klib.h"
#include "pit.h"

#define WEBROOT "/home/user"

static bool httpd_ready;

void httpd_init(void) {
    tcp_listen_port(80);
    httpd_ready = true;
    kprintf("[httpd] serveur web pret sur le port 80 (racine %s)\n", WEBROOT);
}

// --- E/S non bloquantes (cede le CPU a la tache reseau via hlt) --------------
static int h_read(int c, char *buf, int max, uint64_t deadline) {
    for (;;) {
        __asm__ volatile ("cli"); int n = tcp_read(c, buf, max); __asm__ volatile ("sti");
        if (n != 0) return n;
        if (pit_ms() > deadline) return 0;
        __asm__ volatile ("hlt");
    }
}
static bool h_write(int c, const char *buf, int len) {
    int off = 0; uint64_t dl = pit_ms() + 8000;
    while (off < len) {
        __asm__ volatile ("cli"); int n = tcp_write(c, buf + off, len - off); __asm__ volatile ("sti");
        if (n > 0) { off += n; dl = pit_ms() + 8000; }
        else if (n < 0) return false;
        else { if (pit_ms() > dl) return false; __asm__ volatile ("hlt"); }
    }
    return true;
}

// Type MIME d'apres l'extension.
static const char *mime_of(const char *path) {
    int n = (int)strlen(path);
    if (n > 5 && strcmp(path + n - 5, ".html") == 0) return "text/html; charset=utf-8";
    if (n > 4 && strcmp(path + n - 4, ".css") == 0)  return "text/css; charset=utf-8";
    if (n > 3 && strcmp(path + n - 3, ".js") == 0)   return "application/javascript";
    return "text/plain; charset=utf-8";
}

static char req[2048];
static char body[65536];

void httpd_run(void) {
    for (;;) {
        __asm__ volatile ("cli"); int c = httpd_ready ? tcp_accept_nb(80) : -1; __asm__ volatile ("sti");
        if (c < 0) { __asm__ volatile ("hlt"); continue; }

        // --- Lecture de la requete (jusqu'a la fin des en-tetes) ---
        int n = 0; uint64_t dl = pit_ms() + 5000;
        while (n < (int)sizeof(req) - 1) {
            int r = h_read(c, req + n, sizeof(req) - 1 - n, dl);
            if (r <= 0) break;
            n += r; req[n] = 0;
            bool end = false;
            for (int i = 0; i + 3 < n; i++)
                if (req[i]=='\r' && req[i+1]=='\n' && req[i+2]=='\r' && req[i+3]=='\n') { end = true; break; }
            if (end) break;
        }

        // --- Chemin demande (GET /chemin ...) ---
        char path[256] = "/";
        if (strncmp(req, "GET ", 4) == 0) {
            int i = 4, j = 0;
            while (req[i] && req[i] != ' ' && req[i] != '?' && j < 255) path[j++] = req[i++];
            path[j] = 0;
        }
        kprintf("[httpd] GET %s\n", path);

        char vpath[320]; strcpy(vpath, WEBROOT);
        if (strcmp(path, "/") != 0) strcat(vpath, path);   // WEBROOT(10)+path(<=255) < 320
        vfs_node_t *node = vfs_resolve(vpath);

        int blen = 0, status = 200; const char *stext = "OK", *ct = "text/html; charset=utf-8";
        #define B(s) do { for (const char *q=(s); *q && blen < (int)sizeof(body)-1; q++) body[blen++]=*q; } while (0)

        if (!node) {
            status = 404; stext = "Not Found";
            B("<!doctype html><meta charset=utf-8><body style='background:#11141f;color:#d0e0d0;font-family:monospace'>");
            B("<h1 style='color:#ff7ab0'>404</h1><p>Fichier introuvable sur sexOs.</p>");
        } else if (node->type == VFS_DIR) {
          // Si le dossier contient index.html, on le sert comme page d'accueil.
          vfs_node_t *idx = NULL;
          for (vfs_node_t *ch = node->children; ch; ch = ch->next)
              if (ch->type == VFS_FILE && strcmp(ch->name, "index.html") == 0) { idx = ch; break; }
          if (idx) {
            int r = vfs_read(idx, 0, body, sizeof(body) - 1);
            blen = (r < 0) ? 0 : r; ct = "text/html; charset=utf-8";
          } else {
            B("<!doctype html><html><head><meta charset=utf-8><title>sexOs</title><style>");
            B("body{background:#11141f;color:#d0e0d0;font-family:monospace;padding:24px}");
            B("a{color:#6ee79a;text-decoration:none}a:hover{text-decoration:underline}");
            B("h1{color:#ff7ab0}li{margin:3px 0}</style></head><body>");
            B("<h1>sexOs &mdash; "); B(path); B("</h1><ul>");
            for (vfs_node_t *ch = node->children; ch; ch = ch->next) {
                B("<li><a href=\"");
                if (strcmp(path, "/") == 0) { B("/"); }
                else { B(path); if (path[strlen(path)-1] != '/') B("/"); }
                B(ch->name);
                if (ch->type == VFS_DIR) B("/");
                B("\">"); B(ch->name); if (ch->type == VFS_DIR) B("/"); B("</a></li>");
            }
            B("</ul><hr><p style='color:#8a98a6'>servi par sexOs httpd</p></body></html>");
          }
        } else {
            int r = vfs_read(node, 0, body, sizeof(body) - 1);
            blen = (r < 0) ? 0 : r;
            ct = mime_of(vpath);
        }
        #undef B

        // --- En-tetes + corps ---
        char hdr[300]; char nb[12];
        strcpy(hdr, "HTTP/1.0 "); utoa(status, nb, 10); strcat(hdr, nb); strcat(hdr, " "); strcat(hdr, stext);
        strcat(hdr, "\r\nServer: sexOs\r\nConnection: close\r\nContent-Type: "); strcat(hdr, ct);
        strcat(hdr, "\r\nContent-Length: "); utoa(blen, nb, 10); strcat(hdr, nb); strcat(hdr, "\r\n\r\n");
        h_write(c, hdr, (int)strlen(hdr));
        h_write(c, body, blen);

        __asm__ volatile ("cli"); tcp_shutdown(c); __asm__ volatile ("sti");
    }
}
