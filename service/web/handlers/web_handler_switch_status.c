/**
 * @file web_handler_switch_status.c
 * @brief LinkG Web切换运行状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-19
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_json.h"
#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_switch_status.h"

/****************************** 协议转换 ******************************/

/**
 * @brief 将设备角色转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_role_string(linkg_device_role_t role)
{
    if (role == LINKG_DEVICE_ROLE_AP)
    {
        return "ap";
    }

    if (role == LINKG_DEVICE_ROLE_STA)
    {
        return "sta";
    }

    return NULL;
}

/**
 * @brief 将发送模式转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_send_mode_string(linkg_send_mode_t mode)
{
    if (mode == LINKG_SEND_MODE_NONE)
    {
        return "none";
    }

    if (mode == LINKG_SEND_MODE_SINGLE)
    {
        return "single";
    }

    if (mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return "redundant";
    }

    return "unknown";
}

/**
 * @brief 将Switch状态接入类型转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_access_string(linkg_switch_status_access_t access)
{
    if (access == LINKG_SWITCH_STATUS_ACCESS_WIFI)
    {
        return "wifi";
    }

    if (access == LINKG_SWITCH_STATUS_ACCESS_CELLULAR)
    {
        return "cellular";
    }

    if (access == LINKG_SWITCH_STATUS_ACCESS_NONE)
    {
        return "none";
    }

    return "unknown";
}

/**
 * @brief 将Plan中的Link标识转换为Web接入类型字符串。
 */
static const char *_linkg_web_switch_status_link_access_string(uint32_t link_id)
{
    uint32_t cellular_link_id;
    uint32_t wifi_link_id;

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return "none";
    }

    wifi_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    if (wifi_link_id != LINKG_LINK_ID_INVALID && link_id == wifi_link_id)
    {
        return "wifi";
    }

    if (cellular_link_id != LINKG_LINK_ID_INVALID && link_id == cellular_link_id)
    {
        return "cellular";
    }

    return "unknown";
}

/**
 * @brief 将Wi-Fi工作模式转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_work_mode_string(linkg_wifi_work_mode_t mode)
{
    if (mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        return "narrow";
    }

    if (mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        return "wide";
    }

    return "unknown";
}

/**
 * @brief 将Wi-Fi窄带速率控制模式转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_narrow_mode_string(linkg_wifi_narrow_mode_t mode)
{
    if (mode == LINKG_WIFI_NARROW_MODE_FIXED)
    {
        return "fixed";
    }

    if (mode == LINKG_WIFI_NARROW_MODE_ADAPTIVE)
    {
        return "adaptive";
    }

    return "unknown";
}

/**
 * @brief 将Transport业务类别转换为Web协议字符串。
 */
static const char *_linkg_web_switch_status_transport_class_string(uint32_t traffic_class)
{
    if (traffic_class == LINKG_TRANSPORT_CLASS_REALTIME)
    {
        return "realtime";
    }

    if (traffic_class == LINKG_TRANSPORT_CLASS_VIDEO)
    {
        return "video";
    }

    if (traffic_class == LINKG_TRANSPORT_CLASS_DATA)
    {
        return "data";
    }

    return "unknown";
}

/****************************** JSON辅助 ******************************/

/**
 * @brief 将JSON模块错误转换为Web Handler错误。
 */
static int _linkg_web_switch_status_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/**
 * @brief 向JSON对象写入无符号64位整数。
 */
