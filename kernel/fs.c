// =============================================================================
//  kernel/fs.c -- Persistance du systeme de fichiers (instantane sur disque)
// -----------------------------------------------------------------------------
//  Le VFS vit en RAM. Pour le rendre persistant, on serialise tout l'arbre en
//  un instantane ecrit sur le disque IDE, et on le recharge au demarrage.
//  Format (secteur 0+) : "SEXOSFS1" + u32 taille + u32 nb_enfants_racine +
//  noeuds (recursif). Un noeud : u8 type(0=fichier,1=dossier) + u16 len+nom +
//  (fichier: u32 len+donnees) | (dossier: u32 nb_enfants + enfants).
// =============================================================================
#include "fs.h"
#include "ata.h"
#include "vfs.h"
#include "usb.h"
#include "klib.h"

#define FS_MAGIC "SEXOSFS1"
#define FS_CAP   (1024 * 1024)        // 1 Mio d'instantane (suffisant pour des textes)

static uint8_t image[FS_CAP];

// --- ecriture little-endian --------------------------------------------------
static size_t pu8 (uint8_t *b, size_t o, uint8_t v)  { b[o]=v; return o+1; }
static size_t pu16(uint8_t *b, size_t o, uint16_t v) { b[o]=v; b[o+1]=v>>8; return o+2; }
static size_t pu32(uint8_t *b, size_t o, uint32_t v) { b[o]=v; b[o+1]=v>>8; b[o+2]=v>>16; b[o+3]=v>>24; return o+4; }
static uint32_t gu32(const uint8_t *b, size_t o) { return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|((uint32_t)b[o+3]<<24); }
static uint16_t gu16(const uint8_t *b, size_t o) { return b[o]|(b[o+1]<<8); }

// --- serialisation -----------------------------------------------------------
static size_t ser_node(vfs_node_t *n, uint8_t *b, size_t o) {
    int nl = (int)strlen(n->name);
    if (o + 7 + nl > FS_CAP) return o;
    o = pu8(b, o, n->type == VFS_DIR ? 1 : 0);
    o = pu16(b, o, (uint16_t)nl); memcpy(b + o, n->name, nl); o += nl;
    if (n->type == VFS_DIR) {
        uint32_t cnt = 0; for (vfs_node_t *c = n->children; c; c = c->next) cnt++;
        o = pu32(b, o, cnt);
        for (vfs_node_t *c = n->children; c; c = c->next) o = ser_node(c, b, o);
    } else {
        uint32_t dl = (uint32_t)n->size;
        if (o + 4 + dl > FS_CAP) dl = 0;
        o = pu32(b, o, dl);
        if (dl) { memcpy(b + o, n->data, dl); o += dl; }
    }
    return o;
}
// Le dossier /media (points de montage : cle USB...) est gere separement et
// n'est PAS inclus dans l'instantane du disque IDE.
static bool is_mount_root(vfs_node_t *c) { return strcmp(c->name, "media") == 0; }

static size_t fs_serialize(uint8_t *b) {
    memcpy(b, FS_MAGIC, 8);
    size_t o = 12;                                  // [8 magic][4 taille]
    vfs_node_t *r = vfs_root();
    uint32_t cnt = 0; for (vfs_node_t *c = r->children; c; c = c->next) if (!is_mount_root(c)) cnt++;
    o = pu32(b, o, cnt);
    for (vfs_node_t *c = r->children; c; c = c->next) if (!is_mount_root(c)) o = ser_node(c, b, o);
    pu32(b, 8, (uint32_t)o);                        // taille totale utilisee
    return o;
}

// --- deserialisation ---------------------------------------------------------
static size_t deser_node(vfs_node_t *parent, const uint8_t *b, size_t o, size_t len) {
    if (o >= len) return o;
    uint8_t type = b[o++];
    uint16_t nl = gu16(b, o); o += 2;
    char name[VFS_NAME_MAX]; int cn = nl < VFS_NAME_MAX-1 ? nl : VFS_NAME_MAX-1;
    memcpy(name, b + o, cn); name[cn] = 0; o += nl;
    vfs_node_t *node = vfs_create(parent, name, type ? VFS_DIR : VFS_FILE);
    if (type) {
        uint32_t cnt = gu32(b, o); o += 4;
        for (uint32_t i = 0; i < cnt && o < len; i++) o = deser_node(node, b, o, len);
    } else {
        uint32_t dl = gu32(b, o); o += 4;
        if (node && dl) vfs_replace(node, b + o, dl);
        o += dl;
    }
    return o;
}

