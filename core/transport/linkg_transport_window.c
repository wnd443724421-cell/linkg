/**
 * @file linkg_transport_window.c
 * @brief LinkG传输层接收去重窗口
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
 */

#include "linkg_transport_internal.h"

#include <stdint.h>
#include <string.h>

_Static_assert((LINKG_TRANSPORT_RX_WINDOW_BITS & (LINKG_TRANSPORT_RX_WINDOW_BITS - 1U)) == 0U, "RX window bits must be power of two");
_Static_assert((LINKG_TRANSPORT_RX_WINDOW_BITS % 64U) == 0U, "RX window bits must align to 64 bits");
_Static_assert((LINKG_TRANSPORT_RX_WINDOW_WORDS & (LINKG_TRANSPORT_RX_WINDOW_WORDS - 1U)) == 0U, "RX window words must be power of two");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取序列号在环形接收位图中的位置。
 */
static inline void _linkg_transport_window_position(uint32_t sequence, uint32_t *word_index, uint64_t *bit_mask)
{
    *word_index = (sequence >> 6U) & (LINKG_TRANSPORT_RX_WINDOW_WORDS - 1U);
    *bit_mask   = UINT64_C(1) << (sequence & 63U);
}

/**
 * @brief 清除指定序列号对应的环形位图位置。
 */
static inline void _linkg_transport_window_clear_sequence(linkg_transport_rx_window_t *window, uint32_t sequence)
{
    uint32_t word_index;
    uint64_t bit_mask;

    _linkg_transport_window_position(sequence, &word_index, &bit_mask);

    window->received_bitmap[word_index] &= ~bit_mask;
}

/**
 * @brief 设置指定序列号已经接收。
 */
static inline void _linkg_transport_window_set_sequence(linkg_transport_rx_window_t *window, uint32_t sequence)
{
    uint32_t word_index;
    uint64_t bit_mask;

    _linkg_transport_window_position(sequence, &word_index, &bit_mask);

    window->received_bitmap[word_index] |= bit_mask;
}

/**
 * @brief 判断指定序列号是否已经接收。
 */
static inline bool _linkg_transport_window_has_sequence(const linkg_transport_rx_window_t *window, uint32_t sequence)
{
    uint32_t word_index;
    uint64_t bit_mask;

    _linkg_transport_window_position(sequence, &word_index, &bit_mask);

    return (window->received_bitmap[word_index] & bit_mask) != 0U;
}

/****************************** 窗口控制 ******************************/

/**
 * @brief 重置接收去重窗口。
 */
void linkg_transport_window_reset(linkg_transport_rx_window_t *window)
{
    if (window == NULL)
    {
        return;
    }

    memset(window, 0, sizeof(*window));
}

/**
 * @brief 接收并记录一个序列号。
 *
 * @note 窗口仅用于去重和有限范围乱序接收，不缓存或排序Packet。
 *       序列号采用32位回绕比较，有效乱序跨度必须小于2^31。
 */
linkg_transport_window_result_t linkg_transport_window_accept(linkg_transport_rx_window_t *window, uint32_t sequence)
{
    uint32_t distance;
    uint32_t step;
    int32_t  delta;

    if (window == NULL)
    {
        return LINKG_TRANSPORT_WINDOW_INVALID;
    }

    if (!window->initialized)
    {
        memset(window->received_bitmap, 0, sizeof(window->received_bitmap));

        window->highest_sequence = sequence;
        window->initialized      = true;

        _linkg_transport_window_set_sequence(window, sequence);

        return LINKG_TRANSPORT_WINDOW_ACCEPT;
    }

    delta = (int32_t)(sequence - window->highest_sequence);

    if (delta > 0)
    {
        if ((uint32_t)delta >= LINKG_TRANSPORT_RX_WINDOW_BITS)
        {
            memset(window->received_bitmap, 0, sizeof(window->received_bitmap));
        }
        else
        {
            for (step = 1U; step <= (uint32_t)delta; step++)
            {
                _linkg_transport_window_clear_sequence(window,
                                                       window->highest_sequence + step);
            }
        }

        window->highest_sequence = sequence;

        _linkg_transport_window_set_sequence(window, sequence);

        return LINKG_TRANSPORT_WINDOW_ACCEPT;
    }

    if (delta == 0)
    {
        return LINKG_TRANSPORT_WINDOW_DUPLICATE;
    }

    distance = window->highest_sequence - sequence;

    if (distance >= LINKG_TRANSPORT_RX_WINDOW_BITS)
    {
        return LINKG_TRANSPORT_WINDOW_TOO_OLD;
    }

    if (_linkg_transport_window_has_sequence(window, sequence))
    {
        return LINKG_TRANSPORT_WINDOW_DUPLICATE;
    }

    _linkg_transport_window_set_sequence(window, sequence);

    return LINKG_TRANSPORT_WINDOW_ACCEPT;
}
