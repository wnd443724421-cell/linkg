/**
 * @file cellular_tx_rate.h
 * @brief LinkG蜂窝发送共享令牌桶接口
 */

#ifndef CELLULAR_TX_RATE_H
#define CELLULAR_TX_RATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 令牌桶状态 ******************************/

typedef struct
{
    uint64_t rate_bytes_per_second; // 允许发送的字节速率
    uint64_t tokens_scaled;         // 当前令牌，精度为百万分之一字节
    uint64_t burst_scaled;          // 令牌容量，精度为百万分之一字节
    uint64_t last_refill_us;        // 最近一次更新的单调时钟
    uint32_t max_load_mbps;         // 当前生效的上行配置
} linkg_cellular_tx_rate_t;

/****************************** 令牌桶操作 ******************************/

int      linkg_cellular_tx_rate_init(linkg_cellular_tx_rate_t *rate, uint32_t max_load_mbps, uint64_t now_us);
int      linkg_cellular_tx_rate_set(linkg_cellular_tx_rate_t *rate, uint32_t max_load_mbps, uint64_t now_us);
void     linkg_cellular_tx_rate_refill(linkg_cellular_tx_rate_t *rate, uint64_t now_us);
bool     linkg_cellular_tx_rate_can_send(const linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes);
bool     linkg_cellular_tx_rate_debit(linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes);
uint64_t linkg_cellular_tx_rate_wait_us(const linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes);

#ifdef __cplusplus
}
#endif

#endif
