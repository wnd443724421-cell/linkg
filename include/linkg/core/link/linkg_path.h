/**
 * @file linkg_path.h
 * @brief LinkG对端链路路径定义
 * @author Dawn
 * @version 1.4.0
 * @date 2026-08-25
 */

#ifndef LINKG_PATH_H
#define LINKG_PATH_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>

#include "linkg_path_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 路径状态 ******************************/

typedef enum
{
    LINKG_PATH_STATE_EMPTY = 0, // 空闲路径
    LINKG_PATH_STATE_ACTIVE,    // 路径已注册
    LINKG_PATH_STATE_RETIRED,   // 路径已注销，等待引用释放
    LINKG_PATH_STATE_RELEASED   // 引用已释放，等待节点回收
} linkg_path_state_t;

/****************************** 网络端点 ******************************/

typedef struct
{
    struct sockaddr_storage address; // 下一跳套接字地址
    socklen_t               length;  // 地址有效长度
} linkg_path_endpoint_t;

/****************************** 链路路径 ******************************/

typedef struct linkg_path linkg_path_t; // 对端链路路径

typedef void (*linkg_path_released_func_t)(linkg_path_t *path, void *user_data); // 路径异步释放完成回调

struct linkg_path
{
    pthread_mutex_t            stats_lock;         // 路径累计统计保护锁
    linkg_path_stats_t         stats;              // 路径累计统计

    uint32_t                   link_id;            // 承载链路ID
    linkg_path_endpoint_t      next_hop;           // 下一跳端点

    _Atomic uint32_t           reference_count;    // 当前异步持有引用数量
    _Atomic linkg_path_state_t state;              // 路径生命周期状态

    linkg_path_released_func_t released;           // 异步释放完成回调
    void                      *released_user_data; // 回调私有数据

    bool                       initialized;        // 路径对象是否已经初始化
};

/****************************** 生命周期 ******************************/

int  linkg_path_init(linkg_path_t *path);
int  linkg_path_deinit(linkg_path_t *path);
int  linkg_path_activate(linkg_path_t *path, uint32_t link_id, const linkg_path_endpoint_t *next_hop, linkg_path_released_func_t released, void *user_data);
int  linkg_path_acquire(linkg_path_t *path);
int  linkg_path_acquire_batch(linkg_path_t *path, uint32_t count);
int  linkg_path_retire(linkg_path_t *path, bool *released);
int  linkg_path_reset(linkg_path_t *path);
int  linkg_path_update_endpoint(linkg_path_t *path, const linkg_path_endpoint_t *next_hop);
void linkg_path_release(linkg_path_t *path);

/****************************** 统计操作 ******************************/

void linkg_path_record_tx_success(linkg_path_t *path, uint64_t bytes, uint64_t packets);
void linkg_path_record_tx_failed(linkg_path_t *path, uint64_t bytes, uint64_t packets);
void linkg_path_record_tx_dropped(linkg_path_t *path, uint64_t bytes, uint64_t packets);
void linkg_path_record_tx_expired(linkg_path_t *path, uint64_t bytes, uint64_t packets);
void linkg_path_record_rx(linkg_path_t *path, uint64_t bytes, uint64_t packets);
int  linkg_path_get_stats(linkg_path_t *path, linkg_path_stats_t *stats);

/****************************** 状态查询 ******************************/

linkg_path_state_t linkg_path_get_state(const linkg_path_t *path);
uint32_t           linkg_path_get_reference_count(const linkg_path_t *path);
bool               linkg_path_is_active(const linkg_path_t *path);

#ifdef __cplusplus
}
#endif

#endif
