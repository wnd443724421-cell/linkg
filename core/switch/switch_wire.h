/**
 * @file switch_wire.h
 * @brief LinkG链路切换Wire协议定义
 * @author Dawn
 * @version 1.1.0
 * @date 2026-09-19
 */

#ifndef SWITCH_WIRE_H
#define SWITCH_WIRE_H

#include <stddef.h>
#include <stdint.h>

/****************************** 协议常量 ******************************/

#define LINKG_SWITCH_WIRE_VERSION                           1U  // 当前Switch Wire协议版本
#define LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID                0U  // 无效消息编号

#define LINKG_SWITCH_WIRE_HEADER_SIZE                       8U  // Switch公共头Wire长度
#define LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_PAYLOAD_SIZE  8U  // Wi-Fi质量上报Payload长度
#define LINKG_SWITCH_WIRE_PLAN_SYNC_PAYLOAD_SIZE            4U  // 发送计划同步Payload长度
#define LINKG_SWITCH_WIRE_PLAN_ACK_PAYLOAD_SIZE             4U  // 发送计划确认Payload长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_PAYLOAD_SIZE          4U  // Maintenance控制Payload长度
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_PAYLOAD_SIZE      4U  // Maintenance结束确认Payload长度

#define LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_SIZE          (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_PAYLOAD_SIZE)
#define LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE                    (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_PLAN_SYNC_PAYLOAD_SIZE)
#define LINKG_SWITCH_WIRE_PLAN_ACK_SIZE                     (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_PLAN_ACK_PAYLOAD_SIZE)
#define LINKG_SWITCH_WIRE_MAINTENANCE_SIZE                  (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_MAINTENANCE_PAYLOAD_SIZE)
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE              (LINKG_SWITCH_WIRE_HEADER_SIZE + LINKG_SWITCH_WIRE_MAINTENANCE_ACK_PAYLOAD_SIZE)
#define LINKG_SWITCH_WIRE_MAX_SIZE                          LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_SIZE

/****************************** 消息类型 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_TYPE_NONE = 0,            // 无效消息
    LINKG_SWITCH_WIRE_TYPE_WIFI_QUALITY_REPORT, // Wi-Fi上行质量上报
    LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC,           // STA发送计划同步
    LINKG_SWITCH_WIRE_TYPE_PLAN_ACK,            // AP发送计划同步确认
    LINKG_SWITCH_WIRE_TYPE_MAINTENANCE,         // 接入维护开始或结束通知
    LINKG_SWITCH_WIRE_TYPE_MAINTENANCE_ACK,     // 接入维护结束确认
    LINKG_SWITCH_WIRE_TYPE_COUNT                // 消息类型数量
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
    LINKG_SWITCH_WIRE_MODE_NONE = 0,  // 无效发送模式
    LINKG_SWITCH_WIRE_MODE_SINGLE,    // 单链路发送
    LINKG_SWITCH_WIRE_MODE_REDUNDANT, // 双链路冗余发送
    LINKG_SWITCH_WIRE_MODE_COUNT      // 发送模式数量
} linkg_switch_wire_mode_t;

/****************************** Maintenance阶段 ******************************/

typedef enum
{
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_NONE = 0, // 无效Maintenance阶段
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN,    // 指定Access开始进入维护
    LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END,      // 指定Access维护已经完成
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

/****************************** Wi-Fi质量上报 ******************************/

typedef struct
{
    uint32_t loss_permille;  // STA到AP Wi-Fi上行丢包率，0~1000
    uint32_t sample_packets; // 当前统计窗口参与计算的数据包数量
} linkg_switch_wire_wifi_quality_report_t;

/****************************** 发送计划同步 ******************************/

typedef struct
{
    linkg_switch_wire_mode_t   mode;             // STA当前生效发送模式
    linkg_switch_wire_access_t primary_access;   // STA当前主发送接入
    linkg_switch_wire_access_t secondary_access; // STA当前备用发送接入
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
    linkg_switch_wire_maintenance_phase_t phase;  // 当前Maintenance阶段
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
        linkg_switch_wire_wifi_quality_report_t wifi_quality_report; // Wi-Fi质量上报
        linkg_switch_wire_plan_sync_t           plan_sync;           // 发送计划同步
        linkg_switch_wire_plan_ack_t            plan_ack;            // 发送计划确认
        linkg_switch_wire_maintenance_t         maintenance;         // Maintenance开始或结束通知
        linkg_switch_wire_maintenance_ack_t     maintenance_ack;     // Maintenance结束确认
    } payload;
} linkg_switch_wire_message_t;

/****************************** Wire操作 ******************************/

int linkg_switch_wire_encode_wifi_quality_report(uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_plan_sync(uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_plan_ack(uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_maintenance(uint32_t message_id, const linkg_switch_wire_maintenance_t *maintenance, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_encode_maintenance_ack(uint32_t message_id, const linkg_switch_wire_maintenance_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length);
int linkg_switch_wire_decode(const uint8_t *buffer, size_t length, linkg_switch_wire_message_t *message);

#endif
