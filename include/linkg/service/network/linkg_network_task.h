/**
 * @file linkg_network_task.h
 * @brief LinkG Wi-Fi独占维护任务接口
 */

#ifndef LINKG_NETWORK_TASK_H
#define LINKG_NETWORK_TASK_H

#include <stdbool.h>

#include "linkg_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef int (*linkg_network_wifi_task_run_t)(linkg_thread_t *thread, void *context, bool *released);
typedef void (*linkg_network_wifi_task_finish_t)(void *context, int task_error, int restore_error);

/****************************** 独占维护 ******************************/

int  linkg_network_wifi_task_start(linkg_network_wifi_task_run_t run, linkg_network_wifi_task_finish_t finish, void *context);
int  linkg_network_wifi_task_cancel(void);
bool linkg_network_wifi_task_active(void);

#ifdef __cplusplus
}
#endif

#endif
