// =============================================================================
//  kernel/users.h -- Comptes utilisateurs, authentification, privilèges
// =============================================================================
#ifndef SEXOS_USERS_H
#define SEXOS_USERS_H

#include <stdint.h>
#include <stdbool.h>

#define USER_NAME_MAX 32

typedef struct {
    char     name[USER_NAME_MAX];
    uint32_t pass_hash;
    bool     is_admin;
    char     home[64];        // dossier personnel (ex: "/home/user")
} user_t;

void          users_init(void);
int           users_count(void);
const user_t *users_get(int index);

// Authentification : renvoie le compte si (nom, mot de passe) correspond.
const user_t *users_authenticate(const char *name, const char *password);

void          users_set_current(const user_t *u);
const user_t *users_current(void);
bool          users_is_admin(void);

// Politique de privilèges.
bool users_can_admin(void);                  // actions système réservées admin
bool users_can_write_path(const char *path); // écriture autorisée à ce chemin ?

#endif
