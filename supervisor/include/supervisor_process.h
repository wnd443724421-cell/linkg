/**
 * @file supervisor_process.h
 * @brief Supervisor独立进程管理接口
 */

#ifndef SUPERVISOR_PROCESS_H
#define SUPERVISOR_PROCESS_H

#include "supervisor_release.h"

/****************************** 生命周期 ******************************/

int supervisor_process_run(char *const child_arguments[], const char *lock_path);
int supervisor_process_manage(const supervisor_release_paths_t *paths, const char *lock_path);

#endif
