/**
 * @file web_handler_cellular.c
 * @brief LinkG Web蜂窝网络配置与状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-26
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_cellular_config.h"
#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_network.h"
#include "linkg_node.h"
#include "linkg_path_probe.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_CELLULAR_PROBE_MAX_AGE_MS 3000U // 周期Probe超过三个普通发送周期后不展示RTT

/****************************** 内部辅助 ******************************/

/**
 * @brief 将JSON模块错误转换为Web Handler错误。
 */
static int _linkg_web_cellular_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/**
 * @brief 判断Web管理的蜂窝网络配置是否完全相同。
 */
static bool _linkg_web_cellular_config_equal(const linkg_config_t *left, const linkg_config_t *right)
{
    const linkg_cellular_config_t *left_cellular;
    const linkg_cellular_config_t *right_cellular;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    if (left->paths.cellular.enabled != right->paths.cellular.enabled)
    {
        return false;
    }

    left_cellular  = &left->links.cellular;
    right_cellular = &right->links.cellular;

    return left_cellular->enabled == right_cellular->enabled &&
           left_cellular->network_mode == right_cellular->network_mode &&
           strcmp(left_cellular->apn, right_cellular->apn) == 0 &&
           strcmp(left_cellular->pin, right_cellular->pin) == 0;
}

/**
 * @brief 在蜂窝网络配置提交失败后恢复旧配置。
 */
