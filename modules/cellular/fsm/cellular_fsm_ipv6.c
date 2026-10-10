/**
 * @file cellular_fsm_ipv6.c
 * @brief LinkG蜂窝IPv6地址及前缀判定辅助实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/****************************** IPv6网络辅助 ******************************/

/**
 * @brief 判断IPv6地址是否属于指定网络前缀。
 */
bool _cellular_fsm_ipv6_in_prefix(const struct in6_addr *address, const struct in6_addr *prefix, uint8_t prefix_length)
{
    uint8_t mask;
    size_t  full_bytes;
    uint8_t remaining_bits;

    if (address == NULL || prefix == NULL || prefix_length == 0U || prefix_length > 128U)
    {
        return false;
    }

    full_bytes     = prefix_length / 8U;
    remaining_bits = prefix_length % 8U;

    if (full_bytes > 0U && memcmp(address->s6_addr, prefix->s6_addr, full_bytes) != 0)
    {
        return false;
    }

    if (remaining_bits == 0U)
    {
        return true;
    }

    mask = (uint8_t)(0xffU << (8U - remaining_bits));

    return (address->s6_addr[full_bytes] & mask) ==
           (prefix->s6_addr[full_bytes] & mask);
}
