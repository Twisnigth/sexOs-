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
#include "ahci.h"
#include "vfs.h"
#include "usb.h"
#include "fat.h"
#include "klib.h"

#define FS_MAGIC "SEXOSFS1"
#define FS_CAP   (1024 * 1024)        // 1 Mio d'instantane (suffisant pour des textes)

static uint8_t image[FS_CAP];

// --- couche disque : IDE « legacy » + SATA AHCI (VM modernes) ----------------
//  Le disque SYSTEME (racine /) est l'IDE s'il existe, sinon le SATA. Si les
//  DEUX existent, le SATA est monte en plus comme volume (/media/disk).
static int  disk_kind;                 // disque systeme : 0=aucun, 1=IDE, 2=AHCI
static bool ahci_extra;                // un disque SATA est present EN PLUS du systeme
static bool disk_init(void) {
    bool ide  = ata_init();            // IDE primaire (0x1F0)
    bool sata = ahci_init();           // SATA AHCI (q35, VMware...)
    if (ide)       { disk_kind = 1; ahci_extra = sata; }   // IDE = racine, SATA en plus
    else if (sata) { disk_kind = 2; ahci_extra = false; }  // SATA = racine
    else           { disk_kind = 0; return false; }
    return true;
}
static bool disk_ok(void)    { return disk_kind != 0; }
static bool disk_read(uint32_t l, uint32_t c, void *b) {
    return disk_kind == 1 ? ata_read(l, c, b) : disk_kind == 2 ? ahci_read(l, c, b) : false;
}
static bool disk_write(uint32_t l, uint32_t c, const void *b) {
    return disk_kind == 1 ? ata_write(l, c, b) : disk_kind == 2 ? ahci_write(l, c, b) : false;
}

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
bool fs_present(void) { return disk_ok(); }

int fs_save(void) {
    if (!disk_ok()) return -1;
    size_t n = fs_serialize(image);
    uint32_t secs = (n + 511) / 512;
    return disk_write(0, secs, image) ? 0 : -1;
}

static int fs_load(void) {
    if (!disk_ok()) return -1;
    if (!disk_read(0, 1, image)) return -1;
    if (memcmp(image, FS_MAGIC, 8) != 0) return -1;
    uint32_t total = gu32(image, 8);
    if (total < 16 || total > FS_CAP) return -1;
    uint32_t secs = (total + 511) / 512;
    if (!disk_read(0, secs, image)) return -1;
    vfs_reset();                                    // efface l'arborescence par defaut
    size_t o = 12; uint32_t cnt = gu32(image, o); o += 4;
    for (uint32_t i = 0; i < cnt; i++) o = deser_node(vfs_root(), image, o, total);
    return 0;
}

