/**
 * @file linkg_packet_pool.c
 * @brief LinkG数据包内存池实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-31
 */

#include "linkg_packet_pool.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/****************************** 模块常量 ******************************/

#define LINKG_PACKET_SLOT_ALIGNMENT 64U // 数据包槽位起始地址对齐字节数

/****************************** 引用规则 ******************************/

/**
 * 数据包引用规则：
 *
 * 1. alloc成功后，调用方持有一个Packet引用。
 * 2. 只有持有有效引用的模块才能调用retain。
 * 3. 每一个有效引用必须对应一次release。
 * 4. 引用计数归零后Packet立即归还Pool，不得继续访问。
 * 5. pool_deinit前必须停止所有访问Pool的线程。
 * 6. Pool对象首次init前必须清零。
 * 7. reference_count支持跨线程引用管理，data_offset/data_length/flags等可变元数据不支持无锁并发修改。
 */

/****************************** 内部辅助 ******************************/

/**
 * @brief 释放数据包的一个引用并判断引用计数是否归零。
 *
 * @note 调用方必须持有有效Packet引用；返回true后不得继续访问该引用。
 */
static bool _linkg_packet_release_reference(linkg_packet_t *packet)
{
    uint32_t old_count;

    old_count = atomic_fetch_sub_explicit(&packet->reference_count, 1U, memory_order_acq_rel);

    assert(old_count > 0U);

    return old_count == 1U;
}

/**
 * @brief 重置数据包可变元数据。
 *
 * @note 调用时Packet必须尚未交给外部模块，或者引用计数已经归零。
 */
