/**
 * @file transport_window.c
 * @brief LinkG传输层接收序列号窗口实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-11
 */

#include "transport_internal.h"

#include <string.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断窗口中指定历史偏移是否已经收到。
 */
static bool _linkg_transport_window_bit_test(const linkg_transport_rx_window_t *window, uint32_t offset)
{
    uint32_t bit_index;
    uint32_t word_index;

    if (window == NULL || offset >= LINKG_TRANSPORT_RX_WINDOW_BITS)
    {
        return false;
    }

    word_index = offset / 64U;
    bit_index  = offset % 64U;

    return (window->received_bitmap[word_index] & (1ULL << bit_index)) != 0ULL;
}

/**
 * @brief 标记窗口中指定历史偏移已经收到。
 */
static void _linkg_transport_window_bit_set(linkg_transport_rx_window_t *window, uint32_t offset)
{
    uint32_t bit_index;
    uint32_t word_index;

    if (window == NULL || offset >= LINKG_TRANSPORT_RX_WINDOW_BITS)
    {
        return;
    }

    word_index = offset / 64U;
    bit_index  = offset % 64U;

    window->received_bitmap[word_index] |= 1ULL << bit_index;
}

/**
 * @brief 统计当前有效观察范围指定偏移区间中尚未收到的Sequence数量。
 */
static uint32_t _linkg_transport_window_count_missing(const linkg_transport_rx_window_t *window, uint32_t start_offset, uint32_t end_offset)
{
    uint32_t missing;
    uint32_t offset;

    if (window == NULL || start_offset >= end_offset)
    {
        return 0U;
    }

    if (end_offset > window->tracked_count)
    {
        end_offset = window->tracked_count;
    }

    missing = 0U;

    for (offset = start_offset; offset < end_offset; offset++)
    {
        if (!_linkg_transport_window_bit_test(window, offset))
        {
            missing++;
        }
    }

    return missing;
}

/**
 * @brief 将当前位图整体移动到新的最高Sequence坐标系。
 *
 * @note bit 0始终表示highest_sequence，最高Sequence前进时旧Bit向更高偏移移动。
 */
static void _linkg_transport_window_shift(linkg_transport_rx_window_t *window, uint32_t advance)
{
    uint64_t shifted[LINKG_TRANSPORT_RX_WINDOW_WORDS];
    uint32_t bit_shift;
    uint32_t destination_word;
    uint32_t source_word;
    uint32_t word_shift;

    if (window == NULL || advance == 0U)
    {
        return;
    }

    if (advance >= LINKG_TRANSPORT_RX_WINDOW_BITS)
    {
        memset(window->received_bitmap, 0, sizeof(window->received_bitmap));
        return;
    }

    memset(shifted, 0, sizeof(shifted));

    word_shift = advance / 64U;
    bit_shift  = advance % 64U;

    for (destination_word = word_shift; destination_word < LINKG_TRANSPORT_RX_WINDOW_WORDS; destination_word++)
    {
        source_word = destination_word - word_shift;

        shifted[destination_word] |= window->received_bitmap[source_word] << bit_shift;

        if (bit_shift != 0U && source_word > 0U)
        {
            shifted[destination_word] |= window->received_bitmap[source_word - 1U] >> (64U - bit_shift);
        }
    }

    memcpy(window->received_bitmap, shifted, sizeof(window->received_bitmap));
}

/**
 * @brief 计算最高Sequence前进时最终滑出窗口且确认丢失的帧数量。
 */
static uint32_t _linkg_transport_window_confirm_lost(const linkg_transport_rx_window_t *window, uint32_t advance)
{
    uint32_t lost;
    uint32_t leaving_offset;

    if (window == NULL || advance == 0U || window->tracked_count == 0U)
    {
        return 0U;
    }

    if (advance >= LINKG_TRANSPORT_RX_WINDOW_BITS)
    {
        lost = _linkg_transport_window_count_missing(window, 0U, window->tracked_count);
        lost += advance - LINKG_TRANSPORT_RX_WINDOW_BITS;
        return lost;
    }

    leaving_offset = LINKG_TRANSPORT_RX_WINDOW_BITS - advance;

    if (window->tracked_count <= leaving_offset)
    {
        return 0U;
    }

    return _linkg_transport_window_count_missing(window, leaving_offset, window->tracked_count);
}

/****************************** 窗口处理 ******************************/

/**
 * @brief 重置接收序列号窗口。
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
 * @brief 接收一个Sequence并返回窗口判定，同时输出本次最终确认丢失数量。
 *
 * @note confirmed_lost只统计已经永久滑出512窗口且从未收到的Sequence。
 *       仅观察到中间Gap时不会立即计为丢失，迟到帧仍可在窗口内补齐。
 */
linkg_transport_window_result_t linkg_transport_window_accept_ex(linkg_transport_rx_window_t *window, uint32_t sequence, uint32_t *confirmed_lost)
{
    uint32_t advance;
    uint32_t behind;
    uint32_t new_tracked_count;
    int32_t  delta;

    if (confirmed_lost != NULL)
    {
        *confirmed_lost = 0U;
    }

    if (window == NULL)
    {
        return LINKG_TRANSPORT_WINDOW_INVALID;
    }

    if (!window->initialized)
    {
        memset(window->received_bitmap, 0, sizeof(window->received_bitmap));

        window->highest_sequence = sequence;
        window->tracked_count    = 1U;
        window->initialized      = true;

        _linkg_transport_window_bit_set(window, 0U);

        return LINKG_TRANSPORT_WINDOW_ACCEPT;
    }

    delta = (int32_t)(sequence - window->highest_sequence);

    if (delta > 0)
    {
        advance = (uint32_t)delta;

        if (confirmed_lost != NULL)
        {
            *confirmed_lost = _linkg_transport_window_confirm_lost(window, advance);
        }

        _linkg_transport_window_shift(window, advance);

        new_tracked_count = window->tracked_count + advance;
        if (new_tracked_count < window->tracked_count || new_tracked_count > LINKG_TRANSPORT_RX_WINDOW_BITS)
        {
            new_tracked_count = LINKG_TRANSPORT_RX_WINDOW_BITS;
        }

        window->highest_sequence = sequence;
        window->tracked_count    = new_tracked_count;

        _linkg_transport_window_bit_set(window, 0U);

        return LINKG_TRANSPORT_WINDOW_ACCEPT;
    }

    behind = window->highest_sequence - sequence;

    if (behind >= LINKG_TRANSPORT_RX_WINDOW_BITS || behind >= window->tracked_count)
    {
        return LINKG_TRANSPORT_WINDOW_TOO_OLD;
    }

    if (_linkg_transport_window_bit_test(window, behind))
    {
        return LINKG_TRANSPORT_WINDOW_DUPLICATE;
    }

    _linkg_transport_window_bit_set(window, behind);

    return LINKG_TRANSPORT_WINDOW_ACCEPT;
}

/**
 * @brief 接收一个Sequence并返回窗口判定。
 */
linkg_transport_window_result_t linkg_transport_window_accept(linkg_transport_rx_window_t *window, uint32_t sequence)
{
    return linkg_transport_window_accept_ex(window, sequence, NULL);
}
