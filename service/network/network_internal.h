/**
 * @file network_internal.h
 * @brief LinkG网络服务内部定义
 */

#ifndef NETWORK_INTERNAL_H
#define NETWORK_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>

#include "linkg_cellular_config.h"
#include "linkg_device_config.h"
#include "linkg_network.h"
#include "linkg_network_config.h"
#include "linkg_thread.h"
#include "linkg_wifi_config.h"

/****************************** 网络工作线程 ******************************/

typedef int (*linkg_network_worker_start_func_t)(void);
typedef int (*linkg_network_worker_run_func_t)(linkg_thread_t *thread);
typedef int (*linkg_network_worker_stop_func_t)(void);

typedef struct
{
    linkg_thread_t                    thread;          // 模块管理线程
    linkg_network_worker_start_func_t start;           // 模块启动函数
    linkg_network_worker_run_func_t   run;             // 模块运行函数，NULL表示仅等待停止
    linkg_network_worker_stop_func_t  stop;            // 模块停止函数
    int                               start_result;    // 最近一次模块启动结果
    int                               run_result;      // 最近一次模块运行结果
    int                               stop_result;     // 最近一次模块停止结果
    bool                              start_completed; // 最近一次模块启动是否完成
    bool                              run_completed;   // 最近一次模块运行是否结束
    bool                              initialized;     // 工作线程是否初始化
} linkg_network_worker_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t         lock;                    // 网络服务状态锁
    pthread_cond_t          worker_condition;        // 工作线程状态通知条件变量
    linkg_network_state_t   state;                   // 网络服务状态
    linkg_device_role_t     role;                    // 当前设备角色
    linkg_network_config_t  network_config;          // 网络配置快照
    linkg_wifi_config_t     wifi_config;             // Wi-Fi配置快照
    linkg_cellular_config_t cellular_config;         // 5G配置快照
    linkg_network_worker_t  wifi_worker;             // Wi-Fi Owner线程
    linkg_network_worker_t  cellular_worker;         // 5G管理线程
    bool                    ethernet_started;        // Ethernet是否已启动
    bool                    node_address_started;    // 本机节点IPv4地址是否已经配置
    bool                    ipv4_forwarding_enabled; // IPv4转发是否已启用
    bool                    wifi_initialized;        // Wi-Fi模块是否初始化
    bool                    cellular_initialized;    // 5G模块是否初始化
} linkg_network_context_t;

#endif