static int _linkg_web_cellular_restore_config(const linkg_config_t *config, bool restore_network)
{
    int first_error;
    int ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    if (restore_network)
    {
        ret = linkg_network_set_cellular_config(&config->links.cellular);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    ret = linkg_config_save(config, NULL);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = linkg_config_replace(config);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 状态协议转换 ******************************/

/**
 * @brief 将蜂窝网络选择模式转换为Web协议字符串。
 */
static const char *_linkg_web_cellular_network_mode_string(linkg_cellular_network_mode_t mode)
{
    if (mode == LINKG_CELLULAR_NETWORK_MODE_AUTO)
    {
        return "auto";
    }

    if (mode == LINKG_CELLULAR_NETWORK_MODE_4G)
    {
        return "4g";
    }

    if (mode == LINKG_CELLULAR_NETWORK_MODE_5G)
    {
        return "5g";
    }

    return "unknown";
}

/**
 * @brief 将SIM状态转换为Web协议字符串。
 */
static const char *_linkg_web_cellular_sim_state_string(linkg_cellular_sim_state_t state)
{
    if (state == LINKG_CELLULAR_SIM_STATE_NOT_READY)
    {
        return "not_ready";
    }

    if (state == LINKG_CELLULAR_SIM_STATE_ABSENT)
    {
        return "absent";
    }

    if (state == LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED)
    {
        return "pin_required";
    }

    if (state == LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED)
    {
        return "puk_required";
    }

    if (state == LINKG_CELLULAR_SIM_STATE_READY)
    {
        return "ready";
    }

    return "unknown";
}

/**
 * @brief 将蜂窝网络注册状态转换为Web协议字符串。
 */
static const char *_linkg_web_cellular_registration_string(linkg_cellular_registration_state_t state)
{
    if (state == LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED)
    {
        return "not_registered";
    }

    if (state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING)
    {
        return "registering";
    }

    if (state == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return "registered";
    }

    if (state == LINKG_CELLULAR_REGISTRATION_STATE_DENIED)
    {
        return "denied";
    }

    return "unknown";
}

/**
 * @brief 将蜂窝网络类型转换为Web协议字符串。
 */
static const char *_linkg_web_cellular_network_type_string(linkg_cellular_network_type_t type)
{
    if (type == LINKG_CELLULAR_NETWORK_TYPE_LTE)
    {
        return "lte";
    }

    if (type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA)
    {
        return "5g_sa";
    }

    return "unknown";
}

/****************************** 状态JSON辅助 ******************************/

/**
 * @brief 判断周期Probe结果是否仍在允许的展示时间内。
 */
static bool _linkg_web_cellular_probe_is_fresh(const linkg_path_probe_class_snapshot_t *probe, uint64_t now_us)
{
    if (probe == NULL || probe->updated_us == 0U || probe->updated_us > now_us)
    {
        return false;
    }

    return now_us - probe->updated_us <= (uint64_t)LINKG_WEB_CELLULAR_PROBE_MAX_AGE_MS * 1000U;
}

/**
 * @brief 构造单个业务类别的周期Probe状态JSON。
 */
static int _linkg_web_cellular_status_build_probe_class(const linkg_path_probe_class_snapshot_t *probe, uint64_t now_us, cJSON **out)
{
    cJSON *object;
    bool   valid;
    bool   reachable;
    int    ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    valid     = probe != NULL && probe->valid && _linkg_web_cellular_probe_is_fresh(probe, now_us);
    reachable = valid && probe->reachable;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_bool(object, "valid", valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "reachable", reachable);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (reachable)
    {
        ret = linkg_json_add_uint32(object, "rtt_us", probe->rtt_us);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_cellular_json_error(ret);
}

/**
 * @brief 构造Cellular Path三业务类别周期Probe状态JSON。
 */
static int _linkg_web_cellular_status_build_probe(const linkg_path_probe_path_snapshot_t *path, uint64_t now_us, cJSON **out)
{
    const linkg_path_probe_class_snapshot_t *class_probe;
    cJSON                                   *object;
    cJSON                                   *class_object;
    int                                      ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    class_probe  = path != NULL ? &path->classes[LINKG_TRANSPORT_CLASS_REALTIME] : NULL;
    class_object = NULL;
    ret = _linkg_web_cellular_status_build_probe_class(class_probe, now_us, &class_object);
    if (ret != 0)
    {
        goto error;
    }

    ret = linkg_json_add_object(object, "realtime", class_object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(class_object);
        goto json_error;
    }

    class_probe  = path != NULL ? &path->classes[LINKG_TRANSPORT_CLASS_VIDEO] : NULL;
    class_object = NULL;
    ret = _linkg_web_cellular_status_build_probe_class(class_probe, now_us, &class_object);
    if (ret != 0)
    {
        goto error;
    }

    ret = linkg_json_add_object(object, "video", class_object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(class_object);
        goto json_error;
    }

    class_probe  = path != NULL ? &path->classes[LINKG_TRANSPORT_CLASS_DATA] : NULL;
    class_object = NULL;
    ret = _linkg_web_cellular_status_build_probe_class(class_probe, now_us, &class_object);
    if (ret != 0)
    {
        goto error;
    }

    ret = linkg_json_add_object(object, "data", class_object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(class_object);
        goto json_error;
    }

    *out = object;

    return 0;

json_error:
    ret = _linkg_web_cellular_json_error(ret);

error:
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 构造指定直接Peer的Cellular Path状态JSON。
 */
static int _linkg_web_cellular_status_build_path_peer(uint8_t node_id, uint64_t now_us, cJSON **out)
{
    linkg_path_probe_peer_snapshot_t       snapshot;
    const linkg_path_probe_path_snapshot_t *path;
    cJSON                                  *object;
    cJSON                                  *probe;
    bool                                    active;
    int                                     ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    memset(&snapshot, 0, sizeof(snapshot));
    ret = linkg_path_probe_get_peer_snapshot(node_id, &snapshot);

    if (ret == 0)
    {
        active = snapshot.cellular.active;
        path   = active ? &snapshot.cellular : NULL;
    }
    else if (ret == -ENOENT || ret == -ENODEV || ret == -ENETDOWN)
    {
        active = false;
        path   = NULL;
    }
    else
    {
        return ret;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_uint16(object, "node_id", node_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "active", active);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    probe = NULL;
    ret = _linkg_web_cellular_status_build_probe(path, now_us, &probe);
    if (ret != 0)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(object, "probe", probe);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(probe);
        goto error;
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_cellular_json_error(ret);
}

/**
 * @brief 构造当前全部直接Peer的Cellular Path状态JSON。
 */
static int _linkg_web_cellular_status_build_path(linkg_device_role_t role, bool path_enabled, uint64_t now_us, cJSON **out)
{
    linkg_node_peer_snapshot_t peer_snapshot;
    cJSON                     *object;
    cJSON                     *peers;
    cJSON                     *peer;
    uint32_t                   peer_count;
    uint32_t                   found_count;
    uint32_t                   node_id;
    int                        ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    peers  = cJSON_CreateArray();
    if (object == NULL || peers == NULL)
    {
        cJSON_Delete(object);
        cJSON_Delete(peers);
        return -ENOMEM;
    }

    if (path_enabled)
    {
        if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
        {
            ret = -EINVAL;
            goto error;
        }

        ret = linkg_node_get_peer_count(&peer_count);
        if (ret != 0)
        {
            goto error;
        }

        found_count = 0U;

        for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX && found_count < peer_count; node_id++)
        {
            ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &peer_snapshot);
            if (ret == -ENOENT)
            {
                continue;
            }

            if (ret != 0)
            {
                goto error;
            }

            found_count++;

            if ((role == LINKG_DEVICE_ROLE_AP && peer_snapshot.info.role != LINKG_DEVICE_ROLE_STA) ||
                (role == LINKG_DEVICE_ROLE_STA && peer_snapshot.info.role != LINKG_DEVICE_ROLE_AP))
            {
                continue;
            }

            peer = NULL;
            ret = _linkg_web_cellular_status_build_path_peer(peer_snapshot.info.node_id, now_us, &peer);
            if (ret != 0)
            {
                goto error;
            }

            if (!cJSON_AddItemToArray(peers, peer))
            {
                cJSON_Delete(peer);
                ret = -ENOMEM;
                goto error;
            }
        }
    }

    ret = linkg_json_add_array(object, "peers", peers);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(peers);
        goto json_error;
    }

    *out = object;

    return 0;

json_error:
    ret = _linkg_web_cellular_json_error(ret);

error:
    cJSON_Delete(peers);
    cJSON_Delete(object);

    return ret;
}

/**
 * @brief 构造蜂窝本机状态JSON。
 */
static int _linkg_web_cellular_status_build_local(const linkg_cellular_local_status_t *local, cJSON **out)
{
    cJSON *object;
    int    ret;

    if (local == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(object, "network_mode", _linkg_web_cellular_network_mode_string(local->network_mode));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "sim_state", _linkg_web_cellular_sim_state_string(local->sim_state));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_cellular_json_error(ret);
}

/**
 * @brief 构造蜂窝移动网络状态JSON。
 */
static int _linkg_web_cellular_status_build_network(const linkg_cellular_network_status_t *network, cJSON **out)
{
    const linkg_cellular_serving_cell_status_t *serving_cell;
    char                                         plmn[LINKG_CELLULAR_MCC_MAX_LENGTH + LINKG_CELLULAR_MNC_MAX_LENGTH + 1U];
    size_t                                       mcc_length;
    size_t                                       mnc_length;
    cJSON                                       *object;
    int                                          ret;

    if (network == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(object, "registration", _linkg_web_cellular_registration_string(network->registration));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "network_type", _linkg_web_cellular_network_type_string(network->network_type));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    serving_cell = &network->serving_cell;

    if (serving_cell->plmn_valid)
    {
        mcc_length = strlen(serving_cell->mcc);
        mnc_length = strlen(serving_cell->mnc);

        if (mcc_length + mnc_length < sizeof(plmn))
        {
            memcpy(plmn, serving_cell->mcc, mcc_length);
            memcpy(plmn + mcc_length, serving_cell->mnc, mnc_length + 1U);

            ret = linkg_json_add_string(object, "plmn", plmn);
            if (ret != LINKG_JSON_OK)
            {
                goto error;
            }
        }
    }

    if (serving_cell->valid)
    {
        ret = linkg_json_add_uint16(object, "band", serving_cell->band);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    if (serving_cell->rsrp_valid)
    {
        ret = linkg_json_add_int(object, "rsrp_dbm", serving_cell->rsrp_dbm);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    if (serving_cell->rsrq_valid)
    {
        ret = linkg_json_add_int(object, "rsrq_db", serving_cell->rsrq_db);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    if (serving_cell->sinr_valid)
    {
        ret = linkg_json_add_int(object, "sinr_db", serving_cell->sinr_db);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_cellular_json_error(ret);
}

/**
 * @brief 构造蜂窝主机地址状态JSON。
 */
static int _linkg_web_cellular_status_build_data(const linkg_cellular_data_status_t *data, cJSON **out)
{
    char   address[INET6_ADDRSTRLEN];
    cJSON *object;
    int    ret;

    if (data == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    if (data->ipv4_valid && inet_ntop(AF_INET, &data->ipv4, address, sizeof(address)) != NULL)
    {
        ret = linkg_json_add_string(object, "ipv4", address);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    if (data->global_ipv6_valid && inet_ntop(AF_INET6, &data->global_ipv6, address, sizeof(address)) != NULL)
    {
        ret = linkg_json_add_string(object, "ipv6", address);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_cellular_json_error(ret);
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理蜂窝网络配置查询。
 */
int _linkg_web_handler_cellular_config_get(const cJSON *param, char **response)
{
    linkg_config_t config;
    cJSON         *data;
    int            ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_GET, "获取全局配置失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_bool(data, "path_enabled", config.paths.cellular.enabled);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_cellular_config_to_json(data, "cellular", &config.links.cellular);
    if (ret != 0)
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_GET, "构造蜂窝网络配置失败", response);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_CELLULAR_CONFIG_GET, data, NULL, response);

error:
    cJSON_Delete(data);

    return _linkg_web_cellular_json_error(ret);
}

/**
 * @brief 处理蜂窝网络配置更新。
 */
int _linkg_web_handler_cellular_config_set(const cJSON *param, char **response)
{
    linkg_config_t old_config;
    linkg_config_t new_config;
    const cJSON   *cellular;
    const char    *error_message;
    cJSON         *data;
    bool           restore_network;
    int            restore_ret;
    int            ret;

    if (param == NULL || response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&old_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "获取当前配置失败", response);
    }

    new_config = old_config;

    ret = linkg_json_get_bool(param, "path_enabled", &new_config.paths.cellular.enabled);
    if (ret != LINKG_JSON_OK)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "蜂窝路径配置无效", response);
    }

    ret = linkg_json_get_object(param, "cellular", &cellular);
    if (ret != LINKG_JSON_OK)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "蜂窝网络配置无效", response);
    }

    ret = linkg_cellular_config_parse(cellular, &new_config.links.cellular);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "蜂窝网络配置校验失败", response);
    }

    if (new_config.paths.cellular.enabled && !new_config.links.cellular.enabled)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "蜂窝模块关闭时不能启用蜂窝路径", response);
    }

    if (_linkg_web_cellular_config_equal(&old_config, &new_config))
    {
        data = cJSON_CreateObject();
        if (data == NULL)
        {
            return -ENOMEM;
        }

        ret = linkg_json_add_string(data, "apply", "none");
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_cellular_json_error(ret);
        }

        return _linkg_web_response_success(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, data, NULL, response);
    }

    restore_network = false;
    error_message   = NULL;

    ret = linkg_config_save(&new_config, NULL);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "保存蜂窝网络配置失败", response);
    }

    ret = linkg_config_replace(&new_config);
    if (ret != 0)
    {
        error_message = "更新全局配置失败";
        goto rollback;
    }

    ret = linkg_network_set_cellular_config(&new_config.links.cellular);
    if (ret != 0)
    {
        error_message = "更新Network蜂窝网络配置失败";
        goto rollback;
    }

    restore_network = true;

    ret = linkg_network_restart_cellular();
    if (ret != 0)
    {
        error_message = "请求蜂窝网络重启失败";
        goto rollback;
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "apply", "restart");
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return _linkg_web_cellular_json_error(ret);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, data, NULL, response);

