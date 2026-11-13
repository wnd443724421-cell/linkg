/**
 * @file linkg_config.c
 * @brief LinkG全局配置管理接口实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-08-28
 */

#define _POSIX_C_SOURCE        200809L         // 请求POSIX.1-2008接口

#include "linkg_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_file.h"
#include "linkg_json.h"
#include "linkg_log.h"

#include "config_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_CONFIG_FILE_MAX  (64U * 1024U)   // 配置文件最大长度
#define LINKG_CONFIG_FILE_MODE 0600            // 配置文件默认权限，包含Wi-Fi密码和SIM PIN

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_rwlock_t lock;              // 全局配置读写锁
    pthread_mutex_t  file_lock;         // 用户配置文件写入互斥锁
    linkg_config_t   current;           // 当前生效配置
    bool             initialized;       // 是否已经加载配置
    bool             import_committed;  // 导入落盘后等待全服务重载，禁止旧配置覆盖
    bool             restore_committed; // 按键恢复成功后禁止本进程再次保存用户配置
} linkg_config_context_t;

/****************************** 全局上下文 ******************************/

static linkg_config_context_t g_config =
{
    .lock      = PTHREAD_RWLOCK_INITIALIZER, // 全局配置读写锁
    .file_lock = PTHREAD_MUTEX_INITIALIZER   // 配置文件写入锁
};

static pthread_mutex_t g_config_transaction_lock = PTHREAD_MUTEX_INITIALIZER; // 配置写事务锁，不阻塞配置读取

static uint64_t g_config_transaction_reservation; // 已接受异步事务的预留编号，由事务锁保护
static uint64_t g_config_transaction_sequence;    // 预留编号递增序列，不复用0

/****************************** 配置事务 ******************************/

/**
 * @brief 尝试独占一次跨保存和运行态切换的事务，必须由同一线程结束。
 */
int linkg_config_transaction_begin(void)
{
    int ret;

    ret = pthread_mutex_trylock(&g_config_transaction_lock);
    if (ret != 0)
    {
        return -ret;
    }
    if (g_config_transaction_reservation != 0U)
    {
        pthread_mutex_unlock(&g_config_transaction_lock);
        return -EBUSY;
    }
    pthread_mutex_lock(&g_config.file_lock);
    ret = g_config.import_committed || g_config.restore_committed ? -ESHUTDOWN : 0;
    pthread_mutex_unlock(&g_config.file_lock);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_config_transaction_lock);
    }
    return ret;
}

/**
 * @brief 接受异步任务前预留事务，不将普通互斥锁跨线程移交。
 */
int linkg_config_transaction_reserve(uint64_t *reservation)
{
    int ret;

    if (reservation == NULL)
    {
        return -EINVAL;
    }
    *reservation = 0U;
    ret = linkg_config_transaction_begin();
    if (ret != 0)
    {
        return ret;
    }
    g_config_transaction_sequence++;
    if (g_config_transaction_sequence == 0U)
    {
        g_config_transaction_sequence = 1U;
    }
    g_config_transaction_reservation = g_config_transaction_sequence;
    *reservation = g_config_transaction_reservation;
    pthread_mutex_unlock(&g_config_transaction_lock);
    return 0;
}

/**
 * @brief 后台执行线程领取自己的预留，成功后由本线程结束事务。
 */
int linkg_config_transaction_begin_reserved(uint64_t reservation)
{
    int ret;

    if (reservation == 0U)
    {
        return -EINVAL;
    }
    ret = pthread_mutex_lock(&g_config_transaction_lock);
    if (ret != 0)
    {
        return -ret;
    }
    if (g_config_transaction_reservation != reservation)
    {
        pthread_mutex_unlock(&g_config_transaction_lock);
        return -ESTALE;
    }
    g_config_transaction_reservation = 0U;
    pthread_mutex_lock(&g_config.file_lock);
    ret = g_config.import_committed || g_config.restore_committed ? -ESHUTDOWN : 0;
    pthread_mutex_unlock(&g_config.file_lock);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_config_transaction_lock);
    }
    return ret;
}

