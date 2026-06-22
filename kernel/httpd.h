// =============================================================================
//  kernel/httpd.h -- Serveur web minimal (HTTP/1.0, port 80)
// =============================================================================
#ifndef SEXOS_HTTPD_H
#define SEXOS_HTTPD_H

void httpd_init(void);   // ouvre le port 80 en ecoute (appeler apres le DHCP)
void httpd_run(void);    // tache noyau : sert les fichiers du VFS (sched_new_kernel_task)

#endif
