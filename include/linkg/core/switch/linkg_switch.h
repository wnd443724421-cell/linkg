/**
 * @file linkg_switch.h
 * @brief LinkG链路切换状态接口
 */

#ifndef LINKG_SWITCH_H
#define LINKG_SWITCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "linkg_link.h"

/****************************** 发送模式 ******************************/

typedef enum
{
    LINKG_SEND_MODE_NONE = 0, // 无可用链路
    LINKG_SEND_MODE_SINGLE,   // 单链路发送
    LINKG_SEND_MODE_REDUNDANT // 双链路冗余发送
} linkg_send_mode_t;

/****************************** 发送计划 ******************************/

typedef struct
{
    linkg_send_mode_t mode;              // 当前发送模式
    uint32_t          primary_link_id;   // 当前主链路
    uint32_t          secondary_link_id; // 当前备用链路，可无效
} linkg_send_plan_t;

/****************************** 前置声明 ******************************/

typedef struct linkg_packet_pool linkg_packet_pool_t; // LinkG数据包内存池

/****************************** 生命周期 ******************************/

int linkg_switch_init(linkg_packet_pool_t *packet_pool);
int linkg_switch_start(void);
int linkg_switch_stop(void);
int linkg_switch_deinit(void);

/****************************** 运行控制 ******************************/

int linkg_switch_wakeup(void);

/****************************** 计划管理 ******************************/

int linkg_switch_set_plan(uint8_t peer_node_id, const linkg_send_plan_t *plan);
int linkg_switch_get_plan(uint8_t peer_node_id, linkg_send_plan_t *plan);
int linkg_switch_remove_plan(uint8_t peer_node_id);

/****************************** 接入维护 ******************************/

int linkg_switch_begin_maintenance(linkg_link_access_t access);
int linkg_switch_end_maintenance(linkg_link_access_t access);

#ifdef __cplusplus
}
#endif

#endif