static int _linkg_web_switch_status_json_add_uint64(cJSON *object, const char *key, uint64_t value)
{
    cJSON *item;

    if (object == NULL || key == NULL || key[0] == '\0')
    {
        return LINKG_JSON_ERR_PARAM;
    }

    item = cJSON_AddNumberToObject(object, key, (double)value);
    if (item == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 计算状态字段相对本次快照的更新时间，单位毫秒。
 */
static uint64_t _linkg_web_switch_status_age_ms(uint64_t collected_us, uint64_t updated_us)
{
    if (updated_us == 0U || updated_us > collected_us)
    {
        return 0U;
    }

    return (collected_us - updated_us) / 1000U;
}

/**
 * @brief 向JSON对象写入Plan链路端点。
 */
static int _linkg_web_switch_status_json_add_plan_link(cJSON *parent, const char *key, uint32_t link_id)
{
    const char *access;
    cJSON      *link;
    int         ret;

    link = cJSON_CreateObject();
    if (link == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    access = _linkg_web_switch_status_link_access_string(link_id);

    ret = linkg_json_add_string(link, "access", access);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(link, "configured", link_id != LINKG_LINK_ID_INVALID);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        if (cJSON_AddNullToObject(link, "link_id") == NULL)
        {
            ret = LINKG_JSON_ERR_MEMORY;
            goto error;
        }
    }
    else
    {
        ret = linkg_json_add_uint32(link, "link_id", link_id);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    ret = linkg_json_add_object(parent, key, link);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(link);

    return ret;
}

/**
 * @brief 向JSON对象写入发送计划。
 */
static int _linkg_web_switch_status_json_add_plan(cJSON *parent, const char *key, const linkg_send_plan_t *source, bool valid)
{
    linkg_send_plan_t plan;
    cJSON            *object;
    int               ret;

    if (parent == NULL || key == NULL || source == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    plan = *source;

    if (!valid)
    {
        plan.mode              = LINKG_SEND_MODE_NONE;
        plan.primary_link_id   = LINKG_LINK_ID_INVALID;
        plan.secondary_link_id = LINKG_LINK_ID_INVALID;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "valid", valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "mode", _linkg_web_switch_status_send_mode_string(plan.mode));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_plan_link(object, "primary", plan.primary_link_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_plan_link(object, "secondary", plan.secondary_link_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON对象写入Maintenance状态。
 */
static int _linkg_web_switch_status_json_add_maintenance(cJSON *parent, const char *key, const linkg_switch_maintenance_status_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || source == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "active", source->active);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "access", _linkg_web_switch_status_access_string(source->access));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "message_id", source->message_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "started_us", source->started_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "elapsed_ms", _linkg_web_switch_status_age_ms(collected_us, source->started_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON对象写入丢包状态。
 */
static int _linkg_web_switch_status_json_add_loss(cJSON *parent, const char *key, const linkg_switch_status_loss_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "valid", source->valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "loss_permille", source->loss_permille);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "sample_packets", source->sample_packets);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "updated_us", source->updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "age_ms", _linkg_web_switch_status_age_ms(collected_us, source->updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON对象写入业务流量状态。
 */
static int _linkg_web_switch_status_json_add_traffic(cJSON *parent, const linkg_switch_status_traffic_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "valid", source->valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "tx_pps", source->tx_pps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "rx_pps", source->rx_pps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "tx_bps", source->tx_bps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "rx_bps", source->rx_bps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "updated_us", source->updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "age_ms", _linkg_web_switch_status_age_ms(collected_us, source->updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, "traffic", object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON数组写入单个业务Probe状态。
 */
static int _linkg_web_switch_status_json_add_probe(cJSON *array, uint32_t traffic_class, const linkg_switch_status_probe_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "class", _linkg_web_switch_status_transport_class_string(traffic_class));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "valid", source->valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "reachable", source->reachable);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "rtt_us", source->rtt_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "updated_us", source->updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "age_ms", _linkg_web_switch_status_age_ms(collected_us, source->updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (!cJSON_AddItemToArray(array, object))
    {
        ret = LINKG_JSON_ERR_MEMORY;
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON对象写入全部业务Probe状态。
 */
static int _linkg_web_switch_status_json_add_probes(cJSON *parent, const linkg_switch_status_probe_t *source, uint64_t collected_us)
{
    cJSON   *array;
    uint32_t index;
    int      ret;

    array = cJSON_CreateArray();
    if (array == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        ret = _linkg_web_switch_status_json_add_probe(array, index, &source[index], collected_us);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(array);
            return ret;
        }
    }

    ret = linkg_json_add_array(parent, "probes", array);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(array);
        return ret;
    }

    return LINKG_JSON_OK;
}

/****************************** STA状态 ******************************/

/**
 * @brief 向JSON对象写入STA Wi-Fi运行状态。
 */
static int _linkg_web_switch_status_json_add_sta_wifi(cJSON *parent, const linkg_switch_sta_wifi_status_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "available", source->available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "radio_valid", source->radio_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "connected", source->connected);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "statistics_valid", source->statistics_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_int(object, "rssi_dbm", source->rssi_dbm);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "noise_valid", source->noise_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_int(object, "noise_dbm", source->noise_dbm);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "tx_phy_kbps", source->tx_phy_kbps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "rx_phy_kbps", source->rx_phy_kbps);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "inactive_ms", source->inactive_ms);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "work_mode", _linkg_web_switch_status_work_mode_string(source->work_mode));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint16(object, "bandwidth_mhz", source->bandwidth_mhz);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "narrow_mode", _linkg_web_switch_status_narrow_mode_string(source->narrow_mode));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "rate_level_valid", source->rate_level_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint16(object, "rate_level", source->rate_level);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "temperature_valid", source->temperature_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_int(object, "temperature_c", source->temperature_c);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "status_updated_us", source->status_updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "statistics_updated_us", source->statistics_updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "status_age_ms", _linkg_web_switch_status_age_ms(collected_us, source->status_updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "statistics_age_ms", _linkg_web_switch_status_age_ms(collected_us, source->statistics_updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_probes(object, source->probe, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_traffic(object, &source->traffic, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_loss(object, "uplink_loss", &source->uplink_loss, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_loss(object, "downlink_loss", &source->downlink_loss, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, "wifi", object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON对象写入STA Cellular备用状态。
 */
static int _linkg_web_switch_status_json_add_sta_cellular(cJSON *parent, const linkg_switch_sta_cellular_status_t *source, uint64_t collected_us)
{
    cJSON *object;
    int    ret;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "status_valid", source->updated_us != 0U);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "available", source->available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "updated_us", source->updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(object, "age_ms", _linkg_web_switch_status_age_ms(collected_us, source->updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(parent, "cellular", object);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 向JSON根对象写入STA角色运行状态。
 */
static int _linkg_web_switch_status_json_add_sta(cJSON *data, const linkg_switch_sta_status_t *source, uint64_t collected_us)
{
    cJSON *sta;
    int    ret;

    sta = cJSON_CreateObject();
    if (sta == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(sta, "peer_present", source->peer_present);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(sta, "observation_valid", source->observation_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(sta, "peer_node_id", source->peer_node_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(sta, "observation_updated_us", source->observation_updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(sta, "observation_age_ms", _linkg_web_switch_status_age_ms(collected_us, source->observation_updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_plan(sta, "plan", &source->plan, source->peer_present);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_sta_wifi(sta, &source->wifi, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_sta_cellular(sta, &source->cellular, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_maintenance(sta, "remote_maintenance", &source->remote_maintenance, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(data, "sta", sta);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(sta);

    return ret;
}

/****************************** AP状态 ******************************/

/**
 * @brief 向JSON数组写入单个AP直连STA状态。
 */
static int _linkg_web_switch_status_json_add_ap_peer(cJSON *array, const linkg_switch_ap_peer_status_t *source, uint64_t collected_us)
{
    cJSON *peer;
    int    ret;

    peer = cJSON_CreateObject();
    if (peer == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = linkg_json_add_uint32(peer, "node_id", source->peer_node_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_plan(peer, "plan", &source->plan, true);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(peer, "wifi_path_available", source->wifi_path_available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(peer, "cellular_path_available", source->cellular_path_available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(peer, "path_status_valid", source->path_updated_us != 0U);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(peer, "path_updated_us", source->path_updated_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(peer, "path_age_ms", _linkg_web_switch_status_age_ms(collected_us, source->path_updated_us));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_loss(peer, "wifi_uplink_loss", &source->wifi_uplink_loss, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_maintenance(peer, "remote_maintenance", &source->remote_maintenance, collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (!cJSON_AddItemToArray(array, peer))
    {
        ret = LINKG_JSON_ERR_MEMORY;
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(peer);

    return ret;
}

/**
 * @brief 向JSON根对象写入AP角色运行状态。
 */
static int _linkg_web_switch_status_json_add_ap(cJSON *data, const linkg_switch_ap_status_t *source, uint64_t collected_us)
{
    cJSON   *ap;
    cJSON   *peers;
    uint32_t index;
    int      ret;

    if (data == NULL || source == NULL || source->peer_count > LINKG_RESOURCE_NETWORK_STA_MAX)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    ap = cJSON_CreateObject();
    if (ap == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    peers = NULL;

    ret = linkg_json_add_uint32(ap, "peer_count", source->peer_count);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    peers = cJSON_CreateArray();
    if (peers == NULL)
    {
        ret = LINKG_JSON_ERR_MEMORY;
        goto error;
    }

    for (index = 0U; index < source->peer_count; index++)
    {
        ret = _linkg_web_switch_status_json_add_ap_peer(peers, &source->peers[index], collected_us);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    ret = linkg_json_add_array(ap, "peers", peers);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    peers = NULL;

    ret = linkg_json_add_object(data, "ap", ap);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(peers);
    cJSON_Delete(ap);

    return ret;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Switch运行状态查询。
 */
int _linkg_web_handler_switch_status_get(const cJSON *param, char **response)
{
    const linkg_switch_maintenance_status_t *local_maintenance;
    linkg_switch_status_t                    status;
    const char                              *role;
    cJSON                                   *data;
    int                                      ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_switch_get_status(&status);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SWITCH_STATUS_GET, "获取Switch运行状态失败", response);
    }

    role = _linkg_web_switch_status_role_string(status.role);
    if (role == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SWITCH_STATUS_GET, "Switch设备角色无效", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_uint64(data, "collected_us", status.collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (status.role == LINKG_DEVICE_ROLE_STA)
    {
        local_maintenance = &status.data.sta.local_maintenance;

        ret = _linkg_web_switch_status_json_add_sta(data, &status.data.sta, status.collected_us);
    }
    else
    {
        local_maintenance = &status.data.ap.local_maintenance;

        ret = _linkg_web_switch_status_json_add_ap(data, &status.data.ap, status.collected_us);
    }

    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_switch_status_json_add_maintenance(data, "local_maintenance", local_maintenance, status.collected_us);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_SWITCH_STATUS_GET, data, NULL, response);

error:
    cJSON_Delete(data);

    return _linkg_web_switch_status_json_error(ret);
}
