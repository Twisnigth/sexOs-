// =============================================================================
//  kernel/vfs.c -- Arborescence de fichiers en mémoire (ramfs)
// =============================================================================
#include "vfs.h"
#include "heap.h"
#include "klib.h"

static vfs_node_t *root;

static vfs_node_t *node_new(const char *name, vfs_type_t type) {
    vfs_node_t *n = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    if (!n) return NULL;
    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->type = type;
    return n;
}

// Insère 'child' à la fin de la liste des enfants de 'dir'.
static void attach(vfs_node_t *dir, vfs_node_t *child) {
    child->parent = dir;
    child->next = NULL;
    if (!dir->children) { dir->children = child; return; }
    vfs_node_t *c = dir->children;
    while (c->next) c = c->next;
    c->next = child;
}

vfs_node_t *vfs_lookup(vfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIR) return NULL;
    for (vfs_node_t *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

vfs_node_t *vfs_create(vfs_node_t *dir, const char *name, vfs_type_t type) {
    if (!dir || dir->type != VFS_DIR) return NULL;
    if (name[0] == 0 || vfs_lookup(dir, name)) return NULL;   // déjà existant
    vfs_node_t *n = node_new(name, type);
    if (!n) return NULL;
    attach(dir, n);
    return n;
}

bool vfs_delete(vfs_node_t *node) {
    if (!node || !node->parent) return false;     // pas la racine
    // Supprime récursivement les enfants.
    vfs_node_t *c = node->children;
    while (c) { vfs_node_t *nx = c->next; vfs_delete(c); c = nx; }
    // Détache de la liste du parent.
    vfs_node_t *p = node->parent;
    if (p->children == node) {
        p->children = node->next;
    } else {
        for (vfs_node_t *s = p->children; s; s = s->next)
            if (s->next == node) { s->next = node->next; break; }
    }
    if (node->data) kfree(node->data);
    kfree(node);
    return true;
}

bool vfs_rename(vfs_node_t *node, const char *newname) {
    if (!node || !node->parent || newname[0] == 0) return false;
    if (vfs_lookup(node->parent, newname)) return false;
    strncpy(node->name, newname, VFS_NAME_MAX - 1);
    node->name[VFS_NAME_MAX - 1] = 0;
    return true;
}

bool vfs_move(vfs_node_t *node, vfs_node_t *newdir) {
    if (!node || !node->parent || !newdir || newdir->type != VFS_DIR) return false;
    if (vfs_lookup(newdir, node->name)) return false;
    // Empêche de déplacer un dossier dans lui-même ou un descendant.
    for (vfs_node_t *d = newdir; d; d = d->parent)
        if (d == node) return false;
    // Détache de l'ancien parent.
    vfs_node_t *p = node->parent;
    if (p->children == node) p->children = node->next;
    else for (vfs_node_t *s = p->children; s; s = s->next)
            if (s->next == node) { s->next = node->next; break; }
    attach(newdir, node);
    return true;
}

static bool ensure_capacity(vfs_node_t *f, size_t need) {
    if (f->capacity >= need) return true;
    size_t cap = f->capacity ? f->capacity : 64;
    while (cap < need) cap *= 2;
    uint8_t *nd = (uint8_t *)krealloc(f->data, cap);
    if (!nd) return false;
    f->data = nd;
    f->capacity = cap;
    return true;
}

int vfs_read(vfs_node_t *file, size_t off, void *buf, size_t len) {
    if (!file || file->type != VFS_FILE) return -1;
    if (off >= file->size) return 0;
    if (off + len > file->size) len = file->size - off;
    memcpy(buf, file->data + off, len);
    return (int)len;
}

int vfs_write(vfs_node_t *file, size_t off, const void *buf, size_t len) {
    if (!file || file->type != VFS_FILE) return -1;
    if (!ensure_capacity(file, off + len)) return -1;
    memcpy(file->data + off, buf, len);
    if (off + len > file->size) file->size = off + len;
    return (int)len;
}

bool vfs_set_contents(vfs_node_t *file, const char *text) {
    if (!file || file->type != VFS_FILE) return false;
    size_t len = strlen(text);
    if (!ensure_capacity(file, len)) return false;
    memcpy(file->data, text, len);
    file->size = len;
    return true;
}

// Remplace TOUT le contenu d'un fichier (tronque à 'len' octets, binaire-safe).
//  Utilisé par l'éditeur (sauvegarde) : vfs_write ne fait que grandir le fichier.
int vfs_replace(vfs_node_t *file, const void *buf, size_t len) {
    if (!file || file->type != VFS_FILE) return -1;
    if (!ensure_capacity(file, len)) return -1;
    if (len) memcpy(file->data, buf, len);
    file->size = len;
    return (int)len;
}

vfs_node_t *vfs_resolve(const char *path) {
    if (!path || path[0] != '/') return NULL;
    vfs_node_t *cur = root;
    char comp[VFS_NAME_MAX];
    size_t i = 1;
    while (path[i]) {
        size_t j = 0;
        while (path[i] && path[i] != '/' && j < VFS_NAME_MAX - 1) comp[j++] = path[i++];
        comp[j] = 0;
        while (path[i] == '/') i++;
        if (j == 0) continue;
        cur = vfs_lookup(cur, comp);
        if (!cur) return NULL;
    }
    return cur;
}

void vfs_path(vfs_node_t *node, char *buf, size_t buflen) {
    if (!node) { strncpy(buf, "/", buflen); return; }
    // Construit le chemin en remontant les parents.
    char tmp[512];
    tmp[0] = 0;
    if (node == root) { strncpy(buf, "/", buflen); return; }
    char parts[16][VFS_NAME_MAX];
    int n = 0;
    for (vfs_node_t *c = node; c && c != root && n < 16; c = c->parent)
        strncpy(parts[n++], c->name, VFS_NAME_MAX);
    size_t pos = 0;
    for (int k = n - 1; k >= 0; k--) {
        tmp[pos++] = '/';
        for (size_t m = 0; parts[k][m] && pos < sizeof(tmp) - 1; m++) tmp[pos++] = parts[k][m];
    }
    tmp[pos] = 0;
    strncpy(buf, tmp, buflen);
}

int vfs_count_children(vfs_node_t *dir) {
    if (!dir || dir->type != VFS_DIR) return 0;
    int n = 0;
    for (vfs_node_t *c = dir->children; c; c = c->next) n++;
    return n;
}

// -----------------------------------------------------------------------------
//  Arborescence initiale
// -----------------------------------------------------------------------------
void vfs_init(void) {
    root = node_new("", VFS_DIR);

    vfs_node_t *home = vfs_create(root, "home", VFS_DIR);
    vfs_node_t *user = vfs_create(home, "user", VFS_DIR);
    vfs_node_t *docs = vfs_create(user, "Documents", VFS_DIR);
    vfs_create(user, "Images", VFS_DIR);

    // Dossier SSH : .ssh/authorized_keys (vide) — pret a recevoir des cles
    //  publiques (commande "pubkey-add" du terminal ou du shell distant).
    vfs_node_t *dotssh = vfs_create(user, ".ssh", VFS_DIR);
    vfs_create(dotssh, "authorized_keys", VFS_FILE);

    vfs_node_t *f;
    f = vfs_create(user, "bienvenue.sex", VFS_FILE);
    vfs_set_contents(f,
        "Bienvenue dans sexOs v2 !\n\n"
        "Ceci est un fichier texte stocke dans le systeme de fichiers.\n"
        "Utilisez l'explorateur pour naviguer, et l'editeur pour modifier.\n\n"
        "Mascotte :\n  D\n  |\n  |\n  8\n");

    f = vfs_create(docs, "notes.sex", VFS_FILE);
    vfs_set_contents(f, "Liste de choses a faire :\n - tester les fenetres\n - ouvrir les parametres\n");

    f = vfs_create(docs, "lisez-moi.sex", VFS_FILE);
    vfs_set_contents(f, "sexOs est un systeme d'exploitation pedagogique ecrit en C et assembleur.\n");

    // Mini-site d'exemple servi par httpd : http://<ip>/site/
    vfs_node_t *site = vfs_create(user, "site", VFS_DIR);
    f = vfs_create(site, "index.html", VFS_FILE);
    vfs_set_contents(f,
        "<!doctype html><html lang=fr><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Mon site sexOs</title><style>"
        "body{margin:0;font-family:system-ui,monospace;background:#0f1320;color:#e6ecf2}"
        ".hero{padding:60px 24px;text-align:center;background:linear-gradient(135deg,#1a1030,#102030)}"
        "h1{font-size:42px;margin:0;color:#ff7ab0}p{color:#9aa6b4}"
        ".card{max-width:680px;margin:24px auto;padding:20px;background:#161b2b;border:1px solid #2d3446;border-radius:10px}"
        "a{color:#6ee79a}code{background:#0b0e16;padding:2px 6px;border-radius:4px;color:#7cf06a}</style></head><body>"
        "<div class=hero><h1>Bienvenue sur sexOs</h1>"
        "<p>Ce site est servi par le serveur web du noyau (httpd), depuis la RAM.</p></div>"
        "<div class=card><h2>C'est en direct !</h2>"
        "<p>Cette page est dans <code>/home/user/site/index.html</code>.</p>"
        "<p>Modifie-la avec <code>nano site/index.html</code> dans le terminal, "
        "puis recharge la page.</p>"
        "<p>Retour a l'explorateur de fichiers : <a href=\"/\">/</a></p></div>"
        "</body></html>\n");

    vfs_node_t *sys = vfs_create(root, "systeme", VFS_DIR);
    f = vfs_create(sys, "version.sex", VFS_FILE);
    vfs_set_contents(f, "sexOs version 2.0\nNoyau x86_64, demarrage UEFI/BIOS via Limine.\n");

    kprintf("[vfs] arborescence en memoire prete\n");
}

vfs_node_t *vfs_root(void) { return root; }

// Supprime toute l'arborescence (tous les enfants de la racine). Utilise par la
// persistance avant de recharger un instantane depuis le disque.
void vfs_reset(void) {
    if (!root) return;
    while (root->children) vfs_delete(root->children);
}