// --- disque ------------------------------------------------------------------
bool fs_present(void) { return ata_ok(); }

int fs_save(void) {
    if (!ata_ok()) return -1;
    size_t n = fs_serialize(image);
    uint32_t secs = (n + 511) / 512;
    return ata_write(0, secs, image) ? 0 : -1;
}

static int fs_load(void) {
    if (!ata_ok()) return -1;
    if (!ata_read(0, 1, image)) return -1;
    if (memcmp(image, FS_MAGIC, 8) != 0) return -1;
    uint32_t total = gu32(image, 8);
    if (total < 16 || total > FS_CAP) return -1;
    uint32_t secs = (total + 511) / 512;
    if (!ata_read(0, secs, image)) return -1;
    vfs_reset();                                    // efface l'arborescence par defaut
    size_t o = 12; uint32_t cnt = gu32(image, o); o += 4;
    for (uint32_t i = 0; i < cnt; i++) o = deser_node(vfs_root(), image, o, total);
    return 0;
}

void fs_init(void) {
    if (!ata_init()) {
        kprintf("[fs] aucun disque : systeme de fichiers en RAM (non persistant)\n");
        return;
    }
    if (fs_load() == 0) {
        kprintf("[fs] arborescence restauree depuis le disque\n");
    } else {
        kprintf("[fs] disque vierge : ecriture de l'arborescence initiale\n");
        fs_save();
    }
}

// =============================================================================
//  Cle USB montee sur /media/usb : meme format d'instantane, sur le disque USB
// =============================================================================
static bool usb_mounted;

bool usbfs_mounted(void) { return usb_mounted; }

int usbfs_sync(void) {
    if (!usb_mounted || !usb_msc_present()) return -1;
    vfs_node_t *usb = vfs_resolve("/media/usb");
    if (!usb) return -1;
    memcpy(image, FS_MAGIC, 8);
    size_t o = 12;
    uint32_t cnt = 0; for (vfs_node_t *c = usb->children; c; c = c->next) cnt++;
    o = pu32(image, o, cnt);
    for (vfs_node_t *c = usb->children; c; c = c->next) o = ser_node(c, image, o);
    pu32(image, 8, (uint32_t)o);
    uint32_t secs = (o + 511) / 512;
    return usb_msc_write(0, secs, image) == 0 ? 0 : -1;
}

void usbfs_mount(void) {
    if (!usb_msc_present()) return;
    vfs_node_t *media = vfs_lookup(vfs_root(), "media");
    if (!media) media = vfs_create(vfs_root(), "media", VFS_DIR);
    vfs_node_t *usb = media ? vfs_lookup(media, "usb") : NULL;
    if (!usb && media) usb = vfs_create(media, "usb", VFS_DIR);
    if (!usb) return;
    usb_mounted = true;

    // tente de lire un instantane existant sur la cle
    if (usb_msc_read(0, 1, image) == 0 && memcmp(image, FS_MAGIC, 8) == 0) {
        uint32_t total = gu32(image, 8);
        if (total >= 16 && total <= FS_CAP) {
            uint32_t secs = (total + 511) / 512;
            if (usb_msc_read(0, secs, image) == 0) {
                size_t o = 12; uint32_t cnt = gu32(image, o); o += 4;
                for (uint32_t i = 0; i < cnt && o < total; i++) o = deser_node(usb, image, o, total);
                kprintf("[usbfs] cle USB montee sur /media/usb (%d entree(s))\n", cnt);
                return;
            }
        }
    }
    // cle vierge / non reconnue : on l'initialise avec un instantane vide
    if (usbfs_sync() == 0) kprintf("[usbfs] cle USB formatee et montee sur /media/usb\n");
    else { usb_mounted = false; kprintf("[usbfs] cle USB presente mais montage impossible (E/S)\n"); }
}

// Persiste la mutation selon son chemin : la cle USB pour /media/usb, sinon le
// disque systeme.
static bool path_in_usb(const char *p) {
    const char *m = "/media/usb";
    if (!p) return false;
    for (int i = 0; m[i]; i++) if (p[i] != m[i]) return false;
    return p[10] == 0 || p[10] == '/';
}
int fs_persist(const char *path) {
    if (path_in_usb(path)) return usbfs_sync();
    return fs_save();
}
