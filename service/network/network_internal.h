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

typedef int (*linkg_network_worker_init_func_t)(void);
typedef int (*linkg_network_worker_start_func_t)(void);
typedef int (*linkg_network_worker_run_func_t)(linkg_thread_t *thread);
typedef int (*linkg_network_worker_stop_func_t)(void);
typedef int (*linkg_network_worker_deinit_func_t)(void);

typedef struct
{
    linkg_thread_t                     thread;             // Owner线程对象

    linkg_network_worker_init_func_t   init;               // 模块初始化函数
    linkg_network_worker_start_func_t  start;              // 模块启动函数
    linkg_network_worker_run_func_t    run;                // 模块运行函数
    linkg_network_worker_stop_func_t   stop;               // 模块停止函数
    linkg_network_worker_deinit_func_t deinit;             // 模块反初始化函数

    int                                stop_result;        // 最近一次模块清理结果

    bool                               initialized;        // Owner线程对象是否初始化
    bool                               module_initialized; // 模块是否初始化
    bool                               start_attempted;    // 本轮是否已尝试启动Owner

} linkg_network_worker_t;
/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t         lock;                    // 网络服务状态锁
    linkg_packet_pool_t    *packet_pool;             // 全局共享Packet Pool，不由Network释放

    linkg_network_state_t   state;                   // 网络服务状态
    linkg_device_role_t     role;                    // 当前设备角色

    linkg_network_config_t  network_config;          // 网络配置快照
    linkg_wifi_config_t     wifi_config;             // Wi-Fi配置快照
    linkg_cellular_config_t cellular_config;         // 5G配置快照

    linkg_thread_t          manager_thread;          // Network长期管理线程
    linkg_network_worker_t  wifi_worker;             // Wi-Fi Owner线程
    linkg_network_worker_t  cellular_worker;         // 5G Owner线程

    bool                    node_address_started;    // 本机节点IPv4地址是否已经配置
    bool                    ipv4_forwarding_enabled; // IPv4转发是否已启用

    bool                    ethernet_initialized;    // Ethernet模块是否初始化
} linkg_network_context_t;

extern linkg_network_context_t g_network;

#endif
