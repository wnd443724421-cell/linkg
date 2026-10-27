/**
 * @file switch_wire.h
 * @brief LinkG链路切换Wire协议定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-16
 */

#ifndef SWITCH_WIRE_H
#define SWITCH_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "linkg_transport_types.h"

/****************************** 协议常量 ******************************/

#define LINKG_SWITCH_WIRE_VERSION                      1U                                                                               // 当前Switch Wire协议版本
#define LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID           0U                                                                               // 无效消息编号

#define LINKG_SWITCH_WIRE_HEADER_SIZE                  8U                                                                               // Switch公共头Wire长度
#define LINKG_SWITCH_WIRE_QUALITY_HCC_SIZE             20U                                                                              // AP本机Wi-Fi HCC状态Wire长度
#define LINKG_SWITCH_WIRE_QUALITY_REPORT_PAYLOAD_SIZE  (80U + LINKG_SWITCH_WIRE_QUALITY_HCC_SIZE)                                       // 原有80字节质量数据加20字节HCC
#define LINKG_SWITCH_WIRE_CLASS_PLAN_SIZE              4U                                                                               // 单业务发送计划Wire长度
#define LINKG_SWITCH_WIRE_PLAN_SYNC_PAYLOAD_SIZE       (LINKG_SWITCH_WIRE_CLASS_PLAN_SIZE * LINKG_TRANSPORT_CLASS_COUNT)                // 完整Peer发送计划同步Payload长度
#define LINKG_SWITCH_WIRE_PLAN_ACK_PAYLOAD_SIZE        4U                                                                               // 发送计划确认Payload长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_PAYLOAD_SIZE     4U                                                                               // Maintenance控制Payload长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_PAYLOAD_SIZE 4U                                                                               // Maintenance结束确认Payload长度

#define LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE          (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_QUALITY_REPORT_PAYLOAD_SIZE)  // 质量报告长度
#define LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE               (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_PLAN_SYNC_PAYLOAD_SIZE)       // 计划同步长度
#define LINKG_SWITCH_WIRE_PLAN_ACK_SIZE                (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_PLAN_ACK_PAYLOAD_SIZE)        // 计划确认长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_SIZE             (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_MAINTENANCE_PAYLOAD_SIZE)     // 维护长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE         (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_MAINTENANCE_ACK_PAYLOAD_SIZE) // 维护确认长度
#define LINKG_SWITCH_WIRE_MAX_SIZE                     LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE                                            // 最大长度


#define LINKG_SWITCH_WIRE_HCC_FLAG_VALID               0x00000001U                                                                      // HCC采样有效
#define LINKG_SWITCH_WIRE_HCC_FLAG_TX_ALLOWED          0x00000002U                                                                      // HCC允许Host向Device发送
#define LINKG_SWITCH_WIRE_HCC_FLAG_MASK                (LINKG_SWITCH_WIRE_HCC_FLAG_VALID | LINKG_SWITCH_WIRE_HCC_FLAG_TX_ALLOWED)       // 当前支持的全部HCC标志

/****************************** 协议约束 ******************************/

_Static_assert(LINKG_TRANSPORT_CLASS_REALTIME == 0, "invalid realtime class index");
_Static_assert(LINKG_TRANSPORT_CLASS_VIDEO == 1, "invalid video class index");
_Static_assert(LINKG_TRANSPORT_CLASS_DATA == 2, "invalid data class index");
_Static_assert(LINKG_TRANSPORT_CLASS_COUNT == 3, "invalid transport class count");
_Static_assert(LINKG_SWITCH_WIRE_MAX_SIZE >= LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE, "invalid switch wire max size");
_Static_assert(LINKG_SWITCH_WIRE_MAX_SIZE >= LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE, "invalid plan sync max size");
_Static_assert(LINKG_SWITCH_WIRE_MAX_SIZE >= LINKG_SWITCH_WIRE_PLAN_ACK_SIZE, "invalid plan ack max size");
_Static_assert(LINKG_SWITCH_WIRE_MAX_SIZE >= LINKG_SWITCH_WIRE_MAINTENANCE_SIZE, "invalid maintenance max size");
_Static_assert(LINKG_SWITCH_WIRE_MAX_SIZE >= LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE, "invalid maintenance ack max size");
_Static_assert(LINKG_SWITCH_WIRE_QUALITY_HCC_SIZE == 20U, "invalid HCC wire size");

