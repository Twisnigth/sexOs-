// =============================================================================
//  kernel/users.c -- Comptes, authentification et séparation des privilèges
// -----------------------------------------------------------------------------
//  NOTE de sécurité : les mots de passe sont "hachés" par un simple djb2, ce
//  qui n'a AUCUNE valeur cryptographique. C'est suffisant pour démontrer la
//  séparation des privilèges dans un cadre pédagogique, pas pour un usage réel.
// =============================================================================
#include "users.h"
#include "klib.h"

#define MAX_USERS 8

static user_t users[MAX_USERS];
static int user_count;
static const user_t *current;

static uint32_t hash_pw(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = ((h << 5) + h) + (uint8_t)*s;   // djb2
    return h;
}

static void add_user(const char *name, const char *pw, bool admin, const char *home) {
    if (user_count >= MAX_USERS) return;
    user_t *u = &users[user_count++];
    strncpy(u->name, name, USER_NAME_MAX - 1);
    u->pass_hash = hash_pw(pw);
    u->is_admin = admin;
    strncpy(u->home, home, sizeof(u->home) - 1);
}

void users_init(void) {
    user_count = 0;
    current = NULL;
    // Comptes par défaut.
    add_user("root", "root", true,  "/home");
    add_user("user", "user", false, "/home/user");
    kprintf("[users] %d comptes (root=admin, user=standard)\n", user_count);
}

int users_count(void) { return user_count; }
const user_t *users_get(int i) {
    return (i >= 0 && i < user_count) ? &users[i] : NULL;
}

const user_t *users_authenticate(const char *name, const char *password) {
    uint32_t h = hash_pw(password);
    for (int i = 0; i < user_count; i++)
        if (strcmp(users[i].name, name) == 0 && users[i].pass_hash == h)
            return &users[i];
    return NULL;
}

void users_set_current(const user_t *u) { current = u; }
const user_t *users_current(void) { return current; }

// Change le mot de passe de l'utilisateur courant après vérification de l'ancien.
bool users_change_password(const char *oldpw, const char *newpw) {
    if (!current) return false;
    if (current->pass_hash != hash_pw(oldpw)) return false;
    for (int i = 0; i < user_count; i++)
        if (&users[i] == current) { users[i].pass_hash = hash_pw(newpw); return true; }
    return false;
}
bool users_is_admin(void) { return current && current->is_admin; }
bool users_can_admin(void) { return users_is_admin(); }

bool users_can_write_path(const char *path) {
    if (!current) return false;
    if (current->is_admin) return true;            // l'admin écrit partout
    // Les supports amovibles montés sous /media (clé USB) sont accessibles à tous.
    if (strncmp(path, "/media/", 7) == 0) return true;
    // Un utilisateur standard n'écrit que dans son dossier personnel.
    size_t n = strlen(current->home);
    return strncmp(path, current->home, n) == 0 &&
           (path[n] == 0 || path[n] == '/');
}
