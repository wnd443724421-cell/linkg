/**
 * @file wal_tx_flowctrl_ioctl.h
 * @brief HI1105发送流控状态私有ioctl ABI
 */

#ifndef WAL_TX_FLOWCTRL_IOCTL_H
#define WAL_TX_FLOWCTRL_IOCTL_H

#ifndef __KERNEL__
#include <stdint.h>
#endif

/****************************** 接口配置 ******************************/

#define WAL_TX_FLOWCTRL_IOCTL       (0x89F0 + 6) // 发送流控状态私有ioctl命令
#define WAL_TX_FLOWCTRL_ABI_VERSION 1U           // 接口ABI版本

/****************************** 流控状态 ******************************/

typedef struct
{
    uint16_t version;            // ABI版本
    uint16_t struct_size;        // 用户态缓冲区大小

    uint32_t vi_queue_length;    // Host HCC VI发送队列深度
    uint32_t vo_queue_length;    // Host HCC VO发送队列深度
    uint32_t be_queue_length;    // Host HCC BE发送队列深度

    uint32_t flowctrl_off_count; // Device FLOWCTRL_OFF累计次数

    uint8_t  tx_allowed;         // 当前是否允许继续向Device提交数据
    uint8_t  reserved[3];        // 保留字段
} wal_tx_flowctrl_status_stru;

#endif
