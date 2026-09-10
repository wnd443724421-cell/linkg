/**
 * @file link_internal.h
 * @brief LinkG链路基类内部定义
 */

#ifndef LINK_INTERNAL_H
#define LINK_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"
#include "linkg_thread.h"

/****************************** 运行资源 ******************************/

struct linkg_link_runtime
{
    pthread_mutex_t                  control_lock;      // 链路生命周期状态保护锁
    pthread_rwlock_t                 io_lock;           // 同步发送并发读锁，关闭链路使用写锁

    linkg_thread_t                   rx_thread;         // 链路接收队列消费线程

    linkg_link_rx_item_t            *rx_items;          // 接收队列批量取出元素数组

    linkg_link_receive_batch_func_t  receive;           // 上层批量接收处理函数
    void                            *receive_user_data; // 接收处理私有数据

    uint32_t                         tx_batch_size;     // 单次发送批次最大包数
    uint32_t                         rx_batch_size;     // 单次接收队列消费最大包数

    bool                             opened;            // 具体链路运行资源是否已经打开
};

/****************************** 内部接口 ******************************/

void linkg_link_rx_thread(linkg_thread_t *thread, void *user_data);

int  linkg_link_runtime_create(linkg_link_t *link, const linkg_link_config_t *config);
void linkg_link_runtime_destroy(linkg_link_t *link);

#endif