rollback:
    restore_ret = _linkg_web_cellular_restore_config(&old_config, restore_network);
    if (restore_ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, "蜂窝网络配置更新失败，恢复原配置也失败", response);
    }

    return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_CONFIG_SET, error_message, response);
}

/**
 * @brief 处理蜂窝网络运行状态查询。
 */
int _linkg_web_handler_cellular_status_get(const cJSON *param, char **response)
{
    linkg_cellular_status_snapshot_t snapshot;
    linkg_config_t                   config;
    cJSON                           *data;
    cJSON                           *local;
    cJSON                           *network;
    cJSON                           *data_status;
    cJSON                           *path;
    uint64_t                         now_us;
    bool                             available;
    bool                             internet_available;
    int                              ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_STATUS_GET, "获取全局配置失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_bool(data, "enabled", config.links.cellular.enabled);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(data, "path_enabled", config.paths.cellular.enabled);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    available          = false;
    internet_available = false;

    if (config.links.cellular.enabled)
    {
        ret = linkg_cellular_get_internet_available(&internet_available);
        if (ret != 0 && ret != -ENODEV)
        {
            cJSON_Delete(data);
            return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_STATUS_GET, "获取蜂窝网络Internet状态失败", response);
        }

        ret = linkg_cellular_get_status(&snapshot);
        if (ret == 0)
        {
            available = true;
        }
        else if (ret != -ENODEV && ret != -ENETDOWN && ret != -EAGAIN)
        {
            cJSON_Delete(data);
            return _linkg_web_response_error(LINKG_WEB_CMD_CELLULAR_STATUS_GET, "获取蜂窝网络运行状态失败", response);
        }
    }

    ret = linkg_json_add_bool(data, "available", available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(data, "partial", available && snapshot.partial);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(data, "internet_available", internet_available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (available)
    {
        local = NULL;
        ret = _linkg_web_cellular_status_build_local(&snapshot.local, &local);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return ret;
        }

        ret = linkg_json_add_object(data, "local", local);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(local);
            goto error;
        }

        network = NULL;
        ret = _linkg_web_cellular_status_build_network(&snapshot.network, &network);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return ret;
        }

        ret = linkg_json_add_object(data, "network", network);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(network);
            goto error;
        }

        data_status = NULL;
        ret = _linkg_web_cellular_status_build_data(&snapshot.data, &data_status);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return ret;
        }

        ret = linkg_json_add_object(data, "data", data_status);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data_status);
            goto error;
        }
    }

    now_us = linkg_time_monotonic_us();
    path   = NULL;

    ret = _linkg_web_cellular_status_build_path(config.device.role, config.paths.cellular.enabled, now_us, &path);
    if (ret != 0)
    {
        cJSON_Delete(data);
        return ret;
    }

    ret = linkg_json_add_object(data, "path", path);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(path);
        goto error;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_CELLULAR_STATUS_GET, data, NULL, response);

error:
    cJSON_Delete(data);

    return _linkg_web_cellular_json_error(ret);
}
