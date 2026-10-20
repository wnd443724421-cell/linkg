/**
 * @file switch_wire.c
 * @brief LinkG链路切换Wire协议编解码实现
 * @author Dawn
 * @version 3.0.0
 * @date 2026-10-16
 */

#include "switch_wire.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/****************************** Wire偏移 ******************************/

#define LINKG_SWITCH_WIRE_HEADER_VERSION_OFFSET                 0U                                    // 公共头版本偏移
#define LINKG_SWITCH_WIRE_HEADER_TYPE_OFFSET                    1U                                    // 公共头类型偏移
#define LINKG_SWITCH_WIRE_HEADER_LENGTH_OFFSET                  2U                                    // 公共头长度偏移
#define LINKG_SWITCH_WIRE_HEADER_MESSAGE_ID_OFFSET              4U                                    // 公共头消息编号偏移

#define LINKG_SWITCH_WIRE_QUALITY_LOSS_SIZE                     8U                                    // 单项丢包统计Wire长度
#define LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_OFFSET            LINKG_SWITCH_WIRE_HEADER_SIZE         // 质量Wi-Fi上行偏移
#define LINKG_SWITCH_WIRE_QUALITY_WIFI_DOWNLINK_LOAD_OFFSET     (LINKG_SWITCH_WIRE_HEADER_SIZE + 8U)  // 质量Wi-Fi下行负载偏移
#define LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_OFFSET        (LINKG_SWITCH_WIRE_HEADER_SIZE + 16U) // 质量Cellular上行偏移
#define LINKG_SWITCH_WIRE_QUALITY_CELLULAR_DOWNLINK_LOAD_OFFSET (LINKG_SWITCH_WIRE_HEADER_SIZE + 24U) // 质量Cellular下行负载偏移
#define LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_CLASSES_OFFSET    (LINKG_SWITCH_WIRE_HEADER_SIZE + 32U) // Wi-Fi上行三业务Wire偏移
#define LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_CLASSES_OFFSET (LINKG_SWITCH_WIRE_HEADER_SIZE + 56U) // Cellular上行三业务Wire偏移

#define LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET                 LINKG_SWITCH_WIRE_HEADER_SIZE         // 计划同步模式偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET              (LINKG_SWITCH_WIRE_HEADER_SIZE + 1U)  // 计划同步主用接入偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET            (LINKG_SWITCH_WIRE_HEADER_SIZE + 2U)  // 计划同步备用接入偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET             (LINKG_SWITCH_WIRE_HEADER_SIZE + 3U)  // 计划同步保留字段偏移

#define LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET                LINKG_SWITCH_WIRE_HEADER_SIZE         // 计划确认状态偏移

#define LINKG_SWITCH_WIRE_MAINTENANCE_ACCESS_OFFSET             LINKG_SWITCH_WIRE_HEADER_SIZE         // 维护接入类型偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_OFFSET              (LINKG_SWITCH_WIRE_HEADER_SIZE + 1U)  // 维护阶段偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED0_OFFSET          (LINKG_SWITCH_WIRE_HEADER_SIZE + 2U)  // 维护保留字段0偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED1_OFFSET          (LINKG_SWITCH_WIRE_HEADER_SIZE + 3U)  // 维护保留字段1偏移

#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_ACCESS_OFFSET         LINKG_SWITCH_WIRE_HEADER_SIZE         // 维护确认接入类型偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED0_OFFSET      (LINKG_SWITCH_WIRE_HEADER_SIZE + 1U)  // 维护确认保留字段0偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED1_OFFSET      (LINKG_SWITCH_WIRE_HEADER_SIZE + 2U)  // 维护确认保留字段1偏移
#define LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED2_OFFSET      (LINKG_SWITCH_WIRE_HEADER_SIZE + 3U)  // 维护确认保留字段2偏移

_Static_assert(LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_CLASSES_OFFSET ==
               LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_CLASSES_OFFSET +
               LINKG_SWITCH_WIRE_QUALITY_LOSS_SIZE * LINKG_TRANSPORT_CLASS_COUNT,
               "invalid Switch quality report class offsets");
_Static_assert(LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE ==
               LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_CLASSES_OFFSET +
               LINKG_SWITCH_WIRE_QUALITY_LOSS_SIZE * LINKG_TRANSPORT_CLASS_COUNT,
               "invalid Switch quality report wire length");

/****************************** 基础编解码 ******************************/

/**
 * @brief 按网络字节序写入16位无符号整数。
 */
static void _linkg_switch_wire_write_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8U);
    buffer[1] = (uint8_t)value;
}

