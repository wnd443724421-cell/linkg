/**
 * @file wifi_driver_ops.c
 * @brief LinkG Wi-Fi驱动底层操作实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#define _GNU_SOURCE                     1                         // 启用GNU扩展接口

#include "wifi_driver_ops.h"

#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_os.h"
#include "linkg_time.h"

#include "wifi_platform_internal.h"

/****************************** 驱动常量 ******************************/

#define WAL_SIOCDEVPRIVATE              0x89F0U                   // 驱动私有ioctl起始命令
#define WAL_PRIV_CMD                    (WAL_SIOCDEVPRIVATE + 1U) // 驱动字符串私有命令
#define WIFI_DRIVER_PRIVATE_COMMAND_MAX 128U                      // 私有命令最大长度
#define WIFI_DRIVER_HIPRIV_PATH         "/sys/hisys/hipriv"       // HI1105私有命令节点
#define WIFI_DRIVER_HIPRIV_COMMAND_MAX  64U                       // hipriv命令最大长度
#define WIFI_DRIVER_RELOAD_DELAY_MS     200U                      // 驱动上下电等待时间

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t lock;        // 驱动操作互斥锁
    int             ioctl_fd;    // 私有ioctl套接字
    bool            initialized; // 驱动操作层是否已初始化
} wifi_driver_context_t;

typedef struct
{
    unsigned char *puc_buf;   // 命令数据缓冲区
    unsigned int   total_len; // 命令总长度，不包含结束符
    unsigned int   used_len;  // 驱动已使用长度
} wal_wifi_priv_cmd_stru;

/****************************** 全局上下文 ******************************/

static wifi_driver_context_t g_wifi_driver =
{
    .lock        = PTHREAD_MUTEX_INITIALIZER, // 驱动操作锁静态初始化
    .ioctl_fd    = -1,                        // 私有ioctl套接字尚未创建
    .initialized = false                      // 驱动操作层尚未初始化
};

/****************************** ABI约束 ******************************/

/****************************** ABI约束 ******************************/

_Static_assert(offsetof(wal_radio_status_stru, version) == 0U,
               "wal_radio_status_stru.version ABI offset mismatch");

_Static_assert(offsetof(wal_radio_status_stru, struct_size) == 2U,
               "wal_radio_status_stru.struct_size ABI offset mismatch");

_Static_assert(sizeof(wal_radio_peer_status_stru) == 64U,
               "wal_radio_peer_status_stru ABI size mismatch");

_Static_assert(offsetof(wal_radio_peer_status_stru, driver_tx_bytes) == 8U,
               "wal_radio_peer_status_stru statistics ABI offset mismatch");

_Static_assert(offsetof(wal_radio_peer_status_stru, inactive_ms) == 48U,
               "wal_radio_peer_status_stru runtime ABI offset mismatch");

_Static_assert(offsetof(wal_radio_status_stru, peers) == 32U,
               "wal_radio_status_stru.peers ABI offset mismatch");

_Static_assert(sizeof(wal_radio_status_stru) == 1056U,
               "wal_radio_status_stru ABI size mismatch");

_Static_assert(sizeof(wal_tx_flowctrl_status_stru) <= UINT16_MAX,
               "wal_tx_flowctrl_status_stru exceeds ABI struct_size range");

_Static_assert(offsetof(wal_tx_flowctrl_status_stru, version) == 0U,
               "wal_tx_flowctrl_status_stru.version must be the first field");

_Static_assert(offsetof(wal_tx_flowctrl_status_stru, struct_size) == sizeof(uint16_t),
               "wal_tx_flowctrl_status_stru.struct_size must follow version");

_Static_assert(sizeof(WIFI_PLATFORM_INTERFACE_NAME) <= IFNAMSIZ,
               "Wi-Fi interface name exceeds IFNAMSIZ");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取Wi-Fi驱动操作锁。
 */