void fs_init(void) {
    if (!disk_init()) {
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
//  Volumes montes sous /media (cle USB, disque SATA supplementaire...)
// -----------------------------------------------------------------------------
//  Chaque volume porte son propre instantane SEXOSFS1 sur son support bloc.
//  On retient pour chacun ses fonctions de lecture/ecriture de secteurs, ce qui
//  permet de router la persistance vers le bon support selon le chemin.
// =============================================================================
typedef bool (*blk_rd_t)(uint32_t, uint32_t, void *);
typedef bool (*blk_wr_t)(uint32_t, uint32_t, const void *);

#define MAX_VOL 4
typedef struct {
    bool     used;
    bool     fat;                     // true = FAT32 (ecriture incrementale + Windows)
    char     mp[40];                  // point de montage, ex. "/media/usb"
    blk_rd_t rd;
    blk_wr_t wr;
} vol_t;
static vol_t vols[MAX_VOL];
static bool  any_fat;                 // un volume FAT32 est-il deja monte ? (un seul gere)

static vfs_node_t *media_child(const char *name, bool create) {
    vfs_node_t *media = vfs_lookup(vfs_root(), "media");
    if (!media && create) media = vfs_create(vfs_root(), "media", VFS_DIR);
    if (!media) return NULL;
    vfs_node_t *n = vfs_lookup(media, name);
    if (!n && create) n = vfs_create(media, name, VFS_DIR);
    return n;
}

static int vol_write_node(vol_t *v, vfs_node_t *node) {
    memcpy(image, FS_MAGIC, 8);
    size_t o = 12;
    uint32_t cnt = 0; for (vfs_node_t *c = node->children; c; c = c->next) cnt++;
    o = pu32(image, o, cnt);
    for (vfs_node_t *c = node->children; c; c = c->next) o = ser_node(c, image, o);
    pu32(image, 8, (uint32_t)o);
    uint32_t secs = (o + 511) / 512;
    return v->wr(0, secs, image) ? 0 : -1;
}

static const char *vol_relpath(vol_t *v, const char *path) {
    int n = (int)strlen(v->mp);
    return (path[n] == '/') ? path + n + 1 : path + n;   // "" si path == point de montage
}

// Monte un peripherique bloc sous /media/<name> : FAT32 si reconnu (lisible
// Windows), format interne sexOs si reconnu, sinon formate le support vierge en
// FAT32 (cross-platform par defaut). 'nsec' = nombre de secteurs du support.
static void vol_mount(const char *name, blk_rd_t rd, blk_wr_t wr, const char *label, uint32_t nsec) {
    vfs_node_t *node = media_child(name, true);
    if (!node) return;
    vol_t *v = NULL;
    for (int i = 0; i < MAX_VOL; i++) if (!vols[i].used) { v = &vols[i]; break; }
    if (!v) return;
    v->used = true; v->fat = false; v->rd = rd; v->wr = wr;
    strcpy(v->mp, "/media/"); strcat(v->mp, name);

    // 1) FAT32 deja present (format universel) : un seul volume FAT a la fois
    if (!any_fat && fat_mount(rd, wr, node)) {
        v->fat = true; any_fat = true;
        kprintf("[mount] %s monte sur %s (FAT32, lisible Windows/Mac)\n", label, v->mp);
        return;
    }

    // 2) format interne sexOs (SEXOSFS1) deja present
    bool sig = rd(0, 1, image);
    if (sig && memcmp(image, FS_MAGIC, 8) == 0) {
        uint32_t total = gu32(image, 8);
        if (total >= 16 && total <= FS_CAP && rd(0, (total + 511) / 512, image)) {
            size_t o = 12; uint32_t cnt = gu32(image, o); o += 4;
            for (uint32_t i = 0; i < cnt && o < total; i++) o = deser_node(node, image, o, total);
            kprintf("[mount] %s monte sur %s (%d entree(s), format sexOs)\n", label, v->mp, cnt);
            return;
        }
    }
    // SECURITE : ne JAMAIS ecraser un support deja formate qu'on ne sait pas lire
    // (exFAT, NTFS, FAT non monte...). On preserve les donnees.
    if (sig && image[510] == 0x55 && image[511] == 0xAA) {
        v->used = false;
        kprintf("[mount] %s : format non reconnu (donnees preservees, non monte)\n", label);
        return;
    }
    // 3) support vierge : on le formate en FAT32 (cross-platform par defaut)
    if (!any_fat && nsec >= 70000 && fat_format(rd, wr, nsec) == 0 && fat_mount(rd, wr, node)) {
        v->fat = true; any_fat = true;
        kprintf("[mount] %s formate en FAT32 et monte sur %s (lisible Windows/Mac)\n", label, v->mp);
        return;
    }
    // 4) repli : format interne sexOs
    if (vol_write_node(v, node) == 0) kprintf("[mount] %s formate (sexOs) et monte sur %s\n", label, v->mp);
    else { v->used = false; kprintf("[mount] %s : montage impossible (E/S)\n", label); }
}

static vol_t *vol_for_path(const char *p) {
    if (!p) return NULL;
    for (int i = 0; i < MAX_VOL; i++) {
        if (!vols[i].used) continue;
        const char *m = vols[i].mp; int j = 0;
        while (m[j] && p[j] == m[j]) j++;
        if (!m[j] && (p[j] == 0 || p[j] == '/')) return &vols[i];
    }
    return NULL;
}
static int vol_sync(vol_t *v) {
    vfs_node_t *node = vfs_resolve(v->mp);
    return node ? vol_write_node(v, node) : -1;
}

// Adaptateurs bloc pour la cle USB (usb_msc_* renvoie 0/-1, on veut bool).
static bool usbblk_rd(uint32_t l, uint32_t c, void *b)       { return usb_msc_read(l, c, b) == 0; }
static bool usbblk_wr(uint32_t l, uint32_t c, const void *b) { return usb_msc_write(l, c, b) == 0; }

bool usbfs_mounted(void) { return vol_for_path("/media/usb") != NULL; }

void usbfs_mount(void) {     // monte la cle USB (appele aussi au branchement a chaud)
    if (usb_msc_present() && !usbfs_mounted())
        vol_mount("usb", usbblk_rd, usbblk_wr, "cle USB", usb_msc_blocks());
}

// Reformate la cle USB en FAT32 (efface tout), puis la remonte.
int usbfs_format(void) {
    if (!usb_msc_present()) return -1;
    usbfs_unmount();
    if (fat_format(usbblk_rd, usbblk_wr, usb_msc_blocks()) != 0) return -1;
    usbfs_mount();
    return 0;
}

void usbfs_unmount(void) {   // cle retiree a chaud
    for (int i = 0; i < MAX_VOL; i++)
        if (vols[i].used && strcmp(vols[i].mp, "/media/usb") == 0) {
            if (vols[i].fat) any_fat = false;
            vols[i].used = false;
        }
    vfs_node_t *usb = media_child("usb", false);
    if (usb) vfs_delete(usb);
}

int usbfs_sync(void) {
    vol_t *v = vol_for_path("/media/usb");
    return (v && !v->fat) ? vol_sync(v) : 0;   // le FAT ecrit deja a chaque operation
}

// Monte au demarrage les volumes supplementaires (disque SATA + cle USB).
void fs_mount_volumes(void) {
    if (ahci_extra) vol_mount("disk", ahci_read, ahci_write, "disque SATA", ahci_sector_count());
    usbfs_mount();
}

// --- Persistance, routee par chemin (volume FAT, volume sexOs, ou disque) -----
void fs_on_create(const char *path, int is_dir) {
    vol_t *v = vol_for_path(path);
    if (!v) { fs_save(); return; }
    if (v->fat) fat_create(vol_relpath(v, path), is_dir != 0);
    else        vol_sync(v);
}
void fs_on_write(const char *path) {
    vol_t *v = vol_for_path(path);
    if (!v) { fs_save(); return; }
    if (v->fat) {
        vfs_node_t *f = vfs_resolve(path);
        if (f) fat_write(vol_relpath(v, path), f->data ? f->data : (const void *)"", (uint32_t)f->size);
    } else vol_sync(v);
}
void fs_on_delete(const char *path) {
    vol_t *v = vol_for_path(path);
    if (!v) { fs_save(); return; }
    if (v->fat) fat_delete(vol_relpath(v, path));
    else        vol_sync(v);
}

// Force l'ecriture de tout (disque systeme + volumes au format sexOs).
void fs_sync_all(void) {
    fs_save();
    for (int i = 0; i < MAX_VOL; i++) if (vols[i].used && !vols[i].fat) vol_sync(&vols[i]);
}