static void _linkg_packet_reset(linkg_packet_t *packet)
{
    if (packet == NULL || packet->pool == NULL)
    {
        return;
    }

    packet->data_offset = packet->pool->headroom;
    packet->data_length = 0U;
    packet->flags = LINKG_PACKET_FLAG_NONE;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化数据包内存池。
 *
 * @note Pool对象首次初始化前必须清零；重复初始化返回-EALREADY。
 */
int linkg_packet_pool_init(linkg_packet_pool_t *pool, const linkg_packet_pool_config_t *config)
{
    linkg_packet_t *packet;
    size_t data_region_size;
    uint32_t slot_stride;
    uint32_t index;
    int ret;

    if (pool == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (pool->initialized)
    {
        return -EALREADY;
    }

    if (config->packet_count == 0U || config->slot_size == 0U)
    {
        return -EINVAL;
    }

    if (config->headroom >= config->slot_size)
    {
        return -EINVAL;
    }

    if (config->slot_size > UINT32_MAX - (LINKG_PACKET_SLOT_ALIGNMENT - 1U))
    {
        return -EOVERFLOW;
    }

    slot_stride = (config->slot_size + LINKG_PACKET_SLOT_ALIGNMENT - 1U) &
                  ~(LINKG_PACKET_SLOT_ALIGNMENT - 1U);

    if ((size_t)config->packet_count > SIZE_MAX / slot_stride)
    {
        return -EOVERFLOW;
    }

    data_region_size = (size_t)config->packet_count * slot_stride;

    memset(pool, 0, sizeof(*pool));

    pool->packet_count = config->packet_count;
    pool->slot_size = config->slot_size;
    pool->slot_stride = slot_stride;
    pool->headroom = config->headroom;

    pool->packets = calloc(pool->packet_count, sizeof(*pool->packets));
    if (pool->packets == NULL)
    {
        ret = -ENOMEM;
        goto fail;
    }

    pool->free_packets = calloc(pool->packet_count, sizeof(*pool->free_packets));
    if (pool->free_packets == NULL)
    {
        ret = -ENOMEM;
        goto fail;
    }

    // 保证每个数据包槽位的起始地址按64字节对齐。
    ret = posix_memalign((void **)&pool->data_region, LINKG_PACKET_SLOT_ALIGNMENT, data_region_size);
    if (ret != 0)
    {
        ret = -ret;
        goto fail;
    }

    /**
     * 清零连续数据区域并预触碰全部内存页，
     * 避免Packet首次使用时才产生缺页开销。
     */
    memset(pool->data_region, 0, data_region_size);

    ret = pthread_mutex_init(&pool->lock, NULL);
    if (ret != 0)
    {
        ret = -ret;
        goto fail;
    }

    for (index = 0U; index < pool->packet_count; index++)
    {
        packet = &pool->packets[index];

        packet->pool = pool;
        packet->slot = pool->data_region + (size_t)index * pool->slot_stride;

        _linkg_packet_reset(packet);

        atomic_init(&packet->reference_count, 0U);

        pool->free_packets[index] = packet;
    }

    pool->free_count = pool->packet_count;
    pool->initialized = true;

    return 0;

fail:
    free(pool->data_region);
    free(pool->free_packets);
    free(pool->packets);

    memset(pool, 0, sizeof(*pool));

    return ret;
}

/**
 * @brief 销毁数据包内存池。
 *
 * @note 调用前必须停止并等待所有可能访问Pool的线程退出，
 *       并确保全部Packet引用已经释放并归还Pool。
 */
int linkg_packet_pool_deinit(linkg_packet_pool_t *pool)
{
    int unlock_ret;
    int ret;

    if (pool == NULL)
    {
        return -EINVAL;
    }

    if (!pool->initialized)
    {
        return 0;
    }

    /**
     * 三块核心内存必须同时存在，
     * 只存在部分资源说明Pool内部状态已经异常。
     */
    if (pool->packets == NULL ||
        pool->free_packets == NULL ||
        pool->data_region == NULL ||
        pool->packet_count == 0U)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&pool->lock);
    if (ret != 0)
    {
        return -ret;
    }

    // 所有外部Packet引用必须释放完成后才能销毁Pool。
    if (pool->free_count != pool->packet_count)
    {
        unlock_ret = pthread_mutex_unlock(&pool->lock);
        if (unlock_ret != 0)
        {
            return -unlock_ret;
        }

        return -EBUSY;
    }

    ret = pthread_mutex_unlock(&pool->lock);
    if (ret != 0)
    {
        return -ret;
    }

    ret = pthread_mutex_destroy(&pool->lock);
    if (ret != 0)
    {
        return -ret;
    }

    free(pool->data_region);
    free(pool->free_packets);
    free(pool->packets);

    memset(pool, 0, sizeof(*pool));

    return 0;
}

/****************************** 数据包申请 ******************************/

/**
 * @brief 从内存池申请一个数据包。
 *
 * @note 成功返回的Packet引用计数为1，该引用归调用方所有。
 */
linkg_packet_t *linkg_packet_pool_alloc(linkg_packet_pool_t *pool)
{
    linkg_packet_t *packet;
    int ret;

    if (pool == NULL || !pool->initialized || pool->free_packets == NULL)
    {
        return NULL;
    }

    ret = pthread_mutex_lock(&pool->lock);
    if (ret != 0)
    {
        return NULL;
    }

    if (pool->free_count == 0U)
    {
        (void)pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    packet = pool->free_packets[--pool->free_count];

    _linkg_packet_reset(packet);
    atomic_store_explicit(&packet->reference_count, 1U, memory_order_relaxed);

    (void)pthread_mutex_unlock(&pool->lock);

    return packet;
}

/**
 * @brief 从内存池批量申请数据包。
 *
 * @note 返回值为实际申请数量，每个成功返回的Packet都由调用方持有一个引用。
 */
uint32_t linkg_packet_pool_alloc_batch(linkg_packet_pool_t *pool, linkg_packet_t **packets, uint32_t packet_count)
{
    linkg_packet_t *packet;
    uint32_t allocated_count;
    uint32_t index;
    int ret;

    if (pool == NULL ||
        !pool->initialized ||
        pool->free_packets == NULL ||
        packets == NULL ||
        packet_count == 0U)
    {
        return 0U;
    }

    ret = pthread_mutex_lock(&pool->lock);
    if (ret != 0)
    {
        return 0U;
    }

    allocated_count = packet_count;

    if (allocated_count > pool->free_count)
    {
        allocated_count = pool->free_count;
    }

    for (index = 0U; index < allocated_count; index++)
    {
        packet = pool->free_packets[--pool->free_count];

        _linkg_packet_reset(packet);
        atomic_store_explicit(&packet->reference_count, 1U, memory_order_relaxed);

        packets[index] = packet;
    }

    (void)pthread_mutex_unlock(&pool->lock);

    return allocated_count;
}

/****************************** 引用管理 ******************************/

/**
 * @brief 增加数据包引用计数。
 *
 * @note 只有当前已经持有有效Packet引用的模块才能调用本函数。
 */
void linkg_packet_retain(linkg_packet_t *packet)
{
    uint32_t old_count;

    if (packet == NULL)
    {
        return;
    }

    old_count = atomic_fetch_add_explicit(&packet->reference_count, 1U, memory_order_relaxed);

    assert(old_count > 0U);
    assert(old_count < UINT32_MAX);

    // NDEBUG下assert被移除，显式标记调试检查变量已使用。
    (void)old_count;
}

/**
 * @brief 释放数据包的一个引用。
 *
 * @note 每个调用都消费调用方持有的一个引用；引用归零后Packet立即归还Pool。
 */
void linkg_packet_release(linkg_packet_t *packet)
{
    linkg_packet_pool_t *pool;
    int ret;

    if (packet == NULL || packet->pool == NULL)
    {
        return;
    }

    if (!_linkg_packet_release_reference(packet))
    {
        return;
    }

    pool = packet->pool;

    _linkg_packet_reset(packet);

    ret = pthread_mutex_lock(&pool->lock);
    if (ret != 0)
    {
        return;
    }

    if (pool->free_count < pool->packet_count)
    {
        pool->free_packets[pool->free_count++] = packet;
    }

    (void)pthread_mutex_unlock(&pool->lock);
}

/**
 * @brief 批量释放同一内存池中的数据包引用。
 *
 * @note packets中的有效Packet必须属于指定Pool；每个非NULL元素消费一个调用方引用。
 */
void linkg_packet_pool_release_batch(linkg_packet_pool_t *pool, linkg_packet_t **packets, uint32_t packet_count)
{
    linkg_packet_t *packet;
    uint32_t index;
    int ret;

    if (pool == NULL || packets == NULL || packet_count == 0U)
    {
        return;
    }

    ret = pthread_mutex_lock(&pool->lock);
    if (ret != 0)
    {
        return;
    }

    for (index = 0U; index < packet_count; index++)
    {
        packet = packets[index];

        if (packet == NULL)
        {
            continue;
        }

        if (packet->pool != pool)
        {
            assert(false);
            continue;
        }

        if (!_linkg_packet_release_reference(packet))
        {
            continue;
        }

        if (pool->free_count >= pool->packet_count)
        {
            assert(false);
            continue;
        }

        _linkg_packet_reset(packet);
        pool->free_packets[pool->free_count++] = packet;
    }

    (void)pthread_mutex_unlock(&pool->lock);
}

/****************************** 数据访问 ******************************/

/**
 * @brief 在数据包当前有效数据前预留并加入头部空间。
 *
 * @note 成功后data_offset向前移动，data_length同步增加header_size。
 */
void *linkg_packet_push(linkg_packet_t *packet, uint32_t header_size)
{
    if (packet == NULL || header_size == 0U)
    {
        return NULL;
    }

    if (header_size > packet->data_offset)
    {
        return NULL;
    }

    packet->data_offset -= header_size;
    packet->data_length += header_size;

    return packet->slot + packet->data_offset;
}
