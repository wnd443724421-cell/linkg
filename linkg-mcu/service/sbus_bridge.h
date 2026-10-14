/**
 * @file sbus_bridge.h
 * @brief 透明桥接生命周期与累计统计
 */

#ifndef LINKG_SBUS_BRIDGE_H
#define LINKG_SBUS_BRIDGE_H           // 头文件保护

#include <stdint.h>

#include "bsp_uart.h"

/****************************** 模块常量 ******************************/

#define SBUS_BRIDGE_FIFO_CAPACITY 32U // 每个方向独立的字节容量

/****************************** 类型定义 ******************************/

typedef enum
{
    SBUS_BRIDGE_ISOLATED = 0, // 两条路径均关闭
    SBUS_BRIDGE_BYPASS   = 1, // U16 直通，MCU 数据引脚高阻
    SBUS_BRIDGE_RUNNING  = 2, // 双 UART 桥接运行
    SBUS_BRIDGE_FAULT    = 3  // 初始化失败，保持隔离并限频重试
} sbus_bridge_state_t;

typedef struct
{
    uint32_t rx_bytes;         // 从源 UART 读出的字节总数，含硬件报错字节
    uint32_t tx_bytes;         // 已写入目标 TDR 的字节数，不代表远端确认
    uint32_t overflow_bytes;   // FIFO 满时丢弃的新字节数
    uint32_t bad_rx_bytes;     // PE、FE、NE 标记的不可信字节数
    uint32_t parity_errors;    // 奇偶校验错误事件
    uint32_t frame_errors;     // 停止位错误事件
    uint32_t noise_errors;     // 噪声错误事件
    uint32_t overrun_errors;   // ORE 事件数，丢失字节数无法精确恢复
    uint32_t discarded_bytes;  // 停止时清理的软件队列字节数
    uint32_t tx_abort_events;  // 停止时 TC 未完成事件，可能截断在途字节
    uint32_t tx_failures;      // 非忙错误导致的发送失败事件
    uint16_t queued_bytes;     // 当前软件队列长度
    uint16_t high_water_bytes; // 队列历史最大长度
} sbus_bridge_direction_stats_t;

typedef struct
{
    sbus_bridge_direction_stats_t direction[BSP_UART_COUNT]; // 按源端口索引的双向统计
    volatile sbus_bridge_state_t  state;                     // 当前通路状态
    uint32_t                      mode_changes;              // 成功进入运行或直通的次数
    uint32_t                      start_failures;            // UART 启动失败次数
    int32_t                       last_error;                // 最近一次负 errno 错误
} sbus_bridge_stats_t;

/****************************** 生命周期 ******************************/

int  sbus_bridge_init(void);
void sbus_bridge_process(void);
void sbus_bridge_uart_irq(bsp_uart_port_t port);

/****************************** 状态查询 ******************************/

int sbus_bridge_get_stats(sbus_bridge_stats_t *stats);

#endif
