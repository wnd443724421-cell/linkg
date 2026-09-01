#ifndef LINKG_CELLULAR_H
#define LINKG_CELLULAR_H
#include "linkg_cellular_config.h"
#include "linkg_thread.h"
int linkg_cellular_init(const linkg_cellular_config_t *config);
int linkg_cellular_start(void);
int linkg_cellular_run(linkg_thread_t *owner_thread);
int linkg_cellular_stop(void);
int linkg_cellular_deinit(void);
#endif