/**
 * @brief 按网络字节序写入32位无符号整数。
 */
static void _linkg_switch_wire_write_u32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24U);
    buffer[1] = (uint8_t)(value >> 16U);
    buffer[2] = (uint8_t)(value >> 8U);
    buffer[3] = (uint8_t)value;
}

/**
 * @brief 按网络字节序读取16位无符号整数。
 */
static uint16_t _linkg_switch_wire_read_u16(const uint8_t *buffer)
{
    return (uint16_t)(((uint16_t)buffer[0] << 8U) |
                      (uint16_t)buffer[1]);
}

/**
 * @brief 按网络字节序读取32位无符号整数。
 */
static uint32_t _linkg_switch_wire_read_u32(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24U) |
           ((uint32_t)buffer[1] << 16U) |
           ((uint32_t)buffer[2] << 8U) |
           (uint32_t)buffer[3];
}

/**
 * @brief 将Wire中的32位补码值转换为有符号状态码。
 */
static int32_t _linkg_switch_wire_decode_s32(uint32_t value)
{
    if (value <= (uint32_t)INT32_MAX)
    {
        return (int32_t)value;
    }

    return -(int32_t)(UINT32_MAX - value) - 1;
}

/****************************** 字段校验 ******************************/

/**
 * @brief 判断Switch消息类型是否合法。
 */
static bool _linkg_switch_wire_type_valid(linkg_switch_wire_type_t type)
{
    return type > LINKG_SWITCH_WIRE_TYPE_NONE &&
           type < LINKG_SWITCH_WIRE_TYPE_COUNT;
}

/**
 * @brief 判断Switch接入类型是否合法。
 */
static bool _linkg_switch_wire_access_valid(linkg_switch_wire_access_t access)
{
    return access > LINKG_SWITCH_WIRE_ACCESS_NONE &&
           access < LINKG_SWITCH_WIRE_ACCESS_COUNT;
}

/**
 * @brief 判断Switch发送模式是否合法。
 */
static bool _linkg_switch_wire_mode_valid(linkg_switch_wire_mode_t mode)
{
    return mode > LINKG_SWITCH_WIRE_MODE_NONE &&
           mode < LINKG_SWITCH_WIRE_MODE_COUNT;
}

/**
 * @brief 判断Maintenance阶段是否合法。
 */
static bool _linkg_switch_wire_maintenance_phase_valid(linkg_switch_wire_maintenance_phase_t phase)
{
    return phase > LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_NONE &&
           phase < LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_COUNT;
}

/**
 * @brief 校验发送计划Wire字段组合。
 */
