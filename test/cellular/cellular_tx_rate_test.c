/**
 * @file cellular_tx_rate_test.c
 * @brief 蜂窝共享令牌桶的边界与速率回归测试
 *
 * Host编译示例：
 * gcc -std=gnu17 -Wall -Wextra -Werror \
 *     -Imodules/cellular -Iinclude/linkg/config -Ithird_party/cjson \
 *     test/cellular/cellular_tx_rate_test.c modules/cellular/cellular_tx_rate.c \
 *     -o /tmp/cellular_tx_rate_test
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "cellular_tx_rate.h"

/****************************** 速率与精度 ******************************/

static void test_refill_and_debit(void)
{
    linkg_cellular_tx_rate_t rate;
    uint64_t                now_us;
    int                     ret;

    now_us = 1000000ULL;

    ret = linkg_cellular_tx_rate_init(&rate, 1U, now_us);
    assert(ret == 0);
    assert(rate.max_load_mbps == 1U);

    // 1Mbps在12ms内补充1500字节；测试微字节整数累计。
    assert(linkg_cellular_tx_rate_debit(&rate, 6400U));
    assert(!linkg_cellular_tx_rate_can_send(&rate, 1U));

    now_us += 12000ULL;
    linkg_cellular_tx_rate_refill(&rate, now_us);

    assert(linkg_cellular_tx_rate_can_send(&rate, 1500U));
    assert(!linkg_cellular_tx_rate_can_send(&rate, 1501U));
    assert(linkg_cellular_tx_rate_wait_us(&rate, 1600U) == 800U);
    assert(linkg_cellular_tx_rate_debit(&rate, 1500U));
    assert(!linkg_cellular_tx_rate_debit(&rate, 1U));
}

/****************************** 容量与边界 ******************************/

static void test_rate_change_and_burst(void)
{
    linkg_cellular_tx_rate_t rate;
    int                     ret;

    ret = linkg_cellular_tx_rate_init(&rate, 3U, 1000000ULL);
    assert(ret == 0);
    assert(rate.rate_bytes_per_second == 375000ULL);

    // 大于当前桶容量的请求不应无期限等待。
    assert(linkg_cellular_tx_rate_wait_us(&rate, 6401U) == UINT64_MAX);

    assert(linkg_cellular_tx_rate_set(&rate, 0U, 1000000ULL) < 0);
    assert(linkg_cellular_tx_rate_set(&rate, 101U, 1000000ULL) < 0);
    assert(linkg_cellular_tx_rate_set(&rate, 1U, 1000000ULL) == 0);
    assert(rate.max_load_mbps == 1U);
    assert(rate.tokens_scaled <= rate.burst_scaled);

    // 长时间空闲后最多补满一个桶。
    assert(linkg_cellular_tx_rate_debit(&rate, 6400U));
    linkg_cellular_tx_rate_refill(&rate, 6000000ULL);
    assert(linkg_cellular_tx_rate_can_send(&rate, 6400U));
    assert(!linkg_cellular_tx_rate_can_send(&rate, 6401U));
}

/****************************** 测试入口 ******************************/

int main(void)
{
    test_refill_and_debit();
    test_rate_change_and_burst();

    puts("cellular_tx_rate_test: PASS");
    return 0;
}
