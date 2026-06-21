// =============================================================================
//  kernel/vmware.c -- Canal « backdoor » VMware (port 0x5658) : presse-papiers
//  partagé hôte <-> invité (texte), via les commandes historiques GETSEL/SETSEL.
// -----------------------------------------------------------------------------
//  Le backdoor VMware se pilote par une instruction `in` spéciale : on charge
//  EAX = magie 'VMXh', ECX = commande, EDX = port 0x5658, puis l'hyperviseur
//  intercepte l'accès et renseigne EAX/EBX/ECX/EDX. Hors VMware (QEMU, matériel
//  réel), le port n'est pas intercepté : EBX reste inchangé -> non détecté.
// =============================================================================
#include "vmware.h"
#include "clip.h"
#include "klib.h"
#include "sched.h"

#define VMW_MAGIC   0x564D5868u      // 'VMXh'
#define VMW_PORT    0x5658u
#define CMD_GETVERSION   10u
#define CMD_GETSELLEN     6u         // longueur du presse-papiers hôte (EAX)
#define CMD_GETNEXT       7u         // 4 octets suivants (EAX)
#define CMD_SETSELLEN     8u         // fixe la longueur invité->hôte (EBX)
#define CMD_SETNEXT       9u         // 4 octets suivants (EBX)

// Appel backdoor : tous les registres sont entrée ET sortie (l'hyperviseur les
// réécrit). `in %dx,%eax` ne modifie normalement qu'EAX : les contraintes "+b"
// et "+c" signalent à GCC que l'instruction (interceptée) peut aussi les changer.
static inline void bd(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t a = *eax, b = *ebx, c = *ecx, d = *edx;
    __asm__ volatile ("in %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d));
    *eax = a; *ebx = b; *ecx = c; *edx = d;
}

bool vmware_present(void) {
    uint32_t eax = VMW_MAGIC, ebx = ~VMW_MAGIC, ecx = CMD_GETVERSION, edx = VMW_PORT;
    bd(&eax, &ebx, &ecx, &edx);
    return ebx == VMW_MAGIC;
}

// Lit le presse-papiers hôte. Renvoie : n>0 (nouveau contenu de n octets),
// 0 (vide) ou -1 (pas de nouveauté depuis la dernière lecture).
static int host_get(char *out, int max) {
    uint32_t eax = VMW_MAGIC, ebx = 0, ecx = CMD_GETSELLEN, edx = VMW_PORT;
    bd(&eax, &ebx, &ecx, &edx);
    uint32_t len = eax;
    if (len == 0xFFFFFFFFu) return -1;           // rien de neuf
    if (len == 0) return 0;
    // Commande non supportée par l'hôte : le backdoor renvoie la magie (ou une
    // valeur aberrante) dans EAX. On ne l'interprète pas comme une longueur.
    if (len == VMW_MAGIC || len > 65535u) return -1;
    if (len > (uint32_t)max) len = (uint32_t)max;
    uint32_t got = 0;
    while (got < len) {
        uint32_t a = VMW_MAGIC, b = 0, c = CMD_GETNEXT, d = VMW_PORT;
        bd(&a, &b, &c, &d);
        for (int i = 0; i < 4 && got < len; i++) { out[got++] = (char)(a & 0xff); a >>= 8; }
    }
    return (int)len;
}

// Écrit le presse-papiers invité vers l'hôte.
static void host_set(const char *buf, int len) {
    uint32_t eax = VMW_MAGIC, ebx = (uint32_t)len, ecx = CMD_SETSELLEN, edx = VMW_PORT;
    bd(&eax, &ebx, &ecx, &edx);
    for (int i = 0; i < len; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4 && i + j < len; j++) v |= ((uint32_t)(uint8_t)buf[i + j]) << (8 * j);
        uint32_t a = VMW_MAGIC, b = v, c = CMD_SETNEXT, d = VMW_PORT;
        bd(&a, &b, &c, &d);
    }
}

// Tâche noyau : synchronise dans les deux sens, sans boucle de retour (on note
// la génération adoptée pour ne pas réémettre vers l'hôte ce qu'il vient d'envoyer).
static void vmclip_task(void) {
    static char hbuf[CLIP_CAP];
    static char gbuf[CLIP_CAP];
    uint32_t last_gen = clip_kgen();
    for (;;) {
        int n = host_get(hbuf, sizeof hbuf);          // hôte -> invité
        if (n > 0) { clip_kset(hbuf, n); last_gen = clip_kgen(); }
        else if (clip_kgen() != last_gen) {           // invité -> hôte
            int gl = clip_kget(gbuf, sizeof gbuf);
            host_set(gbuf, gl);
            last_gen = clip_kgen();
        }
        sched_sleep_ms(300);
    }
}

void vmware_init(void) {
    if (!vmware_present()) {
        kprintf("[vmware] non detecte (presse-papiers hote non partage)\n");
        return;
    }
    // Sonde la prise en charge du presse-papiers backdoor : un hôte qui ne le
    // gère pas renvoie la magie dans EAX (cas de QEMU, par ex.).
    uint32_t eax = VMW_MAGIC, ebx = 0, ecx = CMD_GETSELLEN, edx = VMW_PORT;
    bd(&eax, &ebx, &ecx, &edx);
    if (eax == VMW_MAGIC)
        kprintf("[vmware] detecte, mais presse-papiers backdoor non supporte par cet hote\n");
    else
        kprintf("[vmware] detecte : presse-papiers partage hote<->invite actif\n");
    sched_new_kernel_task("vmclip", vmclip_task);   // inoffensive si non supporte
}