static int _linkg_switch_wire_validate_class_plan(const linkg_switch_wire_class_send_plan_t *plan)
{
    if (plan == NULL)
    {
        return -EINVAL;
    }

    if (plan->mode == LINKG_SWITCH_WIRE_MODE_NONE)
    {
        return plan->primary_access == LINKG_SWITCH_WIRE_ACCESS_NONE && plan->secondary_access == LINKG_SWITCH_WIRE_ACCESS_NONE ? 0 : -EINVAL;
    }

    if (!_linkg_switch_wire_mode_valid(plan->mode) ||
        !_linkg_switch_wire_access_valid(plan->primary_access))
    {
        return -EINVAL;
    }

    if (plan->secondary_access != LINKG_SWITCH_WIRE_ACCESS_NONE &&
        !_linkg_switch_wire_access_valid(plan->secondary_access))
    {
        return -EINVAL;
    }

    if (plan->secondary_access == plan->primary_access)
    {
        return -EINVAL;
    }

    if (plan->mode == LINKG_SWITCH_WIRE_MODE_REDUNDANT &&
        plan->secondary_access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验完整Peer发送计划，拒绝任一非法业务字段。
 */
static int _linkg_switch_wire_validate_plan(const linkg_switch_wire_plan_sync_t *plan)
{
    uint32_t class_index;
    int      ret;

    if (plan == NULL)
    {
        return -EINVAL;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        ret = _linkg_switch_wire_validate_class_plan(&plan->classes[class_index]);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 校验Maintenance Wire字段组合。
 */
static int _linkg_switch_wire_validate_maintenance(const linkg_switch_wire_maintenance_t *maintenance)
{
    if (maintenance == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_access_valid(maintenance->access))
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_maintenance_phase_valid(maintenance->phase))
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验Maintenance ACK Wire字段组合。
 */
static int _linkg_switch_wire_validate_maintenance_ack(const linkg_switch_wire_maintenance_ack_t *ack)
{
    if (ack == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_access_valid(ack->access))
    {
        return -EINVAL;
    }

    return 0;
}

/****************************** 公共头 ******************************/

/**
 * @brief 编码Switch公共Wire头。
 */
static int _linkg_switch_wire_encode_header(linkg_switch_wire_type_t type, uint16_t wire_length, uint32_t message_id, uint8_t *buffer, size_t capacity)
{
    if (buffer == NULL ||
        !_linkg_switch_wire_type_valid(type) ||
        message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return -EINVAL;
    }

    if (capacity < wire_length)
    {
        return -ENOSPC;
    }

    buffer[LINKG_SWITCH_WIRE_HEADER_VERSION_OFFSET] = LINKG_SWITCH_WIRE_VERSION;
    buffer[LINKG_SWITCH_WIRE_HEADER_TYPE_OFFSET]    = (uint8_t)type;

    _linkg_switch_wire_write_u16(&buffer[LINKG_SWITCH_WIRE_HEADER_LENGTH_OFFSET],
                                 wire_length);

    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_HEADER_MESSAGE_ID_OFFSET],
                                 message_id);

    return 0;
}

/**
 * @brief 解码并校验Switch公共Wire头。
 */
static int _linkg_switch_wire_decode_header(const uint8_t *buffer, size_t length, linkg_switch_wire_header_t *header)
{
    uint8_t  wire_type;
    uint16_t wire_length;

    if (buffer == NULL || header == NULL)
    {
        return -EINVAL;
    }

    memset(header, 0, sizeof(*header));

    if (length < LINKG_SWITCH_WIRE_HEADER_SIZE ||
        length > UINT16_MAX)
    {
        return -EMSGSIZE;
    }

    header->version = buffer[LINKG_SWITCH_WIRE_HEADER_VERSION_OFFSET];
    if (header->version != LINKG_SWITCH_WIRE_VERSION)
    {
        return -EPROTONOSUPPORT;
    }

    wire_type = buffer[LINKG_SWITCH_WIRE_HEADER_TYPE_OFFSET];
    if (wire_type <= LINKG_SWITCH_WIRE_TYPE_NONE ||
        wire_type >= LINKG_SWITCH_WIRE_TYPE_COUNT)
    {
        return -EPROTONOSUPPORT;
    }

    wire_length = _linkg_switch_wire_read_u16(&buffer[LINKG_SWITCH_WIRE_HEADER_LENGTH_OFFSET]);
    if ((size_t)wire_length != length)
    {
        return -EMSGSIZE;
    }

    header->type       = (linkg_switch_wire_type_t)wire_type;
    header->length     = wire_length;
    header->message_id = _linkg_switch_wire_read_u32(&buffer[LINKG_SWITCH_WIRE_HEADER_MESSAGE_ID_OFFSET]);

    if (header->message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return -EINVAL;
    }

    return 0;
}

/****************************** 质量上报 ******************************/

/**
 * @brief 校验单项丢包质量指标。
 *
 * 实际丢包数量不能超过当前窗口总样本数。
 * 零样本只允许对应零丢包。
 */
static bool _linkg_switch_wire_loss_valid(const linkg_switch_wire_loss_t *loss)
{
    if (loss == NULL)
    {
        return false;
    }

    return loss->lost_packets <= loss->sample_packets;
}

/**
 * @brief 按网络字节序编码单项丢包质量指标，占8字节。
 */
static void _linkg_switch_wire_encode_loss(uint8_t *buffer, const linkg_switch_wire_loss_t *loss)
{
    _linkg_switch_wire_write_u32(buffer,      loss->lost_packets);
    _linkg_switch_wire_write_u32(buffer + 4U, loss->sample_packets);
}

/**
 * @brief 按网络字节序解码单项丢包质量指标，占8字节。
 */
static void _linkg_switch_wire_decode_loss(const uint8_t *buffer, linkg_switch_wire_loss_t *loss)
{
    loss->lost_packets   = _linkg_switch_wire_read_u32(buffer);
    loss->sample_packets = _linkg_switch_wire_read_u32(buffer + 4U);
}

/**
 * @brief 校验整链路指标及三业务独立Wire丢包指标。
 */
static bool _linkg_switch_wire_quality_valid(const linkg_switch_wire_quality_report_t *report)
{
    uint32_t class_index;

    if (report == NULL ||
        !_linkg_switch_wire_loss_valid(&report->wifi_uplink) ||
        !_linkg_switch_wire_loss_valid(&report->wifi_downlink_load) ||
        !_linkg_switch_wire_loss_valid(&report->cellular_uplink) ||
        !_linkg_switch_wire_loss_valid(&report->cellular_downlink_load))
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (!_linkg_switch_wire_loss_valid(&report->wifi_uplink_classes[class_index]) ||
            !_linkg_switch_wire_loss_valid(&report->cellular_uplink_classes[class_index]))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 编码AP到STA的Wi-Fi与Cellular质量上报消息。
 */
int linkg_switch_wire_encode_quality_report(uint32_t message_id, const linkg_switch_wire_quality_report_t *report, uint8_t *buffer, size_t capacity, size_t *length)
{
    uint32_t class_index;
    uint32_t offset;
    int      ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (report == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_quality_valid(report))
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_QUALITY_REPORT,
                                           LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_OFFSET],
                                   &report->wifi_uplink);
    _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_DOWNLINK_LOAD_OFFSET],
                                   &report->wifi_downlink_load);
    _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_OFFSET],
                                   &report->cellular_uplink);
    _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_DOWNLINK_LOAD_OFFSET],
                                   &report->cellular_downlink_load);

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        offset = class_index * LINKG_SWITCH_WIRE_QUALITY_LOSS_SIZE;

        _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_CLASSES_OFFSET + offset],
                                       &report->wifi_uplink_classes[class_index]);
        _linkg_switch_wire_encode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_CLASSES_OFFSET + offset],
                                       &report->cellular_uplink_classes[class_index]);
    }

    *length = LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE;

    return 0;
}

