// =============================================================================
//  kernel/vfs.h -- Système de fichiers virtuel (arborescence en mémoire)
// -----------------------------------------------------------------------------
//  Fournit une arborescence fichiers/dossiers avec lecture/écriture et les
//  opérations nécessaires à l'explorateur (créer, supprimer, renommer, déplacer).
// =============================================================================
#ifndef SEXOS_VFS_H
#define SEXOS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VFS_NAME_MAX 64

typedef enum { VFS_FILE, VFS_DIR } vfs_type_t;

typedef struct vfs_node {
    char              name[VFS_NAME_MAX];
    vfs_type_t        type;
    struct vfs_node  *parent;
    struct vfs_node  *children;   // premier enfant (si dossier)
    struct vfs_node  *next;       // frère suivant
    uint8_t          *data;       // contenu (si fichier)
    size_t            size;
    size_t            capacity;
} vfs_node_t;

void        vfs_init(void);
vfs_node_t *vfs_root(void);

vfs_node_t *vfs_lookup(vfs_node_t *dir, const char *name);
vfs_node_t *vfs_create(vfs_node_t *dir, const char *name, vfs_type_t type);
bool        vfs_delete(vfs_node_t *node);
bool        vfs_rename(vfs_node_t *node, const char *newname);
bool        vfs_move(vfs_node_t *node, vfs_node_t *newdir);

int  vfs_read(vfs_node_t *file, size_t off, void *buf, size_t len);
int  vfs_write(vfs_node_t *file, size_t off, const void *buf, size_t len);
int  vfs_replace(vfs_node_t *file, const void *buf, size_t len);  // remplace tout (tronque)
bool vfs_set_contents(vfs_node_t *file, const char *text);  // remplace par une chaîne

vfs_node_t *vfs_resolve(const char *path);                  // chemin absolu "/a/b"
void        vfs_path(vfs_node_t *node, char *buf, size_t buflen);
int         vfs_count_children(vfs_node_t *dir);

#endif
