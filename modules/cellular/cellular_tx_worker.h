/**
 * @file cellular_tx_worker.h
 * @brief LinkG蜂窝发送异步调度Worker接口
 */

#ifndef CELLULAR_TX_WORKER_H
#define CELLULAR_TX_WORKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型与回调 ******************************/

typedef struct linkg_cellular_tx_worker linkg_cellular_tx_worker_t;

/**
 * @brief 执行一次发送调度，并返回下一次检查的单调时钟绝对时间（微秒）。
 *
 * 由TX模块负责队列互斥锁及业务语义；Worker只负责唤醒与定时。
 */
typedef uint64_t (*linkg_cellular_tx_worker_process_fn)(void *context, uint64_t now_us);

/****************************** 生命周期 ******************************/

linkg_cellular_tx_worker_t *linkg_cellular_tx_worker_create(linkg_cellular_tx_worker_process_fn process, void *context);
void                       linkg_cellular_tx_worker_destroy(linkg_cellular_tx_worker_t *worker);
int                        linkg_cellular_tx_worker_start(linkg_cellular_tx_worker_t *worker);
int                        linkg_cellular_tx_worker_stop(linkg_cellular_tx_worker_t *worker);

/****************************** 事件通知 ******************************/

void linkg_cellular_tx_worker_notify(linkg_cellular_tx_worker_t *worker);

#ifdef __cplusplus
}
#endif

#endif
