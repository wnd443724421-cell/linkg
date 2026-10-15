/**
 * @file linkg_uart_manager.h
 * @brief LinkG串口透传与SBUS服务公共接口
 */

#ifndef LINKG_UART_MANAGER_H
#define LINKG_UART_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_uart_config.h"
#include "linkg_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** SBUS缓冲模式 ******************************/

typedef enum
{
    LINKG_UART_SBUS_PROFILE_WIFI = 0, // Wi-Fi单链路，目标2帧
    LINKG_UART_SBUS_PROFILE_BACKUP    // 冗余或5G，目标5帧
} linkg_uart_sbus_profile_t;

/****************************** 应用状态 ******************************/

typedef enum
{
    LINKG_UART_APPLY_NONE = 0,     // 无待应用变更
    LINKG_UART_APPLY_RUNNING,      // 正在重载
    LINKG_UART_APPLY_APPLIED,      // 运行态和磁盘已提交
    LINKG_UART_APPLY_ROLLED_BACK,  // 应用失败，已恢复原配置
    LINKG_UART_APPLY_FAILED        // 应用失败或回滚未完成
} linkg_uart_apply_state_t;

typedef struct
{
    linkg_uart_apply_state_t state;          // 最后一次任务状态
    uint32_t                 job_id;         // 本进程内的任务编号
    int                      error;          // 应用错误码
    int                      rollback_error; // 回滚错误码，非零时不能声称恢复成功
    uint64_t                 elapsed_ms;     // 完成任务的耗时
} linkg_uart_apply_status_t;

typedef struct
{
    uint8_t                uart_num;                                           // 逻辑串口号
    const char            *device_path;                                        // 静态资源字符串，不转移所有权
    bool                   device_open;                                        // 设备实际打开状态
    bool                   sbus_running;                                       // SBUS线程循环状态
    uint32_t               route_count;                                        // 活动路由数
    const char            *routes[LINKG_UART_CONFIG_ROUTE_COUNT_MAX];          // 原子读取的路由状态字符串
    uint64_t               uart_rx_bytes;                                      // UART累计接收字节
    uint64_t               uart_tx_bytes;                                      // UART累计提交字节
    uint64_t               net_rx_bytes;                                       // 网络累计接收字节
    uint64_t               net_tx_bytes;                                       // 网络累计提交字节
    uint64_t               errors;                                             // 累计IO错误次数
    uint64_t               drops;                                              // 累计丢弃数据块或帧次数
    bool                   baud_verified;                                      // 当前设备驱动分频信息有效
    linux_uart_baud_info_t baud;                                               // 计算得到的实际波特率
} linkg_uart_port_status_t;

typedef struct
{
    const char              *state;                                           // 生命周期名称，静态字符串
    bool                     worker_running;                                  // Worker是否仍在执行
    uint32_t                 port_count;                                      // 活动上下文数量，不等于已配置端口数
    linkg_uart_port_status_t ports[LINKG_UART_CONFIG_PORT_COUNT_MAX];         // 线程安全的计数和状态采样
} linkg_uart_status_t;

/****************************** 生命周期 ******************************/

int linkg_uart_manager_init(void);
int linkg_uart_manager_start(void);
int linkg_uart_manager_stop(void);
int linkg_uart_manager_deinit(void);

/****************************** 运行配置 ******************************/

int linkg_uart_manager_set_sbus_profile(linkg_uart_sbus_profile_t profile);
int linkg_uart_manager_set_debug(bool enabled);

/****************************** 配置应用 ******************************/

int         linkg_uart_manager_apply(const linkg_uart_config_t *config, uint32_t *job_id);
int         linkg_uart_manager_get_config(linkg_uart_config_t *out);
int         linkg_uart_manager_get_apply_status(linkg_uart_apply_status_t *out);
int         linkg_uart_manager_get_status(linkg_uart_status_t *out);
const char *linkg_uart_manager_device_path(uint8_t uart_num);

#ifdef __cplusplus
}
#endif

#endif
