// =============================================================================
//  kernel/pkg.c -- Gestionnaire de paquets natif imitant pacman
// -----------------------------------------------------------------------------
//  Dépôt HTTP (servi via la pile réseau). Base du dépôt + paquets téléchargés,
//  vérifiés par SHA-256, installés dans le VFS. Base locale des paquets
//  installés sous /var/lib/pacman/local/.
//
//  Format de paquet (texte + données) :
//     MONPAC1
//     name: hello
//     version: 1.0
//     depends: libfoo,libbar
//     FILE /chemin/du/fichier <taille>
//     <octets...>
//     FILE ...
//
//  Format de la base du dépôt (repo.db), une ligne par paquet :
//     <nom> <version> <depends_csv|-> <sha256hex>
// =============================================================================
#include "pkg.h"
#include "net.h"
#include "crypto.h"
#include "vfs.h"
#include "klib.h"
#include "heap.h"

// Dépôt : accessible via la passerelle SLIRP de QEMU (hôte).
#define REPO_IP   IP4(10,0,2,2)
#define REPO_PORT 8000
#define REPO_HOST "10.0.2.2"

static pkg_out_t out;
void pkg_set_output(pkg_out_t fn) { out = fn; }
static void say(const char *s) { if (out) out(s); }

// --- Utilitaires VFS ---------------------------------------------------------
// Crée (si besoin) l'arborescence d'un chemin de dossier et renvoie le nœud.
static vfs_node_t *mkpath(const char *path) {
    vfs_node_t *cur = vfs_root();
    char comp[VFS_NAME_MAX]; int i = (path[0] == '/') ? 1 : 0;
    while (path[i]) {
        int j = 0;
        while (path[i] && path[i] != '/' && j < VFS_NAME_MAX - 1) comp[j++] = path[i++];
        comp[j] = 0;
        while (path[i] == '/') i++;
        if (!j) continue;
        vfs_node_t *n = vfs_lookup(cur, comp);
        if (!n) n = vfs_create(cur, comp, VFS_DIR);
        if (!n) return NULL;
        cur = n;
    }
    return cur;
}

// Écrit un fichier (crée les dossiers parents).
static bool write_file(const char *path, const uint8_t *data, int len) {
    char dir[256]; int slash = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') slash = i;
    if (slash <= 0) return false;
    memcpy(dir, path, slash); dir[slash] = 0;
    vfs_node_t *d = mkpath(dir);
    if (!d) return false;
    const char *base = path + slash + 1;
    vfs_node_t *f = vfs_lookup(d, base);
    if (!f) f = vfs_create(d, base, VFS_FILE);
    if (!f) return false;
    f->size = 0;
    int w = vfs_write(f, 0, data, len);
    return w >= 0;
}

// --- SHA-256 hexadécimal -----------------------------------------------------
static void sha_hex(const uint8_t *data, int len, char *hex) {
    uint8_t h[32]; sha256(data, len, h);
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex[i*2] = d[h[i]>>4]; hex[i*2+1] = d[h[i]&0xF]; }
    hex[64] = 0;
}

// --- Téléchargement ----------------------------------------------------------
static int download(const char *path, char *buf, int max) {
    if (!netif.up) return -1;
    return http_download(REPO_IP, REPO_PORT, REPO_HOST, path, buf, max);
}

// --- Base du dépôt (repo.db) -------------------------------------------------
static vfs_node_t *repo_db(void) { return vfs_resolve("/var/lib/pacman/repo.db"); }

// Cherche une ligne de repo.db pour 'name'. Remplit version/deps/sha.
static bool repo_find(const char *name, char *ver, char *deps, char *sha) {
    vfs_node_t *db = repo_db();
    if (!db) return false;
    char *p = (char *)db->data; int n = db->size;
    int i = 0;
    while (i < n) {
        char line[256]; int l = 0;
        while (i < n && db->data[i] != '\n' && l < 255) line[l++] = db->data[i++];
        if (i < n) i++;
        line[l] = 0;
        // <nom> <ver> <deps> <sha>
        char nm[64], vr[32], dp[160], sh[80];
        int f = 0, k = 0; char *tok[4] = { nm, vr, dp, sh }; int max_[4] = {63,31,159,79};
        for (int c = 0; line[c] && f < 4; c++) {
            if (line[c] == ' ') { tok[f][k] = 0; f++; k = 0; }
            else if (k < max_[f]) tok[f][k++] = line[c];
        }
        if (f < 4) { if (f==3) tok[3][k]=0; }
        else tok[3][k] = 0;
        if (strcmp(nm, name) == 0) {
            if (ver) strcpy(ver, vr);
            if (deps) strcpy(deps, dp);
            if (sha) strcpy(sha, sh);
            return true;
        }
        (void)p;
    }
    return false;
}