/****************************** 发送计划同步 ******************************/

/**
 * @brief 编码STA到AP的发送计划同步消息。
 */
int linkg_switch_wire_encode_plan_sync(uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan, uint8_t *buffer, size_t capacity, size_t *length)
{
    uint32_t class_index;
    uint32_t offset;
    int      ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (plan == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_validate_plan(plan);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC,
                                           LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        offset = class_index * LINKG_SWITCH_WIRE_CLASS_PLAN_SIZE;
        buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET + offset]      = (uint8_t)plan->classes[class_index].mode;
        buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET + offset]   = (uint8_t)plan->classes[class_index].primary_access;
        buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET + offset] = (uint8_t)plan->classes[class_index].secondary_access;
        buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET + offset]  = 0U;
    }

    *length = LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE;

    return 0;
}

/****************************** 发送计划确认 ******************************/

/**
 * @brief 编码AP到STA的发送计划同步确认消息。
 */
int linkg_switch_wire_encode_plan_ack(uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length)
{
    int ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (ack == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (ack->status > 0)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_PLAN_ACK,
                                           LINKG_SWITCH_WIRE_PLAN_ACK_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET],
                                 (uint32_t)ack->status);

    *length = LINKG_SWITCH_WIRE_PLAN_ACK_SIZE;

    return 0;
}

/****************************** Maintenance通知 ******************************/

/**
 * @brief 编码Maintenance BEGIN或END通知消息。
 */
int linkg_switch_wire_encode_maintenance(uint32_t message_id, const linkg_switch_wire_maintenance_t *maintenance, uint8_t *buffer, size_t capacity, size_t *length)
{
    int ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (maintenance == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_validate_maintenance(maintenance);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_MAINTENANCE,
                                           LINKG_SWITCH_WIRE_MAINTENANCE_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACCESS_OFFSET]    = (uint8_t)maintenance->access;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_OFFSET]     = (uint8_t)maintenance->phase;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED0_OFFSET] = 0U;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED1_OFFSET] = 0U;

    *length = LINKG_SWITCH_WIRE_MAINTENANCE_SIZE;

    return 0;
}

/****************************** Maintenance确认 ******************************/

/**
 * @brief 编码Maintenance END处理确认消息。
 */
int linkg_switch_wire_encode_maintenance_ack(uint32_t message_id, const linkg_switch_wire_maintenance_ack_t *ack, uint8_t *buffer, size_t capacity, size_t *length)
{
    int ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (ack == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_validate_maintenance_ack(ack);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_MAINTENANCE_ACK,
                                           LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_ACCESS_OFFSET]    = (uint8_t)ack->access;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED0_OFFSET] = 0U;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED1_OFFSET] = 0U;
    buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED2_OFFSET] = 0U;

    *length = LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE;

    return 0;
}

/****************************** Wire解码 ******************************/

/**
 * @brief 解码并严格校验一条Switch Wire消息。
 */
