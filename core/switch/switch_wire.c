/**
 * @file switch_wire.c
 * @brief LinkG链路切换Wire协议编解码实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_wire.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/****************************** Wire偏移 ******************************/

#define LINKG_SWITCH_WIRE_HEADER_VERSION_OFFSET             0U                                   // 公共头协议版本偏移
#define LINKG_SWITCH_WIRE_HEADER_TYPE_OFFSET                1U                                   // 公共头消息类型偏移
#define LINKG_SWITCH_WIRE_HEADER_LENGTH_OFFSET              2U                                   // 公共头消息长度偏移
#define LINKG_SWITCH_WIRE_HEADER_MESSAGE_ID_OFFSET          4U                                   // 公共头消息编号偏移
#define LINKG_SWITCH_WIRE_WIFI_QUALITY_LOSS_OFFSET          LINKG_SWITCH_WIRE_HEADER_SIZE        // Wi-Fi质量上报丢包率偏移
#define LINKG_SWITCH_WIRE_WIFI_QUALITY_SAMPLE_OFFSET        (LINKG_SWITCH_WIRE_HEADER_SIZE + 4U) // Wi-Fi质量上报样本数量偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET             LINKG_SWITCH_WIRE_HEADER_SIZE        // 发送计划同步模式偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET          (LINKG_SWITCH_WIRE_HEADER_SIZE + 1U) // 发送计划同步主接入偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET        (LINKG_SWITCH_WIRE_HEADER_SIZE + 2U) // 发送计划同步备用接入偏移
#define LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET         (LINKG_SWITCH_WIRE_HEADER_SIZE + 3U) // 发送计划同步保留字段偏移
#define LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET            LINKG_SWITCH_WIRE_HEADER_SIZE        // 发送计划确认状态码偏移

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
 * @brief 将Wire中的32位补码转换为有符号状态码。
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
 * @brief 校验发送计划Wire字段组合。
 */
static int _linkg_switch_wire_validate_plan(const linkg_switch_wire_plan_sync_t *plan)
{
    if (plan == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_mode_valid(plan->mode))
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_access_valid(plan->primary_access))
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

/****************************** 公共头 ******************************/

/**
 * @brief 编码Switch公共Wire头。
 */
static int _linkg_switch_wire_encode_header(linkg_switch_wire_type_t type, uint16_t wire_length, uint32_t message_id, uint8_t *buffer, size_t capacity)
{
    if (buffer == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_wire_type_valid(type))
    {
        return -EINVAL;
    }

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return -EINVAL;
    }

    if (capacity < wire_length)
    {
        return -ENOSPC;
    }

    buffer[LINKG_SWITCH_WIRE_HEADER_VERSION_OFFSET] = LINKG_SWITCH_WIRE_VERSION;
    buffer[LINKG_SWITCH_WIRE_HEADER_TYPE_OFFSET]    = (uint8_t)type;

    _linkg_switch_wire_write_u16(&buffer[LINKG_SWITCH_WIRE_HEADER_LENGTH_OFFSET], wire_length);
    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_HEADER_MESSAGE_ID_OFFSET], message_id);

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

    if (length < LINKG_SWITCH_WIRE_HEADER_SIZE || length > UINT16_MAX)
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

/****************************** Wi-Fi质量上报 ******************************/

/**
 * @brief 编码AP到STA的Wi-Fi上行质量上报消息。
 */
int linkg_switch_wire_encode_wifi_quality_report(uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report, uint8_t *buffer, size_t capacity, size_t *length)
{
    int ret;

    if (length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (report == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (report->loss_permille > 1000U)
    {
        return -EINVAL;
    }

    if (report->sample_packets == 0U && report->loss_permille != 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_wire_encode_header(LINKG_SWITCH_WIRE_TYPE_WIFI_QUALITY_REPORT,
                                           LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_SIZE,
                                           message_id,
                                           buffer,
                                           capacity);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_WIFI_QUALITY_LOSS_OFFSET], report->loss_permille);
    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_WIFI_QUALITY_SAMPLE_OFFSET], report->sample_packets);

    *length = LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_SIZE;

    return 0;
}

/****************************** 发送计划同步 ******************************/

/**
 * @brief 编码STA到AP的发送计划同步消息。
 */
int linkg_switch_wire_encode_plan_sync(uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan, uint8_t *buffer, size_t capacity, size_t *length)
{
    int ret;

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

    buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET]      = (uint8_t)plan->mode;
    buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET]   = (uint8_t)plan->primary_access;
    buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET] = (uint8_t)plan->secondary_access;
    buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET]  = 0U;

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

    _linkg_switch_wire_write_u32(&buffer[LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET], (uint32_t)ack->status);

    *length = LINKG_SWITCH_WIRE_PLAN_ACK_SIZE;

    return 0;
}

/****************************** Wire解码 ******************************/

/**
 * @brief 解码并严格校验一条Switch Wire消息。
 */
int linkg_switch_wire_decode(const uint8_t *buffer, size_t length, linkg_switch_wire_message_t *message)
{
    linkg_switch_wire_plan_sync_t *plan;
    uint32_t                       status_value;
    int                            ret;

    if (buffer == NULL || message == NULL)
    {
        return -EINVAL;
    }

    memset(message, 0, sizeof(*message));

    ret = _linkg_switch_wire_decode_header(buffer, length, &message->header);
    if (ret != 0)
    {
        return ret;
    }

    switch (message->header.type)
    {
        case LINKG_SWITCH_WIRE_TYPE_WIFI_QUALITY_REPORT:
        {
            if (length != LINKG_SWITCH_WIRE_WIFI_QUALITY_REPORT_SIZE)
            {
                return -EMSGSIZE;
            }

            message->payload.wifi_quality_report.loss_permille =
                _linkg_switch_wire_read_u32(&buffer[LINKG_SWITCH_WIRE_WIFI_QUALITY_LOSS_OFFSET]);

            message->payload.wifi_quality_report.sample_packets =
                _linkg_switch_wire_read_u32(&buffer[LINKG_SWITCH_WIRE_WIFI_QUALITY_SAMPLE_OFFSET]);

            if (message->payload.wifi_quality_report.loss_permille > 1000U)
            {
                return -EINVAL;
            }

            if (message->payload.wifi_quality_report.sample_packets == 0U &&
                message->payload.wifi_quality_report.loss_permille != 0U)
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

            if (buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_RESERVED_OFFSET] != 0U)
            {
                return -EINVAL;
            }

            plan = &message->payload.plan_sync;

            plan->mode             = (linkg_switch_wire_mode_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_MODE_OFFSET];
            plan->primary_access   = (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_PRIMARY_OFFSET];
            plan->secondary_access = (linkg_switch_wire_access_t)buffer[LINKG_SWITCH_WIRE_PLAN_SYNC_SECONDARY_OFFSET];

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

            status_value = _linkg_switch_wire_read_u32(&buffer[LINKG_SWITCH_WIRE_PLAN_ACK_STATUS_OFFSET]);

            message->payload.plan_ack.status = _linkg_switch_wire_decode_s32(status_value);

            if (message->payload.plan_ack.status > 0)
            {
                return -EINVAL;
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
