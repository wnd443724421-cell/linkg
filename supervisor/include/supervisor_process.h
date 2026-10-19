/**
 * @file supervisor_process.h
 * @brief Supervisor独立进程管理接口
 */

#ifndef SUPERVISOR_PROCESS_H
#define SUPERVISOR_PROCESS_H

/****************************** 生命周期 ******************************/

int supervisor_process_run(char *const child_arguments[], const char *lock_path);

#endif