// --- Base locale (paquets installés) -----------------------------------------
static bool is_installed(const char *name) {
    char path[128]; strcpy(path, "/var/lib/pacman/local/"); strcat(path, name);
    return vfs_resolve(path) != NULL;
}

// --- Installation d'un paquet (avec dépendances) -----------------------------
static int do_install(const char *name, int depth);

static int install_deps(const char *deps, int depth) {
    char buf[160]; strncpy(buf, deps, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    if (buf[0] == 0 || strcmp(buf, "-") == 0) return 0;
    char *p = buf;
    while (*p) {
        char dep[64]; int k = 0;
        while (*p && *p != ',' && k < 63) dep[k++] = *p++;
        dep[k] = 0; if (*p == ',') p++;
        if (dep[0] && !is_installed(dep)) {
            if (do_install(dep, depth + 1) != 0) return -1;
        }
    }
    return 0;
}

// Analyse et installe le contenu d'un paquet (déjà téléchargé en mémoire).
static int extract(const char *name, const char *version, const uint8_t *pkg, int len) {
    if (len < 7 || memcmp(pkg, "MONPAC1", 7) != 0) { say("  format de paquet invalide\n"); return -1; }
    // Liste des fichiers installés (pour la base locale).
    static char filelist[4096]; int fl = 0;

    int i = 0;
    // saute la 1ère ligne (MONPAC1)
    while (i < len && pkg[i] != '\n') i++; i++;
    while (i < len) {
        // lit une ligne
        char line[300]; int l = 0;
        while (i < len && pkg[i] != '\n' && l < 299) line[l++] = pkg[i++];
        if (i < len) i++;
        line[l] = 0;
        if (strncmp(line, "FILE ", 5) == 0) {
            // FILE <chemin> <taille>
            char path[200]; int pi = 0, c = 5;
            while (line[c] && line[c] != ' ' && pi < 199) path[pi++] = line[c++];
            path[pi] = 0;
            while (line[c] == ' ') c++;
            int sz = 0; for (; line[c] >= '0' && line[c] <= '9'; c++) sz = sz*10 + (line[c]-'0');
            if (i + sz > len) break;
            write_file(path, pkg + i, sz);
            i += sz;
            if (i < len && pkg[i] == '\n') i++;
            // ajoute à la liste
            for (int k = 0; path[k] && fl < 4090; k++) filelist[fl++] = path[k];
            filelist[fl++] = '\n';
        }
    }
    filelist[fl] = 0;

    // Enregistre dans la base locale : /var/lib/pacman/local/<name>
    mkpath("/var/lib/pacman/local");
    char dbpath[128]; strcpy(dbpath, "/var/lib/pacman/local/"); strcat(dbpath, name);
    char content[4200]; int co = 0;
    for (const char *v = version; *v; v++) content[co++] = *v;
    content[co++] = '\n';
    for (int k = 0; filelist[k]; k++) content[co++] = filelist[k];
    content[co] = 0;
    write_file(dbpath, (uint8_t *)content, co);
    return 0;
}

static int do_install(const char *name, int depth) {
    if (depth > 8) return -1;
    if (is_installed(name)) { say("  "); say(name); say(" : deja installe\n"); return 0; }

    char ver[32], deps[160], sha[80];
    if (!repo_find(name, ver, deps, sha)) { say("  paquet introuvable : "); say(name); say("\n"); return -1; }

    if (install_deps(deps, depth) != 0) return -1;

    // Télécharge le paquet : /<nom>-<version>.pkg
    char path[128]; strcpy(path, "/"); strcat(path, name); strcat(path, "-"); strcat(path, ver); strcat(path, ".pkg");
    static char pkgbuf[200000];
    say("  telechargement de "); say(name); say("-"); say(ver); say("...\n");
    int n = download(path, pkgbuf, sizeof(pkgbuf));
    if (n <= 0) { say("  echec du telechargement\n"); return -1; }

    // Vérifie le SHA-256.
    char hex[65]; sha_hex((uint8_t *)pkgbuf, n, hex);
    if (sha[0] && strcmp(hex, sha) != 0) {
        say("  ERREUR d'integrite (SHA-256) pour "); say(name); say("\n");
        return -1;
    }
    say("  integrite verifiee (SHA-256)\n");

    if (extract(name, ver, (uint8_t *)pkgbuf, n) != 0) return -1;
    say("  installe : "); say(name); say(" "); say(ver); say("\n");
    return 0;
}

// --- Commandes publiques -----------------------------------------------------
int pkg_sync(void) {
    if (!netif.up) { say("reseau indisponible\n"); return -1; }
    mkpath("/var/lib/pacman");
    static char buf[65536];
    say("synchronisation de la base du depot...\n");
    int n = download("/repo.db", buf, sizeof(buf));
    if (n <= 0) { say("echec : depot injoignable ("); say(REPO_HOST); say(")\n"); return -1; }
    if (!write_file("/var/lib/pacman/repo.db", (uint8_t *)buf, n)) {
        say("echec : ecriture de la base impossible\n"); return -1;
    }
    say("base synchronisee.\n");
    return 0;
}

int pkg_install(const char *name) {
    if (!repo_db()) { say("executez d'abord : pacman -Sy\n"); return -1; }
    say("installation de "); say(name); say(" :\n");
    return do_install(name, 0);
}

int pkg_remove(const char *name) {
    char dbpath[128]; strcpy(dbpath, "/var/lib/pacman/local/"); strcat(dbpath, name);
    vfs_node_t *db = vfs_resolve(dbpath);
    if (!db) { say("paquet non installe : "); say(name); say("\n"); return -1; }
    // Lignes : version, puis fichiers.
    char *p = (char *)db->data; int n = db->size, i = 0;
    // saute la version
    while (i < n && p[i] != '\n') i++; i++;
    while (i < n) {
        char path[200]; int l = 0;
        while (i < n && p[i] != '\n' && l < 199) path[l++] = p[i++];
        if (i < n) i++;
        path[l] = 0;
        if (path[0] == '/') { vfs_node_t *f = vfs_resolve(path); if (f) vfs_delete(f); }
    }
    vfs_delete(db);
    say("supprime : "); say(name); say("\n");
    return 0;
}

int pkg_query(void) {
    vfs_node_t *local = vfs_resolve("/var/lib/pacman/local");
    if (!local || !local->children) { say("aucun paquet installe.\n"); return 0; }
    for (vfs_node_t *c = local->children; c; c = c->next) {
        // 1ère ligne du fichier = version
        char ver[32]; int l = 0;
        for (size_t k = 0; k < c->size && c->data[k] != '\n' && l < 31; k++) ver[l++] = c->data[k];
        ver[l] = 0;
        say(c->name); say(" "); say(ver); say("\n");
    }
    return 0;
}

int pkg_upgrade(void) {
    if (pkg_sync() != 0) return -1;
    vfs_node_t *local = vfs_resolve("/var/lib/pacman/local");
    if (!local) { say("rien a mettre a jour.\n"); return 0; }
    int up = 0;
    for (vfs_node_t *c = local->children; c; c = c->next) {
        char ver[32]; int l = 0;
        for (size_t k = 0; k < c->size && c->data[k] != '\n' && l < 31; k++) ver[l++] = c->data[k];
        ver[l] = 0;
        char rver[32], rd[160], rs[80];
        if (repo_find(c->name, rver, rd, rs) && strcmp(rver, ver) != 0) {
            say("mise a jour de "); say(c->name); say(" "); say(ver); say(" -> "); say(rver); say("\n");
            char nm[64]; strcpy(nm, c->name);
            pkg_remove(nm); do_install(nm, 0); up++;
        }
    }
    if (!up) say("tout est a jour.\n");
    return 0;
}
