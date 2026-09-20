/**
 * @file wal_temperature_ioctl.h
 * @brief HI1105芯片温度私有ioctl ABI
 */

#ifndef WAL_TEMPERATURE_IOCTL_H
#define WAL_TEMPERATURE_IOCTL_H

#ifndef __KERNEL__
#include <stdint.h>
#endif

/****************************** 接口配置 ******************************/

#define WAL_TEMPERATURE_IOCTL       (0x89F0 + 7) // 芯片温度私有ioctl命令
#define WAL_TEMPERATURE_ABI_VERSION 1U           // 接口ABI版本

/****************************** 温度状态 ******************************/

typedef struct
{
    uint16_t version;       // 用户态请求的ABI版本
    uint16_t struct_size;   // 用户态缓冲区大小
    int32_t  temperature_c; // HI1105芯片结温，单位摄氏度
} wal_temperature_status_stru;

#endif
