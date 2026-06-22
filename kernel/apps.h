// =============================================================================
//  kernel/apps.h -- Applications du bureau
// =============================================================================
#ifndef SEXOS_APPS_H
#define SEXOS_APPS_H

#include "vfs.h"

void app_terminal_open(void);
void app_files_open(void);
void app_settings_open(void);
void app_about_open(void);
void app_editor_open(vfs_node_t *file);

#endif
