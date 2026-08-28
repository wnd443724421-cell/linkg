/**
 * @file linkg_config.c
 * @brief LinkG全局配置管理接口实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-08-28
 */

#define _POSIX_C_SOURCE 200809L

#include "linkg_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config_internal.h"
#include "linkg_file.h"
#include "linkg_json.h"

/****************************** 模块常量 ******************************/

#define LINKG_CONFIG_FILE_MAX  (64U * 1024U) // 配置文件最大长度
#define LINKG_CONFIG_FILE_MODE 0600           // 配置文件默认权限，包含Wi-Fi密码和SIM PIN

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_rwlock_t lock;        // 全局配置读写锁
    linkg_config_t   current;     // 当前生效配置
    bool             initialized; // 是否已经加载配置
} linkg_config_context_t;

/****************************** 全局上下文 ******************************/

static linkg_config_context_t g_config =
{
    .lock = PTHREAD_RWLOCK_INITIALIZER // 全局配置读写锁初始值
};

/****************************** 错误处理 ******************************/

/**
 * @brief 将文件错误转换为配置错误。
 */
static int _config_error_from_file(int error)
{
    switch (error)
    {
        case 0:
            return CONFIG_OK;

        case -EINVAL:
            return CONFIG_ERR_FILE;

        case -ENOMEM:
            return CONFIG_ERR_MEMORY;

        case -EFBIG:
        case -EOVERFLOW:
            return CONFIG_ERR_VALIDATE;

        default:
            return CONFIG_ERR_FILE;
    }
}

/****************************** 默认配置 ******************************/

/**
 * @brief 设置全局配置默认值。
 */
static void _config_set_default(linkg_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    linkg_device_config_set_default(&out->device);
    linkg_network_config_set_default(&out->network);
    linkg_links_config_set_default(&out->links);
    linkg_paths_config_set_default(&out->paths);
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验全局配置。
 */
static int _config_validate(const linkg_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_device_config_validate(&config->device);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_network_config_validate(&config->network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_links_config_validate(&config->links);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_paths_config_validate(&config->paths);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (config->paths.wifi.enabled && !config->links.wifi.enabled)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->paths.cellular.enabled && !config->links.cellular.enabled)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/****************************** JSON解析 ******************************/

/**
 * @brief 解析全局JSON配置。
 */
static int _config_parse_json(const cJSON *root, linkg_config_t *out)
{
    linkg_config_t temp;
    const cJSON   *child;
    int            ret;

    if (root == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(root))
    {
        return CONFIG_ERR_PARSE;
    }

    _config_set_default(&temp);

    child = NULL;

    ret = linkg_json_get_object(root, "device", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_device_config_parse(child, &temp.device);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "network", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_network_config_parse(child, &temp.network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "links", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_links_config_parse(temp.device.role, child, &temp.links);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "paths", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_paths_config_parse(child, &temp.paths);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 加载全局配置。
 */
int linkg_config_load(const char *path)
{
    linkg_config_t temp;
    const char    *actual_path;
    char          *content;
    cJSON         *root;
    int            ret;

    actual_path = path != NULL ? path : LINKG_CONFIG_DEFAULT_PATH;

    ret = linkg_file_read_all(actual_path, LINKG_CONFIG_FILE_MAX, &content, NULL);
    if (ret != 0)
    {
        return _config_error_from_file(ret);
    }

    ret = linkg_json_parse(content, &root);

    free(content);

    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _config_parse_json(root, &temp);

    cJSON_Delete(root);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return linkg_config_replace(&temp);
}

/**
 * @brief 反初始化全局配置。
 */
void linkg_config_deinit(void)
{
    pthread_rwlock_wrlock(&g_config.lock);

    memset(&g_config.current, 0, sizeof(g_config.current));
    g_config.initialized = false;

    pthread_rwlock_unlock(&g_config.lock);
}

/****************************** 配置获取 ******************************/

/**
 * @brief 创建当前全局配置快照。
 */
int linkg_config_create_snapshot(linkg_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取设备配置。
 */
int linkg_config_get_device(linkg_device_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.device;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取网络配置。
 */
int linkg_config_get_network(linkg_network_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.network;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取接入模块配置。
 */
int linkg_config_get_links(linkg_links_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.links;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取逻辑路径配置。
 */
int linkg_config_get_paths(linkg_paths_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.paths;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取Wi-Fi配置。
 */
int linkg_config_get_wifi(linkg_wifi_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.links.wifi;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/**
 * @brief 获取蜂窝配置。
 */
int linkg_config_get_cellular(linkg_cellular_config_t *out)
{
    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    pthread_rwlock_rdlock(&g_config.lock);

    if (!g_config.initialized)
    {
        pthread_rwlock_unlock(&g_config.lock);
        return CONFIG_ERR_VALIDATE;
    }

    *out = g_config.current.links.cellular;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/****************************** 配置更新 ******************************/

/**
 * @brief 替换当前全局配置。
 */
int linkg_config_replace(const linkg_config_t *config)
{
    linkg_config_t temp;
    int            ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    temp = *config;

    ret = _config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    pthread_rwlock_wrlock(&g_config.lock);

    g_config.current     = temp;
    g_config.initialized = true;

    pthread_rwlock_unlock(&g_config.lock);

    return CONFIG_OK;
}

/****************************** 配置持久化 ******************************/

/**
 * @brief 保存全局配置。
 */
int linkg_config_save(const linkg_config_t *config, const char *path)
{
    const char *actual_path;
    cJSON      *root;
    char       *content;
    int         ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    actual_path = path != NULL ? path : LINKG_CONFIG_DEFAULT_PATH;

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_device_config_to_json(root, "device", &config->device);

    if (ret == CONFIG_OK)
    {
        ret = linkg_network_config_to_json(root, "network", &config->network);
    }

    if (ret == CONFIG_OK)
    {
        ret = linkg_links_config_to_json(root, "links", &config->links);
    }

    if (ret == CONFIG_OK)
    {
        ret = linkg_paths_config_to_json(root, "paths", &config->paths);
    }

    if (ret != CONFIG_OK)
    {
        cJSON_Delete(root);
        return ret;
    }

    ret = linkg_json_print_formatted(root, &content);

    cJSON_Delete(root);

    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_file_write_all_atomic(actual_path, content, strlen(content), LINKG_CONFIG_FILE_MODE);

    linkg_json_string_free(content);

    if (ret != 0)
    {
        return _config_error_from_file(ret);
    }

    return CONFIG_OK;
}