_Static_assert(LINKG_SWITCH_WIRE_QUALITY_REPORT_PAYLOAD_SIZE == 100U, "invalid quality report payload size");

_Static_assert(LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE == 108U, "invalid quality report wire size");

_Static_assert((LINKG_SWITCH_WIRE_HCC_FLAG_VALID & LINKG_SWITCH_WIRE_HCC_FLAG_TX_ALLOWED) == 0U, "overlapping HCC flags");
/****************************** 消息类型 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_TYPE_NONE            = 0, // 无效消息
    LINKG_SWITCH_WIRE_TYPE_QUALITY_REPORT  = 1, // AP向STA上报链路质量
    LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC       = 2, // STA向AP同步完整Plan
    LINKG_SWITCH_WIRE_TYPE_PLAN_ACK        = 3, // AP确认Plan同步
    LINKG_SWITCH_WIRE_TYPE_MAINTENANCE     = 4, // 接入维护开始或结束
    LINKG_SWITCH_WIRE_TYPE_MAINTENANCE_ACK = 5, // 接入维护结束确认
    LINKG_SWITCH_WIRE_TYPE_COUNT           = 6  // 消息类型数量
} linkg_switch_wire_type_t;

/****************************** 接入类型 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_ACCESS_NONE = 0, // 无效接入类型
    LINKG_SWITCH_WIRE_ACCESS_WIFI,     // Wi-Fi接入
    LINKG_SWITCH_WIRE_ACCESS_CELLULAR, // Cellular接入
    LINKG_SWITCH_WIRE_ACCESS_COUNT     // 接入类型数量
} linkg_switch_wire_access_t;

/****************************** 发送模式 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_MODE_NONE = 0,  // 当前业务无可用链路
    LINKG_SWITCH_WIRE_MODE_SINGLE,    // 单链路发送
    LINKG_SWITCH_WIRE_MODE_REDUNDANT, // 双链路冗余发送
    LINKG_SWITCH_WIRE_MODE_COUNT      // 发送模式数量
} linkg_switch_wire_mode_t;

/****************************** Maintenance阶段 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_NONE = 0, // 无效阶段
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN,    // 指定Access进入维护
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END,      // 指定Access退出维护
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_COUNT     // Maintenance阶段数量
} linkg_switch_wire_maintenance_phase_t;

/****************************** 公共头 ******************************/

typedef struct
{
    uint8_t                  version;    // Switch Wire协议版本
    linkg_switch_wire_type_t type;       // Switch模块消息类型
    uint16_t                 length;     // 完整Switch消息Wire长度
    uint32_t                 message_id; // 非零消息编号或事务编号
} linkg_switch_wire_header_t;

/****************************** 质量上报 ******************************/

/**
 * @brief 单项当前窗口丢包质量报告。
 *
 * lost_packets为当前窗口实际确认丢失的报文数量。
 * sample_packets为当前窗口总样本数量，包含成功与丢失报文。
 * 样本数为0时，丢包数量必须为0。
 */
typedef struct
{
    uint32_t lost_packets;   // 当前窗口实际丢包数量
    uint32_t sample_packets; // 当前窗口总样本数量
} linkg_switch_wire_loss_t;

/****************************** HCC质量上报 ******************************/

typedef struct
{
    uint32_t vo_queue_length;    // AP本机VO实时业务发送队列长度
    uint32_t vi_queue_length;    // AP本机VI视频业务发送队列长度
    uint32_t be_queue_length;    // AP本机BE普通业务发送队列长度
    uint32_t flowctrl_off_count; // AP本机Device FLOWCTRL_OFF累计次数
    uint32_t flags;              // HCC状态有效性及原始发送允许标志
} linkg_switch_wire_hcc_t;

