/**
 * @file linkg_lifecycle.h
 * @brief LinkG应用生命周期管理接口
 */

#ifndef LINKG_LIFECYCLE_H
#define LINKG_LIFECYCLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_EXIT_ACTION_NONE             = 0, // 未请求退出
    LINKG_EXIT_ACTION_EXIT,                 // 正常退出应用
    LINKG_EXIT_ACTION_RESTART_SERVICES,     // 重载配置并重启服务
    LINKG_EXIT_ACTION_REBOOT_SYSTEM         // 重新启动操作系统
} linkg_exit_action_t;                      // 应用退出动作

/****************************** 生命周期 ******************************/

int  linkg_lifecycle_init(void);
int  linkg_lifecycle_reset(void);
void linkg_lifecycle_deinit(void);

/****************************** 退出请求 ******************************/

int linkg_lifecycle_request_exit(linkg_exit_action_t action);
linkg_exit_action_t linkg_lifecycle_wait_for_exit(void);

/****************************** 状态查询 ******************************/

bool linkg_lifecycle_is_exit_requested(void);
linkg_exit_action_t linkg_lifecycle_get_exit_action(void);

#ifdef __cplusplus
}
#endif

#endif // LINKG_LIFECYCLE_H
