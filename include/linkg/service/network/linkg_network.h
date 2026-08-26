/**
 * @file linkg_network.h
 * @brief LinkG网络服务接口
 */

#ifndef LINKG_NETWORK_H
#define LINKG_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_NETWORK_STATE_UNINITIALIZED = 0, // 网络模块未初始化
    LINKG_NETWORK_STATE_STOPPED,           // 网络模块已停止
    LINKG_NETWORK_STATE_STARTING,          // 网络模块正在启动
    LINKG_NETWORK_STATE_RUNNING,           // 网络模块正在运行
    LINKG_NETWORK_STATE_STOPPING,          // 网络模块正在停止
    LINKG_NETWORK_STATE_FAILED             // 网络模块运行失败
} linkg_network_state_t;

/****************************** 生命周期 ******************************/

int linkg_network_init(void);
int linkg_network_start(void);
int linkg_network_stop(void);
int linkg_network_deinit(void);

/****************************** 状态查询 ******************************/

linkg_network_state_t linkg_network_get_state(void);

#ifdef __cplusplus
}
#endif

#endif