int linkg_switch_wire_decode(const uint8_t *buffer, size_t length, linkg_switch_wire_message_t *message)
{
    uint32_t                             class_index;
    uint32_t                             offset;
    linkg_switch_wire_maintenance_t     *maintenance;
    linkg_switch_wire_maintenance_ack_t *maintenance_ack;
    linkg_switch_wire_plan_sync_t       *plan;
    uint32_t                             status_value;
    int                                  ret;

    if (buffer == NULL || message == NULL)
    {
        return -EINVAL;
    }

    memset(message, 0, sizeof(*message));

    ret = _linkg_switch_wire_decode_header(buffer,
                                           length,
                                           &message->header);
    if (ret != 0)
    {
        return ret;
    }

    switch (message->header.type)
    {
        case LINKG_SWITCH_WIRE_TYPE_QUALITY_REPORT:
        {
            linkg_switch_wire_quality_report_t *report;

            if (length != LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE)
            {
                return -EMSGSIZE;
            }

            report = &message->payload.quality_report;

            _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_OFFSET],
                                           &report->wifi_uplink);
            _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_DOWNLINK_LOAD_OFFSET],
                                           &report->wifi_downlink_load);
            _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_OFFSET],
                                           &report->cellular_uplink);
            _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_DOWNLINK_LOAD_OFFSET],
                                           &report->cellular_downlink_load);

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                offset = class_index * LINKG_SWITCH_WIRE_QUALITY_LOSS_SIZE;

                _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_WIFI_UPLINK_CLASSES_OFFSET + offset],
                                               &report->wifi_uplink_classes[class_index]);
                _linkg_switch_wire_decode_loss(&buffer[LINKG_SWITCH_WIRE_QUALITY_CELLULAR_UPLINK_CLASSES_OFFSET + offset],
                                               &report->cellular_uplink_classes[class_index]);
            }

            if (!_linkg_switch_wire_quality_valid(report))
            {
                return -EINVAL;
            }

            break;
        }

        case LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC:
        {
            if (length != LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE)
            {
                return -EMSGSIZE;
            }

            plan = &message->payload.plan_sync;

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                offset = class_index * LINKG_SWITCH_WIRE_CLASS_PLAN_SIZE;
                if (buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET + offset] != 0U)
                {
                    return -EINVAL;
                }

                plan->classes[class_index].mode             = (linkg_switch_wire_mode_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET + offset];
                plan->classes[class_index].primary_access   = (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET + offset];
                plan->classes[class_index].secondary_access = (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET + offset];
            }

            ret = _linkg_switch_wire_validate_plan(plan);
            if (ret != 0)
            {
                return ret;
            }

            break;
        }

        case LINKG_SWITCH_WIRE_TYPE_PLAN_ACK:
        {
            if (length != LINKG_SWITCH_WIRE_PLAN_ACK_SIZE)
            {
                return -EMSGSIZE;
            }

            status_value =
                _linkg_switch_wire_read_u32(&buffer[LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET]);

            message->payload.plan_ack.status =
                _linkg_switch_wire_decode_s32(status_value);

            if (message->payload.plan_ack.status > 0)
            {
                return -EINVAL;
            }

            break;
        }

        case LINKG_SWITCH_WIRE_TYPE_MAINTENANCE:
        {
            if (length != LINKG_SWITCH_WIRE_MAINTENANCE_SIZE)
            {
                return -EMSGSIZE;
            }

            if (buffer[LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED0_OFFSET] != 0U ||
                buffer[LINKG_SWITCH_WIRE_MAINTENANCE_RESERVED1_OFFSET] != 0U)
            {
                return -EINVAL;
            }

            maintenance = &message->payload.maintenance;

            maintenance->access =
                (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACCESS_OFFSET];

            maintenance->phase =
                (linkg_switch_wire_maintenance_phase_t)buffer[LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_OFFSET];

            ret = _linkg_switch_wire_validate_maintenance(maintenance);
            if (ret != 0)
            {
                return ret;
            }

            break;
        }

        case LINKG_SWITCH_WIRE_TYPE_MAINTENANCE_ACK:
        {
            if (length != LINKG_SWITCH_WIRE_MAINTENANCE_ACK_SIZE)
            {
                return -EMSGSIZE;
            }

            if (buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED0_OFFSET] != 0U ||
                buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED1_OFFSET] != 0U ||
                buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_RESERVED2_OFFSET] != 0U)
            {
                return -EINVAL;
            }

            maintenance_ack = &message->payload.maintenance_ack;

            maintenance_ack->access =
                (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_MAINTENANCE_ACK_ACCESS_OFFSET];

            ret = _linkg_switch_wire_validate_maintenance_ack(maintenance_ack);
            if (ret != 0)
            {
                return ret;
            }

            break;
        }

        default:
        {
            return -EPROTONOSUPPORT;
        }
    }

    return 0;
}