static int _wifi_driver_lock(void)
{
    int ret;

    ret = pthread_mutex_lock(&g_wifi_driver.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 释放Wi-Fi驱动操作锁。
 */
static int _wifi_driver_unlock(void)
{
    int ret;

    ret = pthread_mutex_unlock(&g_wifi_driver.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 在持锁状态下执行驱动ioctl。
 */
static int _wifi_driver_ioctl_locked(unsigned long command, void *data, bool log_failure)
{
    struct ifreq ifr;
    size_t       ifname_length;
    int          saved_errno;

    if (data == NULL)
    {
        return -EINVAL;
    }

    if (!g_wifi_driver.initialized || g_wifi_driver.ioctl_fd < 0)
    {
        return -ENODEV;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifname_length = strlen(WIFI_PLATFORM_INTERFACE_NAME);
    if (ifname_length >= IFNAMSIZ)
    {
        return -ENAMETOOLONG;
    }

    memcpy(ifr.ifr_name, WIFI_PLATFORM_INTERFACE_NAME, ifname_length + 1U);
    ifr.ifr_data = data;

    if (command != WAL_TX_FLOWCTRL_IOCTL)
    {
        WIFI_DRIVER_DEBUG("ioctl command=0x%lx", command);
    }

    if (ioctl(g_wifi_driver.ioctl_fd, command, &ifr) < 0)
    {
        saved_errno = errno;

        if (log_failure)
        {
            WIFI_DRIVER_ERROR("ioctl failed, command=0x%lx, error=%d", command, saved_errno);
        }

        return -saved_errno;
    }

    return 0;
}

/**
 * @brief 在持锁状态下发送驱动字符串私有命令。
 */
static int _wifi_driver_private_command_locked(const char *command)
{
    wal_wifi_priv_cmd_stru private_command;
    char                   buffer[WIFI_DRIVER_PRIVATE_COMMAND_MAX];
    size_t                 command_length;

    if (command == NULL || command[0] == '\0')
    {
        return -EINVAL;
    }

    command_length = strnlen(command, sizeof(buffer));
    if (command_length >= sizeof(buffer))
    {
        return -ENAMETOOLONG;
    }

    memset(buffer, 0, sizeof(buffer));
    memset(&private_command, 0, sizeof(private_command));

    memcpy(buffer, command, command_length);

    private_command.puc_buf   = (unsigned char *)buffer;
    private_command.total_len = (unsigned int)command_length;
    private_command.used_len  = 0U;

    WIFI_DRIVER_DEBUG("private command=%s", command);

    return _wifi_driver_ioctl_locked(WAL_PRIV_CMD, &private_command, true);
}

/**
 * @brief 在持锁状态下执行HI1105 hipriv命令。
 */
static int _wifi_driver_hipriv_command_locked(const char *command)
{
    char   command_buffer[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    size_t command_length;
    size_t write_length;
    int    ret;

    if (command == NULL || command[0] == '\0')
    {
        return -EINVAL;
    }

    if (!g_wifi_driver.initialized)
    {
        return -ENODEV;
    }

    command_length = strnlen(command, sizeof(command_buffer));
    if (command_length >= sizeof(command_buffer))
    {
        return -ENAMETOOLONG;
    }

    memcpy(command_buffer, command, command_length);
    write_length = command_length;

    if (command_buffer[command_length - 1U] != '\n')
    {
        command_buffer[write_length] = '\n';
        write_length++;
    }

    ret = linkg_file_write_all(WIFI_DRIVER_HIPRIV_PATH, command_buffer, write_length);
    if (ret != 0)
    {
        WIFI_DRIVER_DEBUG("write hipriv command failed, command=%s, error=%d", command, ret);
        return ret;
    }

    WIFI_DRIVER_DEBUG("hipriv command=%s", command);

    return 0;
}

/**
 * @brief 执行HI1105 hipriv命令。
 */
static int _wifi_driver_hipriv_command(const char *command)
{
    int unlock_ret;
    int ret;

    if (command == NULL || command[0] == '\0')
    {
        return -EINVAL;
    }

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_hipriv_command_locked(command);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    return ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi驱动操作层。
 */
int wifi_driver_ops_init(void)
{
    int saved_errno;
    int unlock_ret;
    int ret;

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    if (g_wifi_driver.initialized)
    {
        unlock_ret = _wifi_driver_unlock();
        if (unlock_ret != 0)
        {
            return unlock_ret;
        }

        return -EALREADY;
    }

    g_wifi_driver.ioctl_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (g_wifi_driver.ioctl_fd < 0)
    {
        saved_errno = errno;
        g_wifi_driver.ioctl_fd = -1;

        unlock_ret = _wifi_driver_unlock();

        WIFI_DRIVER_ERROR("create ioctl socket failed, interface=%s, error=%d", WIFI_PLATFORM_INTERFACE_NAME, saved_errno);

        if (unlock_ret != 0)
        {
            return unlock_ret;
        }

        return -saved_errno;
    }

    g_wifi_driver.initialized = true;

    unlock_ret = _wifi_driver_unlock();
    if (unlock_ret != 0)
    {
        return unlock_ret;
    }

    WIFI_DRIVER_DEBUG("operations initialized, interface=%s", WIFI_PLATFORM_INTERFACE_NAME);

    return 0;
}

/**
 * @brief 释放Wi-Fi驱动操作上下文。
 */
void wifi_driver_ops_deinit(void)
{
    int saved_errno;
    int ret;

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("operations lock failed during deinit, error=%d", ret);
        return;
    }

    if (!g_wifi_driver.initialized)
    {
        ret = _wifi_driver_unlock();
        if (ret != 0)
        {
            WIFI_DRIVER_ERROR("operations unlock failed during deinit, error=%d", ret);
        }

        return;
    }

    if (g_wifi_driver.ioctl_fd >= 0)
    {
        if (close(g_wifi_driver.ioctl_fd) != 0)
        {
            saved_errno = errno;
            WIFI_DRIVER_WARN("close ioctl socket failed, error=%d", saved_errno);
        }

        g_wifi_driver.ioctl_fd = -1;
    }

    g_wifi_driver.initialized = false;

    ret = _wifi_driver_unlock();
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("operations unlock failed during deinit, error=%d", ret);
        return;
    }

    WIFI_DRIVER_DEBUG("operations deinitialized");
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取当前无线接口及关联对端的完整状态。
 */
int wifi_driver_get_radio_status(wal_radio_status_stru *status)
{
    int unlock_ret;
    int ret;

    if (status == NULL)
    {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));

    status->version     = WAL_RADIO_STATUS_ABI_VERSION;
    status->struct_size = (uint16_t)sizeof(*status);

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
        return ret;
    }

    ret = _wifi_driver_ioctl_locked(WAL_RADIO_STATUS_IOCTL, status, false);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
        return ret;
    }

    if (status->version != WAL_RADIO_STATUS_ABI_VERSION ||
        status->struct_size != (uint16_t)sizeof(*status))
    {
        WIFI_DRIVER_DEBUG("radio status ABI mismatch, version=%u, struct_size=%u",
                          status->version,
                          status->struct_size);

        memset(status, 0, sizeof(*status));
        return -EPROTO;
    }

    if (status->peer_count > WAL_RADIO_STATUS_MAX_PEERS)
    {
        WIFI_DRIVER_DEBUG("invalid radio status peer count, peer_count=%u", status->peer_count);

        memset(status, 0, sizeof(*status));
        return -EPROTO;
    }

    WIFI_DRIVER_DEBUG("radio status updated, role=%u, state=%u, peers=%u",
                      status->role,
                      status->state,
                      status->peer_count);

    return 0;
}

/**
 * @brief 获取HCC发送队列及设备流控状态。
 */
int wifi_driver_get_tx_flowctrl_status(wal_tx_flowctrl_status_stru *status)
{
    int unlock_ret;
    int ret;

    if (status == NULL)
    {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));

    status->version     = WAL_TX_FLOWCTRL_ABI_VERSION;
    status->struct_size = (uint16_t)sizeof(*status);

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
        return ret;
    }

    ret = _wifi_driver_ioctl_locked(WAL_TX_FLOWCTRL_IOCTL, status, false);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
        return ret;
    }

    if (status->version != WAL_TX_FLOWCTRL_ABI_VERSION ||
        status->struct_size != (uint16_t)sizeof(*status))
    {
        memset(status, 0, sizeof(*status));
        return -EPROTO;
    }

    return 0;
}

