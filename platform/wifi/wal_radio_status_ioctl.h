/**
 * @file wal_radio_status_ioctl.h
 * @brief HI1105无线接口及对端状态私有ioctl ABI
 */

#ifndef WAL_RADIO_STATUS_IOCTL_H
#define WAL_RADIO_STATUS_IOCTL_H

#ifndef __KERNEL__
#include <stdint.h>
#endif

/****************************** 接口配置 ******************************/

#define WAL_RADIO_STATUS_IOCTL              (0x89F0 + 5) // 无线状态私有ioctl命令
#define WAL_RADIO_STATUS_ABI_VERSION        1U           // 接口ABI版本
#define WAL_RADIO_STATUS_MAC_LENGTH         6U           // 无线MAC地址长度
#define WAL_RADIO_STATUS_MAX_PEERS          16U          // 单次返回的最大对端数量
#define WAL_RADIO_STATUS_REFRESH_MS         250U         // 状态缓存刷新周期，单位毫秒

/****************************** 整体状态标志 ******************************/

#define WAL_RADIO_FLAG_NOISE_VALID          (1U << 0)    // 噪声强度有效
#define WAL_RADIO_FLAG_PEERS_TRUNCATED      (1U << 1)    // 对端列表已被截断
#define WAL_RADIO_FLAG_PARTIAL              (1U << 2)    // 部分状态查询失败

/****************************** 对端状态标志 ******************************/

#define WAL_RADIO_PEER_FLAG_VALID           (1U << 0)    // 对端基础信息有效
#define WAL_RADIO_PEER_FLAG_QUERY_FAILED    (1U << 1)    // 对端详细状态查询失败

/****************************** 无线角色 ******************************/

#define WAL_RADIO_ROLE_UNKNOWN              0U           // 未知角色
#define WAL_RADIO_ROLE_STA                  1U           // STA角色
#define WAL_RADIO_ROLE_AP                   2U           // AP角色

/****************************** 接口状态 ******************************/

#define WAL_RADIO_STATE_DOWN                0U           // 无线接口未运行
#define WAL_RADIO_STATE_READY               1U           // 无线接口已运行但无关联对端
#define WAL_RADIO_STATE_CONNECTED           2U           // 至少存在一个关联对端

/****************************** 对端状态 ******************************/

/**
 * @brief 无线对端运行状态。
 *
 * @note 速率单位为kbps，所有统计量均从本机视角描述。
 */
typedef struct
{
    uint8_t  mac[WAL_RADIO_STATUS_MAC_LENGTH]; // 对端MAC地址
    int8_t   rssi_dbm;                         // 本机接收对端的信号强度，单位dBm
    uint8_t  flags;                            // 对端状态标志

    uint64_t driver_tx_bytes;                  // 驱动统计：本机向对端累计发送字节
    uint64_t driver_rx_bytes;                  // 驱动统计：本机从对端累计接收字节
    uint64_t driver_tx_packets;                // 驱动统计：本机向对端累计发送包数
    uint64_t driver_rx_packets;                // 驱动统计：本机从对端累计接收包数
    uint64_t driver_tx_failed;                 // 驱动统计：本机向对端累计发送失败次数

    uint32_t inactive_ms;                      // 对端空闲时间，单位毫秒
    uint32_t tx_rate_kbps;                     // 本机向对端发送的PHY速率
    uint32_t rx_rate_kbps;                     // 本机接收对端数据的PHY速率
    uint32_t connected_time_s;                 // 当前连接持续时间，单位秒
} wal_radio_peer_status_stru;

/****************************** 无线状态 ******************************/

/**
 * @brief 本机无线接口及全部关联对端的单次状态快照。
 *
 * @note AP模式下peers保存关联STA，STA模式下peers[0]保存关联AP。
 * @note 调用前必须填写version和struct_size，其余字段清零。
 */
typedef struct
{
    uint16_t                   version;                                // 用户态请求的ABI版本
    uint16_t                   struct_size;                            // 用户态缓冲区大小
    uint8_t                    role;                                   // 本机无线角色
    uint8_t                    state;                                  // 本机无线接口状态
    uint8_t                    peer_count;                             // 当前返回的有效对端数量
    uint8_t                    reserved0;                              // 保留字段
    uint8_t                    local_mac[WAL_RADIO_STATUS_MAC_LENGTH]; // 本机无线MAC地址
    uint8_t                    reserved1[2];                           // 保留字段
    uint32_t                   flags;                                  // 整体状态标志
    uint32_t                   frequency_mhz;                          // 当前工作频率，单位MHz
    uint16_t                   channel;                                // 当前工作信道
    uint16_t                   bandwidth_mhz;                          // 当前工作带宽，单位MHz
    int32_t                    noise_dbm;                              // 本机噪声强度，单位dBm
    wal_radio_peer_status_stru peers[WAL_RADIO_STATUS_MAX_PEERS];      // 当前关联对端列表
} wal_radio_status_stru;
#endif
