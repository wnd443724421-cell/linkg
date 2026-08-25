/**
 * @file linkg_packet_pool.h
 * @brief LinkG数据包内存池接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-31
 */

#ifndef LINKG_PACKET_POOL_H
#define LINKG_PACKET_POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef struct linkg_packet      linkg_packet_t;      // 数据包对象
typedef struct linkg_packet_pool linkg_packet_pool_t; // 数据包内存池

/****************************** 数据包标志 ******************************/

#define LINKG_PACKET_FLAG_NONE     0U        // 普通数据，无特殊业务标志
#define LINKG_PACKET_FLAG_REALTIME (1U << 0) // 实时低延时数据
#define LINKG_PACKET_FLAG_VIDEO    (1U << 1) // 视频实时媒体数据

/****************************** 内存池配置 ******************************/

typedef struct
{
    uint32_t packet_count; // 数据包槽位数量
    uint32_t slot_size;    // 单个槽位总大小，包含预留头部
    uint32_t headroom;     // 初始数据前预留的头部空间
} linkg_packet_pool_config_t;

/****************************** 数据包定义 ******************************/

struct linkg_packet
{
    linkg_packet_pool_t *pool;            // 所属内存池
    uint8_t             *slot;            // 固定槽位起始地址
    _Atomic uint32_t     reference_count; // 当前引用数量
    uint32_t             data_offset;     // 有效数据相对slot的偏移
    uint32_t             data_length;     // 当前有效数据长度
    uint32_t             flags;           // 数据包业务分类标志
};

/****************************** 内存池定义 ******************************/

struct linkg_packet_pool
{
    linkg_packet_t  *packets;      // 全部数据包描述符
    linkg_packet_t **free_packets; // 空闲数据包指针栈
    uint8_t         *data_region;  // 连续槽位数据区域

    pthread_mutex_t  lock;         // 空闲数据包指针栈保护锁

    uint32_t         packet_count; // 数据包槽位总数
    uint32_t         slot_size;    // 单个槽位可用总大小
    uint32_t         slot_stride;  // 相邻槽位起始地址间隔
    uint32_t         headroom;     // 默认预留头部大小
    uint32_t         free_count;   // 当前空闲槽位数量
    bool             initialized;  // 内存池是否已经初始化
};

/****************************** 生命周期 ******************************/

int linkg_packet_pool_init(linkg_packet_pool_t *pool, const linkg_packet_pool_config_t *config);
int linkg_packet_pool_deinit(linkg_packet_pool_t *pool);

/****************************** 数据包申请 ******************************/

linkg_packet_t *linkg_packet_pool_alloc(linkg_packet_pool_t *pool);
uint32_t        linkg_packet_pool_alloc_batch(linkg_packet_pool_t *pool, linkg_packet_t **packets, uint32_t packet_count);

/****************************** 引用管理 ******************************/

void linkg_packet_retain(linkg_packet_t *packet);
void linkg_packet_release(linkg_packet_t *packet);
void linkg_packet_pool_release_batch(linkg_packet_pool_t *pool, linkg_packet_t **packets, uint32_t packet_count);

/****************************** 数据访问 ******************************/

static inline uint8_t *linkg_packet_data(linkg_packet_t *packet)
{
    return packet->slot + packet->data_offset;
}

static inline const uint8_t *linkg_packet_const_data(const linkg_packet_t *packet)
{
    return packet->slot + packet->data_offset;
}

static inline uint32_t linkg_packet_capacity(const linkg_packet_t *packet)
{
    return packet->pool->slot_size - packet->data_offset;
}

static inline uint8_t *linkg_packet_pull(linkg_packet_t *packet, uint32_t data_size)
{
    if (packet == NULL || packet->slot == NULL || data_size > packet->data_length)
    {
        return NULL;
    }

    packet->data_offset += data_size;
    packet->data_length -= data_size;

    return linkg_packet_data(packet);
}

static inline uint32_t linkg_packet_headroom(const linkg_packet_t *packet)
{
    return packet->data_offset;
}

void *linkg_packet_push(linkg_packet_t *packet, uint32_t header_size);

/****************************** 业务分类 ******************************/

static inline bool linkg_packet_is_realtime(const linkg_packet_t *packet)
{
    if (packet == NULL)
    {
        return false;
    }

    return (packet->flags & LINKG_PACKET_FLAG_REALTIME) != 0U;
}

static inline void linkg_packet_set_realtime(linkg_packet_t *packet, bool realtime)
{
    if (packet == NULL)
    {
        return;
    }

    if (realtime)
    {
        packet->flags &= ~LINKG_PACKET_FLAG_VIDEO;
        packet->flags |= LINKG_PACKET_FLAG_REALTIME;
    }
    else
    {
        packet->flags &= ~LINKG_PACKET_FLAG_REALTIME;
    }
}

static inline bool linkg_packet_is_video(const linkg_packet_t *packet)
{
    if (packet == NULL)
    {
        return false;
    }

    return (packet->flags & LINKG_PACKET_FLAG_VIDEO) != 0U;
}

static inline void linkg_packet_set_video(linkg_packet_t *packet, bool video)
{
    if (packet == NULL)
    {
        return;
    }

    if (video)
    {
        packet->flags &= ~LINKG_PACKET_FLAG_REALTIME;
        packet->flags |= LINKG_PACKET_FLAG_VIDEO;
    }
    else
    {
        packet->flags &= ~LINKG_PACKET_FLAG_VIDEO;
    }
}

static inline bool linkg_packet_is_data(const linkg_packet_t *packet)
{
    uint32_t traffic_flags;

    if (packet == NULL)
    {
        return false;
    }

    traffic_flags = LINKG_PACKET_FLAG_REALTIME |
                    LINKG_PACKET_FLAG_VIDEO;

    return (packet->flags & traffic_flags) == 0U;
}

static inline void linkg_packet_set_data(linkg_packet_t *packet)
{
    if (packet == NULL)
    {
        return;
    }

    packet->flags &= ~(LINKG_PACKET_FLAG_REALTIME |
                       LINKG_PACKET_FLAG_VIDEO);
}

#ifdef __cplusplus
}
#endif

#endif
