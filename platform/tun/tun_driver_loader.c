/**
 * @file tun_driver_loader.c
 * @brief LinkG TUN内核驱动加载与卸载实现
 */

#include "tun_driver_loader.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_os.h"

/****************************** 模块常量 ******************************/

#define TUN_DRIVER_MODULE_NAME      "tun"                              // TUN内核模块名称
#define TUN_DRIVER_MODULE_PATH      "/app/current/drivers/tun.ko"      // 当前LinkG版本的TUN驱动路径
#define TUN_DRIVER_MODULE_LINE_MAX  256U                               // /proc/modules单行缓冲区长度
#define TUN_DRIVER_LOG_TAG          "TUN-DRIVER"                       // 驱动管理日志标签

/****************************** 日志定义 ******************************/

#define TUN_DRIVER_INFO(fmt, ...)  LINKG_LOG_INFO("%s: " fmt, TUN_DRIVER_LOG_TAG, ##__VA_ARGS__)
#define TUN_DRIVER_WARN(fmt, ...)  LINKG_LOG_WARN("%s: " fmt, TUN_DRIVER_LOG_TAG, ##__VA_ARGS__)
#define TUN_DRIVER_ERROR(fmt, ...) LINKG_LOG_ERROR("%s: " fmt, TUN_DRIVER_LOG_TAG, ##__VA_ARGS__)

/****************************** 模块状态 ******************************/

static pthread_mutex_t g_tun_driver_lock  = PTHREAD_MUTEX_INITIALIZER;
static bool            g_tun_driver_owned = false; // 仅表示本进程已成功装载或接管当前模块

/****************************** 内部辅助 ******************************/

/**
 * @brief 查询TUN模块是否已经加载。
 *
 * 不能把/proc/modules读取失败误判为模块未加载。
 */
static int _tun_driver_module_loaded(bool *loaded)
{
    FILE   *stream;
    char    line[TUN_DRIVER_MODULE_LINE_MAX];
    size_t  name_length;
    bool    found;
    int     ret;

    if (loaded == NULL)
    {
        return -EINVAL;
    }

    *loaded      = false;
    name_length  = strlen(TUN_DRIVER_MODULE_NAME);
    found        = false;

    stream = fopen("/proc/modules", "r");
    if (stream == NULL)
    {
        return -errno;
    }

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (strncmp(line, TUN_DRIVER_MODULE_NAME, name_length) != 0)
        {
            continue;
        }

        if (line[name_length] != ' ')
        {
            continue;
        }

        found = true;
        break;
    }

    ret = ferror(stream) ? -EIO : 0;

    if (fclose(stream) != 0 && ret == 0)
    {
        ret = -errno;
    }

    if (ret == 0)
    {
        *loaded = found;
    }

    return ret;
}

/**
 * @brief 卸载当前已加载的TUN模块，并确认模块确实消失。
 *
 * 调用方必须持有g_tun_driver_lock，且已确认模块处于已加载状态。
 * 不允许使用强制卸载参数；模块仍有引用时由内核拒绝卸载。
 */
static int _tun_driver_remove_locked(void)
{
    bool loaded;
    int  command_ret;
    int  ret;

    command_ret = linkg_os_run("rmmod", TUN_DRIVER_MODULE_NAME, NULL);

    ret = _tun_driver_module_loaded(&loaded);
    if (ret != 0)
    {
        return ret;
    }

    if (loaded)
    {
        return command_ret != 0 ? command_ret : -EBUSY;
    }

    return 0;
}

/****************************** 驱动生命周期 ******************************/

/**
 * @brief 加载当前LinkG版本的TUN驱动。
 *
 * 已由本进程加载且仍存在时幂等返回。
 * 遇到前一进程遗留或系统预加载的tun时，先正常卸载，再加载当前版本。
 * 该方案以LinkG独占TUN驱动为前提；存在其他使用者时卸载将失败。
 */
int tun_driver_load(void)
{
    bool loaded;
    int  ret;

    ret = pthread_mutex_lock(&g_tun_driver_lock);
    if (ret != 0)
    {
        return -ret;
    }

    ret = _tun_driver_module_loaded(&loaded);
    if (ret != 0)
    {
        TUN_DRIVER_ERROR("query module failed, error=%d", ret);
        goto cleanup;
    }

    if (loaded && g_tun_driver_owned)
    {
        ret = 0;
        goto cleanup;
    }

    if (access(TUN_DRIVER_MODULE_PATH, R_OK) != 0)
    {
        ret = -errno;
        TUN_DRIVER_ERROR("module file unavailable, path=%s, error=%d",
                         TUN_DRIVER_MODULE_PATH,
                         ret);
        goto cleanup;
    }

    if (loaded)
    {
        TUN_DRIVER_WARN("preloaded module found, replacing with current release");

        ret = _tun_driver_remove_locked();
        if (ret != 0)
        {
            TUN_DRIVER_ERROR("remove preloaded module failed, error=%d", ret);
            goto cleanup;
        }
    }

    g_tun_driver_owned = false;

    ret = linkg_os_run("insmod", TUN_DRIVER_MODULE_PATH, NULL);
    if (ret != 0)
    {
        TUN_DRIVER_ERROR("load module failed, path=%s, error=%d",
                         TUN_DRIVER_MODULE_PATH,
                         ret);
        goto cleanup;
    }

    /* insmod成功后立即记录所有权，避免后续状态读取失败造成驱动遗留。 */
    g_tun_driver_owned = true;

    ret = _tun_driver_module_loaded(&loaded);
    if (ret != 0)
    {
        TUN_DRIVER_ERROR("verify loaded module failed, error=%d", ret);
        goto cleanup;
    }

    if (!loaded)
    {
        g_tun_driver_owned = false;
        ret                = -ENODEV;
        TUN_DRIVER_ERROR("module not present after insmod, path=%s", TUN_DRIVER_MODULE_PATH);
        goto cleanup;
    }

    TUN_DRIVER_INFO("module loaded, path=%s", TUN_DRIVER_MODULE_PATH);

cleanup:
    (void)pthread_mutex_unlock(&g_tun_driver_lock);

    return ret;
}

/**
 * @brief 卸载本进程装载的TUN驱动。
 *
 * 必须在TUN读取线程停止、/dev/net/tun文件描述符关闭之后调用。
 * 卸载失败时保留所有权，以便上层后续重试。
 */
int tun_driver_unload(void)
{
    bool loaded;
    int  ret;

    ret = pthread_mutex_lock(&g_tun_driver_lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!g_tun_driver_owned)
    {
        ret = 0;
        goto cleanup;
    }

    ret = _tun_driver_module_loaded(&loaded);
    if (ret != 0)
    {
        TUN_DRIVER_ERROR("query module failed before unload, error=%d", ret);
        goto cleanup;
    }

    if (!loaded)
    {
        g_tun_driver_owned = false;
        ret                = 0;
        goto cleanup;
    }

    ret = _tun_driver_remove_locked();
    if (ret != 0)
    {
        TUN_DRIVER_ERROR("unload module failed, error=%d", ret);
        goto cleanup;
    }

    g_tun_driver_owned = false;
    TUN_DRIVER_INFO("module unloaded, module=%s", TUN_DRIVER_MODULE_NAME);

cleanup:
    (void)pthread_mutex_unlock(&g_tun_driver_lock);

    return ret;
}