/**
 * @brief 线程创建失败时撤销本任务预留，不影响其他任务的事务。
 */
int linkg_config_transaction_cancel_reserved(uint64_t reservation)
{
    int ret;

    if (reservation == 0U)
    {
        return -EINVAL;
    }
    ret = pthread_mutex_lock(&g_config_transaction_lock);
    if (ret != 0)
    {
        return -ret;
    }
    ret = g_config_transaction_reservation == reservation ? 0 : -ESTALE;
    if (ret == 0)
    {
        g_config_transaction_reservation = 0U;
    }
    pthread_mutex_unlock(&g_config_transaction_lock);
    return ret;
}

/**
 * @brief 结束当前线程持有的配置写事务。
 */
void linkg_config_transaction_end(void)
{
    pthread_mutex_unlock(&g_config_transaction_lock);
}

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

/****************************** 配置文件读取 ******************************/

/**
 * @brief 读取、解析并校验指定配置文件。
 *
 * 成功后由调用方持有content，需要使用free释放。
 * 失败时content保持NULL，不修改调用方的有效配置。
 */
static int _config_read_validated_file(const char *path, linkg_config_t *config, char **content, size_t *length)
{
    cJSON  *root;
    char   *buffer;
    size_t  file_length;
    int     ret;

    if (path == NULL || config == NULL || content == NULL || length == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    *content = NULL;
    *length  = 0U;

    buffer      = NULL;
    root        = NULL;
    file_length = 0U;

    ret = linkg_file_read_all(path, LINKG_CONFIG_FILE_MAX, &buffer, &file_length);
    if (ret != 0)
    {
        return _config_error_from_file(ret);
    }

    if (file_length == 0U || memchr(buffer, '\0', file_length) != NULL)
    {
        free(buffer);
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_parse(buffer, &root);
    if (ret != LINKG_JSON_OK)
    {
        free(buffer);
        return config_json_parse_error(ret);
    }

    ret = linkg_config_parse_json(root, config);

    cJSON_Delete(root);

    if (ret != CONFIG_OK)
    {
        free(buffer);
        return ret;
    }

    *content = buffer;
    *length  = file_length;

    return CONFIG_OK;
}

/****************************** 出厂文件恢复 ******************************/

/**
 * @brief 校验出厂配置并原子恢复用户文件。
 *
 * restored允许为NULL；成功时可返回本次恢复的配置快照。
 * 不修改当前全局配置，调用方自行决定是否重启或加载。
 */
static int _config_restore_factory_file(linkg_config_t *restored)
{
    linkg_config_t temp;
    char          *content;
    size_t         length;
    int            ret;

    content = NULL;
    length  = 0U;

    ret = _config_read_validated_file(LINKG_CONFIG_FACTORY_PATH, &temp, &content, &length);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_file_write_all_atomic(LINKG_CONFIG_USER_PATH,
                                      content,
                                      length,
                                      LINKG_CONFIG_FILE_MODE);

    free(content);

    if (ret != 0)
    {
        return _config_error_from_file(ret);
    }

    if (restored != NULL)
    {
        *restored = temp;
    }

    return CONFIG_OK;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 加载用户配置，加载失败时自动恢复出厂配置。
 */
int linkg_config_load(void)
{
    linkg_config_t config;
    char          *content;
    size_t         length;
    int            ret;

    content = NULL;
    length  = 0U;

    ret = _config_read_validated_file(LINKG_CONFIG_USER_PATH, &config, &content, &length);

    free(content);

    if (ret != CONFIG_OK)
    {
        if (ret == CONFIG_ERR_MEMORY)
        {
            return ret;
        }

        LINKG_LOG_INFO("CONFIG: user configuration invalid, error=%d, restoring factory defaults", ret);

        ret = _config_restore_factory_file(&config);
        if (ret != CONFIG_OK)
        {
            LINKG_LOG_ERROR("CONFIG: factory configuration restore failed, error=%d", ret);
            return ret;
        }

        LINKG_LOG_INFO("CONFIG: factory configuration restored successfully");
    }

    return linkg_config_replace(&config);
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

    // 全部业务模块停止后，允许下一个应用生命周期重新保存配置。
    pthread_mutex_lock(&g_config.file_lock);
    g_config.restore_committed = false;
    g_config.import_committed  = false;
    pthread_mutex_unlock(&g_config.file_lock);
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
 * @brief 获取串口配置。
 */
int linkg_config_get_uart(linkg_uart_config_t *out)
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

    *out = g_config.current.uart;

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

    ret = config_validate(&temp);
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
 * @brief 保存当前配置到用户配置文件。
 */
static int _config_save(const linkg_config_t *config, bool imported)
{
    cJSON *root;
    char  *content;
    int    ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_config_to_json(config, &root);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_print_formatted(root, &content);

    cJSON_Delete(root);

    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    pthread_mutex_lock(&g_config.file_lock);

    if (g_config.restore_committed || g_config.import_committed)
    {
        pthread_mutex_unlock(&g_config.file_lock);
        linkg_json_string_free(content);
        LINKG_LOG_WARN("CONFIG: save rejected, configuration reload is committed");
        return CONFIG_ERR_FILE;
    }

    ret = linkg_file_write_all_atomic(LINKG_CONFIG_USER_PATH,
                                      content,
                                      strlen(content),
                                      LINKG_CONFIG_FILE_MODE);

    if (ret == 0 && imported)
    {
        g_config.import_committed = true;
    }

    pthread_mutex_unlock(&g_config.file_lock);
    linkg_json_string_free(content);

    if (ret != 0)
    {
        return _config_error_from_file(ret);
    }

    return CONFIG_OK;
}

/****************************** 配置导入 ******************************/

/**
 * @brief 保存配置，调用方使用配置事务协调运行态更新。
 */
int linkg_config_save(const linkg_config_t *config)
{
    return _config_save(config, false);
}

/**
 * @brief 完整校验并原子保存导入配置，不提前替换当前内存快照。
 *
 * 调用方必须持有配置写事务；成功后必须重载全部服务。
 */
int linkg_config_import(const cJSON *root)
{
    linkg_config_t config;
    int            ret;

    // 备份文件必须包含五个完整配置域，拒绝局部配置或其他产品文件。
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 5 ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "uart")))
    {
        return CONFIG_ERR_VALIDATE;
    }

    ret = linkg_config_parse_json(root, &config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return _config_save(&config, true);
}

/**
 * @brief 查询导入是否已经落盘并等待全服务重载。
 */
bool linkg_config_import_pending(void)
{
    bool pending;

    pthread_mutex_lock(&g_config.file_lock);
    pending = g_config.import_committed;
    pthread_mutex_unlock(&g_config.file_lock);

    return pending;
}

/****************************** 配置恢复 ******************************/

/**
 * @brief 将出厂默认配置恢复到用户配置文件。
 *
 * 不修改当前内存配置。恢复成功后禁止普通保存，调用方应请求系统重启。
 * 开机配置损坏的自动恢复不走本接口，因此不影响启动后的正常保存。
 */
int linkg_config_restore_factory(void)
{
    int ret;

    pthread_mutex_lock(&g_config.file_lock);

    if (g_config.restore_committed)
    {
        pthread_mutex_unlock(&g_config.file_lock);
        return CONFIG_OK;
    }

    if (g_config.import_committed)
    {
        pthread_mutex_unlock(&g_config.file_lock);
        return CONFIG_ERR_FILE;
    }

    // 先等待已经开始的Web保存完成，再原子恢复出厂配置。
    ret = _config_restore_factory_file(NULL);
    if (ret == CONFIG_OK)
    {
        g_config.restore_committed = true;
    }

    pthread_mutex_unlock(&g_config.file_lock);

    return ret;
}