/****************************** 接收模式 ******************************/

/**
 * @brief 设置驱动NAPI/GRO接收模式。
 */
int wifi_driver_set_napi_state(bool enable)
{
    const char *command;
    int         unlock_ret;
    int         ret;

    command = enable ? "CMD_NAPI_STATE 1" : "CMD_NAPI_STATE 0";

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_private_command_locked(command);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("NAPI/GRO state changed, enable=%d", enable);

    return 0;
}

/**
 * @brief 设置驱动低延迟模式。
 */
int wifi_driver_set_low_latency(bool enable)
{
    const char *command;
    int         unlock_ret;
    int         ret;

    command = enable ? "CMD_LOW_LATENCY_ON" : "CMD_LOW_LATENCY_OFF";

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_private_command_locked(command);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("low-latency state changed, enable=%d", enable);

    return 0;
}

/****************************** 电源管理 ******************************/

/**
 * @brief 设置HI1105驱动电源管理状态。
 */
int wifi_driver_set_power_management(bool enable)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  unlock_ret;
    int  ret;

    command_length = snprintf(command,
                              sizeof(command),
                              "%s pm_switch %u",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              enable ? 1U : 0U);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_hipriv_command_locked(command);

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    return ret;
}

/**
 * @brief 设置STA标准节能及HI1105私有节能状态。
 *
 * 整个组合操作持有驱动锁，保证标准节能设置和HI1105私有节能设置
 * 不会与其他驱动控制命令交叉执行。
 */
