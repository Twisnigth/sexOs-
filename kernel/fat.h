// =============================================================================
//  kernel/fat.h -- Pilote FAT32 (lecture + ecriture) pour cles USB / disques
//  formates sous Windows/Mac. Permet l'echange de fichiers entre sexOs et les
//  autres OS sur le meme support.
// =============================================================================
#ifndef SEXOS_FAT_H
#define SEXOS_FAT_H

#include <stdint.h>
#include <stdbool.h>

typedef bool (*fat_rd_t)(uint32_t lba, uint32_t count, void *buf);
typedef bool (*fat_wr_t)(uint32_t lba, uint32_t count, const void *buf);

struct vfs_node;

// Detecte un FAT32 sur le support et, si valide, charge son arborescence dans
// le noeud VFS 'mount' (avec le contenu des fichiers). Renvoie true si monte.
bool fat_mount(fat_rd_t rd, fat_wr_t wr, struct vfs_node *mount);

bool fat_active(void);   // un volume FAT est-il monte ?

// Operations d'ecriture (le chemin est RELATIF a la racine du volume, ex.
// "dossier/fichier.txt"). Renvoient 0 si ok, -1 sinon.
int  fat_create(const char *relpath, bool is_dir);          // cree fichier/dossier vide
int  fat_write(const char *relpath, const void *data, uint32_t len); // (re)ecrit un fichier
int  fat_delete(const char *relpath);                       // supprime fichier/dossier (vide)

#endif
