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

/****************************** 生命周期 ******************************/

int linkg_switch_init(void);
int linkg_switch_deinit(void);

/****************************** 计划管理 ******************************/

int linkg_switch_set_plan(uint8_t peer_node_id, const linkg_send_plan_t *plan);
int linkg_switch_get_plan(uint8_t peer_node_id, linkg_send_plan_t *plan);
int linkg_switch_remove_plan(uint8_t peer_node_id);

#ifdef __cplusplus
}
#endif

#endif