int wifi_driver_set_sta_power_save(bool enable)
{
    const char *power_save_state;
    char        command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int         command_length;
    int         process_ret;
    int         unlock_ret;
    int         ret;

    power_save_state = enable ? "on" : "off";

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    if (!g_wifi_driver.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    // 通过nl80211设置Linux标准STA节能状态。
    process_ret = linkg_os_run("iw",
                               "dev",
                               WIFI_PLATFORM_INTERFACE_NAME,
                               "set",
                               "power_save",
                               power_save_state,
                               NULL);
    if (process_ret != 0)
    {
        WIFI_DRIVER_ERROR("set standard STA power save failed, enable=%d, status=%d", enable, process_ret);

        ret = process_ret < 0 ? process_ret : -EIO;
        goto unlock;
    }

    command_length = snprintf(command,
                              sizeof(command),
                              "%s set_sta_pm_on %u",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              enable ? 1U : 0U);
    if (command_length < 0)
    {
        ret = -EIO;
        goto unlock;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        ret = -ENAMETOOLONG;
        goto unlock;
    }

    ret = _wifi_driver_hipriv_command_locked(command);

unlock:
    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("STA power save changed, enable=%d", enable);

    return 0;
}

/****************************** 驱动控制 ******************************/

/**
 * @brief 通过驱动重新上电触发INI配置重新加载。
 *
 * AP模式重新上电前需要恢复驱动电源管理设置。
 * 整个重载序列持有驱动锁，包括必要的等待，避免其他线程在上下电期间插入驱动控制操作。
 */
int wifi_driver_reload_ini(linkg_device_role_t role)
{
    int unlock_ret;
    int ret;

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    ret = _wifi_driver_lock();
    if (ret != 0)
    {
        return ret;
    }

    if (role == LINKG_DEVICE_ROLE_AP)
    {
        ret = _wifi_driver_private_command_locked("SET_POWER_MGMT_ON 1");
        if (ret == 0)
        {
            ret = linkg_time_sleep_ms(WIFI_DRIVER_RELOAD_DELAY_MS);
        }
    }
    else
    {
        ret = 0;
    }

    if (ret == 0)
    {
        ret = _wifi_driver_private_command_locked("SET_POWER_ON 0");
    }

    if (ret == 0)
    {
        ret = linkg_time_sleep_ms(WIFI_DRIVER_RELOAD_DELAY_MS);
    }

    if (ret == 0)
    {
        ret = _wifi_driver_private_command_locked("SET_POWER_ON 1");
    }

    if (ret == 0)
    {
        ret = linkg_time_sleep_ms(WIFI_DRIVER_RELOAD_DELAY_MS);
    }

    unlock_ret = _wifi_driver_unlock();
    if (ret == 0 && unlock_ret != 0)
    {
        ret = unlock_ret;
    }

    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("INI reloaded, role=%d", role);

    return 0;
}

/****************************** 窄带控制 ******************************/

/**
 * @brief 设置HI1105窄带开关及工作带宽。
 */
int wifi_driver_set_narrow_bandwidth(bool enable, uint16_t bandwidth_mhz)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  ret;

    if (bandwidth_mhz == 0U)
    {
        return -EINVAL;
    }

    command_length = snprintf(command,
                              sizeof(command),
                              "%s narrow_bw %u %um",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              enable ? 1U : 0U,
                              (unsigned int)bandwidth_mhz);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_hipriv_command(command);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("narrow-band bandwidth changed, enable=%d, bandwidth=%u", enable, bandwidth_mhz);

    return 0;
}