/**
 * @brief AP向指定STA上报Wi-Fi和Cellular质量。
 *
 * Wire由AP接收端根据各业务独立Sequence窗口统计。
 * 保留整链路Wire统计，同时增加三业务独立统计。
 *
 * Load由AP发送端根据Path累计统计计算。
 * Wi-Fi和Cellular统一采用整链路TX Queue Overflow。
 *
 * STA负责自行采集另外四组本地方向的Wire和Load观测。
 */
typedef struct
{
    linkg_switch_wire_loss_t wifi_uplink;            // STA到AP Wi-Fi整链路Wire丢包
    linkg_switch_wire_loss_t wifi_downlink_load;     // AP到STA Wi-Fi整链路Load丢包

    linkg_switch_wire_loss_t cellular_uplink;        // STA到AP Cellular整链路Wire丢包
    linkg_switch_wire_loss_t cellular_downlink_load; // AP到STA Cellular整链路Load丢包

    linkg_switch_wire_loss_t wifi_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];     // STA到AP Wi-Fi三业务独立Wire丢包
    linkg_switch_wire_loss_t cellular_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT]; // STA到AP Cellular三业务独立Wire丢包

    linkg_switch_wire_hcc_t  wifi_downlink_hcc;      // AP到STA Wi-Fi发送侧HCC队列及全局流控状态

} linkg_switch_wire_quality_report_t;

/****************************** 发送计划同步 ******************************/

typedef struct
{
    linkg_switch_wire_mode_t   mode;             // 当前业务生效发送模式
    linkg_switch_wire_access_t primary_access;   // 当前业务主发送接入
    linkg_switch_wire_access_t secondary_access; // 当前业务备用发送接入
} linkg_switch_wire_class_send_plan_t;

typedef struct
{
    linkg_switch_wire_class_send_plan_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 一次同步全部业务计划
} linkg_switch_wire_plan_sync_t;

/****************************** 发送计划确认 ******************************/

typedef struct
{
    int32_t status; // AP应用对应发送计划的结果，0成功，负errno失败
} linkg_switch_wire_plan_ack_t;

/****************************** Maintenance通知 ******************************/

typedef struct
{
    linkg_switch_wire_access_t            access; // 当前进入或退出维护的接入类型
    linkg_switch_wire_maintenance_phase_t phase;  // Maintenance开始或结束
} linkg_switch_wire_maintenance_t;

/****************************** Maintenance确认 ******************************/

typedef struct
{
    linkg_switch_wire_access_t access; // 已完成END处理的接入类型
} linkg_switch_wire_maintenance_ack_t;

/****************************** 解码结果 ******************************/

typedef struct
{
    linkg_switch_wire_header_t header; // 已校验并转换为主机字节序的公共头

    union
    {
        linkg_switch_wire_quality_report_t  quality_report;  // Wi-Fi和Cellular质量上报
        linkg_switch_wire_plan_sync_t       plan_sync;       // 发送计划同步
        linkg_switch_wire_plan_ack_t        plan_ack;        // 发送计划确认
        linkg_switch_wire_maintenance_t     maintenance;     // Maintenance开始或结束通知
        linkg_switch_wire_maintenance_ack_t maintenance_ack; // Maintenance结束确认
    } payload;
} linkg_switch_wire_message_t;

/****************************** Wire操作 ******************************/

int linkg_switch_wire_encode_quality_report(uint32_t message_id, const linkg_switch_wire_quality_report_t *report, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_plan_sync(uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_plan_ack(uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_maintenance(uint32_t message_id, const linkg_switch_wire_maintenance_t *maintenance, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_maintenance_ack(uint32_t message_id, const linkg_switch_wire_maintenance_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_decode(const uint8_t *buffer, size_t length, linkg_switch_wire_message_t *message);

#endif
