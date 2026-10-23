/**
 * @file cellular_tx_rate.c
 * @brief LinkG蜂窝发送共享令牌桶实现
 */

#include "cellular_tx_rate.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "linkg_cellular_config.h"

/****************************** 算法常量 ******************************/

#define LINKG_CELLULAR_TX_RATE_TOKEN_SCALE    1000000ULL // 百万分之一字节精度
#define LINKG_CELLULAR_TX_RATE_MIN_BURST      6400ULL    // 至少容纳两个最大Transport分片
#define LINKG_CELLULAR_TX_RATE_BURST_DIVISOR  200ULL     // 默认允许约5ms突发
#define LINKG_CELLULAR_TX_RATE_MAX_DELTA_US   1000000ULL // 大于1秒按桶满处理

/****************************** 令牌桶操作 ******************************/

/**
 * @brief 初始化共享令牌桶；新会话允许容量范围内的短时突发。
 */
int linkg_cellular_tx_rate_init(linkg_cellular_tx_rate_t *rate, uint32_t max_load_mbps, uint64_t now_us)
{
    if (rate == NULL)
    {
        return -EINVAL;
    }

    memset(rate, 0, sizeof(*rate));
    return linkg_cellular_tx_rate_set(rate, max_load_mbps, now_us);
}

/**
 * @brief 配置速率；保留已有令牌但不允许超过新容量。
 *
 * 所有接口由Cellular TX唯一互斥锁串行化，本模块不持有线程资源。
 */
int linkg_cellular_tx_rate_set(linkg_cellular_tx_rate_t *rate, uint32_t max_load_mbps, uint64_t now_us)
{
    uint64_t burst_bytes;

    if (rate == NULL ||
        max_load_mbps < LINKG_CELLULAR_MAX_LOAD_MBPS_MIN ||
        max_load_mbps > LINKG_CELLULAR_MAX_LOAD_MBPS_MAX)
    {
        return -EINVAL;
    }

    if (rate->max_load_mbps == max_load_mbps)
    {
        return 0;
    }

    if (rate->max_load_mbps != 0U)
    {
        linkg_cellular_tx_rate_refill(rate, now_us);
    }

    rate->rate_bytes_per_second = (uint64_t)max_load_mbps * 125000ULL;

    burst_bytes = rate->rate_bytes_per_second / LINKG_CELLULAR_TX_RATE_BURST_DIVISOR;
    if (burst_bytes < LINKG_CELLULAR_TX_RATE_MIN_BURST)
    {
        burst_bytes = LINKG_CELLULAR_TX_RATE_MIN_BURST;
    }

    rate->burst_scaled = burst_bytes * LINKG_CELLULAR_TX_RATE_TOKEN_SCALE;

    if (rate->max_load_mbps == 0U)
    {
        rate->tokens_scaled = rate->burst_scaled;
    }
    else if (rate->tokens_scaled > rate->burst_scaled)
    {
        rate->tokens_scaled = rate->burst_scaled;
    }

    rate->max_load_mbps  = max_load_mbps;
    rate->last_refill_us = now_us;
    return 0;
}

/**
 * @brief 依据单调时钟补充令牌，无需固定频率定时器。
 */
void linkg_cellular_tx_rate_refill(linkg_cellular_tx_rate_t *rate, uint64_t now_us)
{
    uint64_t elapsed_us;
    uint64_t added;

    if (rate == NULL || rate->max_load_mbps == 0U || now_us <= rate->last_refill_us)
    {
        return;
    }

    elapsed_us = now_us - rate->last_refill_us;
    rate->last_refill_us = now_us;

    if (elapsed_us >= LINKG_CELLULAR_TX_RATE_MAX_DELTA_US)
    {
        rate->tokens_scaled = rate->burst_scaled;
        return;
    }

    added = elapsed_us * rate->rate_bytes_per_second;
    if (added >= rate->burst_scaled - rate->tokens_scaled)
    {
        rate->tokens_scaled = rate->burst_scaled;
    }
    else
    {
        rate->tokens_scaled += added;
    }
}

/** @brief 判断指定Wire报文字节数是否拥有完整发送额度。 */
bool linkg_cellular_tx_rate_can_send(const linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes)
{
    if (rate == NULL || wire_bytes > UINT64_MAX / LINKG_CELLULAR_TX_RATE_TOKEN_SCALE)
    {
        return false;
    }

    return wire_bytes * LINKG_CELLULAR_TX_RATE_TOKEN_SCALE <= rate->tokens_scaled;
}

/**
 * @brief 只在实际成功提交Socket后扣令牌，失败时不改变状态。
 */
bool linkg_cellular_tx_rate_debit(linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes)
{
    if (!linkg_cellular_tx_rate_can_send(rate, wire_bytes))
    {
        return false;
    }

    rate->tokens_scaled -= wire_bytes * LINKG_CELLULAR_TX_RATE_TOKEN_SCALE;
    return true;
}

/**
 * @brief 返回凑齐发送额度所需时间，已足够返回0，不可达返回UINT64_MAX。
 */
uint64_t linkg_cellular_tx_rate_wait_us(const linkg_cellular_tx_rate_t *rate, uint64_t wire_bytes)
{
    uint64_t required;
    uint64_t missing;

    if (rate == NULL || rate->rate_bytes_per_second == 0U ||
        wire_bytes > UINT64_MAX / LINKG_CELLULAR_TX_RATE_TOKEN_SCALE)
    {
        return UINT64_MAX;
    }

    required = wire_bytes * LINKG_CELLULAR_TX_RATE_TOKEN_SCALE;
    if (required > rate->burst_scaled)
    {
        return UINT64_MAX;
    }

    if (required <= rate->tokens_scaled)
    {
        return 0U;
    }

    missing = required - rate->tokens_scaled;
    return (missing + rate->rate_bytes_per_second - 1U) / rate->rate_bytes_per_second;
}