/**
 * @brief 设置HI1105窄带自适应速率状态。
 */
int wifi_driver_set_narrow_auto_rate(bool enable)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  ret;

    command_length = snprintf(command,
                              sizeof(command),
                              "%s autorate_cfg_set enable %u",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              enable ? 1U : 0U);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_hipriv_command(command);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("narrow-band auto rate changed, enable=%d", enable);

    return 0;
}

/**
 * @brief 设置HI1105窄带固定速率等级。
 */
int wifi_driver_set_narrow_rate_level(uint8_t rate_level)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  ret;

    command_length = snprintf(command,
                              sizeof(command),
                              "%s autorate_cfg_set level %u",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              (unsigned int)rate_level);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_hipriv_command(command);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("narrow-band rate level changed, level=%u", rate_level);

    return 0;
}

/**
 * @brief 重新向驱动下发当前INI中的窄带功率参数表。
 */
int wifi_driver_refresh_narrow_power_table(void)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  ret;

    command_length = snprintf(command,
                              sizeof(command),
                              "%s nb_power_enable 1",
                              WIFI_PLATFORM_INTERFACE_NAME);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_hipriv_command(command);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("narrow-band power table refreshed");

    return 0;
}

/****************************** 日志控制 ******************************/

/**
 * @brief 设置HI1105驱动日志等级。
 */
int wifi_driver_set_log_level(wifi_driver_log_level_t level)
{
    char command[WIFI_DRIVER_HIPRIV_COMMAND_MAX];
    int  command_length;
    int  ret;

    if (level < WIFI_DRIVER_LOG_LEVEL_ERROR || level > WIFI_DRIVER_LOG_LEVEL_INFO)
    {
        return -EINVAL;
    }

    command_length = snprintf(command,
                              sizeof(command),
                              "%s log_level %u",
                              WIFI_PLATFORM_INTERFACE_NAME,
                              (unsigned int)level);
    if (command_length < 0)
    {
        return -EIO;
    }

    if ((size_t)command_length >= sizeof(command))
    {
        return -ENAMETOOLONG;
    }

    ret = _wifi_driver_hipriv_command(command);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_DRIVER_DEBUG("driver log level changed, level=%u", (unsigned int)level);

    return 0;
}
