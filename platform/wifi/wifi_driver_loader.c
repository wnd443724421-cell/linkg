/**
 * @file wifi_driver_loader.c
 * @brief LinkG HI1105 Wi-Fi驱动加载及INI配置实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#include "wifi_driver_loader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_os.h"

#include "wifi_platform_internal.h"

/****************************** 模块常量 ******************************/

#define WIFI_DRIVER_MODULE_LINE_MAX 256U // /proc/modules单行缓冲区长度

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查指定内核模块是否已经加载。
 */
static bool _wifi_driver_module_loaded(const char *module_name)
{
    FILE *stream = NULL;
    char line[WIFI_DRIVER_MODULE_LINE_MAX];
    size_t name_length;
    bool loaded = false;

    if (module_name == NULL)
    {
        return false;
    }

    if (module_name[0] == '\0')
    {
        return false;
    }

    name_length = strlen(module_name);
    if (name_length >= sizeof(line) - 1U)
    {
        return false;
    }

    if (linkg_file_stream_open("/proc/modules", "r", &stream) != 0)
    {
        return false;
    }

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (strncmp(line, module_name, name_length) != 0)
        {
            continue;
        }

        if (line[name_length] != ' ')
        {
            continue;
        }

        loaded = true;
        break;
    }

    (void)linkg_file_stream_close(&stream);

    return loaded;
}

/**
 * @brief 加载指定内核模块。
 *
 * @note 模块已经加载时直接返回成功；insmod失败后会再次确认实际模块状态，
 *       兼容重复加载或并发加载导致的命令失败。
 */
static int _wifi_driver_load_module(const char *module_path, const char *module_name)
{
    int saved_errno;
    int ret;

    if (module_path == NULL)
    {
        return -EINVAL;
    }

    if (module_path[0] == '\0')
    {
        return -EINVAL;
    }

    if (module_name == NULL)
    {
        return -EINVAL;
    }

    if (module_name[0] == '\0')
    {
        return -EINVAL;
    }

    if (_wifi_driver_module_loaded(module_name))
    {
        WIFI_DRIVER_DEBUG("module already loaded, module=%s", module_name);
        return 0;
    }

    if (access(module_path, R_OK) != 0)
    {
        saved_errno = errno;

        WIFI_DRIVER_ERROR("module file unavailable, module=%s, path=%s, error=%d",
                          module_name,
                          module_path,
                          saved_errno);

        return -saved_errno;
    }

    WIFI_DRIVER_DEBUG("loading module, module=%s, path=%s", module_name, module_path);

    ret = linkg_os_run("insmod", module_path, NULL);
    if (ret != 0)
    {
        /**
         * insmod可能因为重复加载或并发加载返回失败，
         * 失败后再次确认模块实际状态，避免把已成功加载误判为失败。
         */
        if (_wifi_driver_module_loaded(module_name))
        {
            WIFI_DRIVER_DEBUG("module loaded after insmod result, module=%s", module_name);
            return 0;
        }

        WIFI_DRIVER_ERROR("load module failed, module=%s, path=%s, error=%d",
                          module_name,
                          module_path,
                          ret);

        return ret;
    }

    return 0;
}

/**
 * @brief 比较指定INI文件与当前生效INI文件内容是否一致。
 */
static int _wifi_driver_ini_matches(const char *source_path, bool *matches)
{
    char *source_content = NULL;
    char *active_content = NULL;
    size_t source_length = 0U;
    size_t active_length = 0U;
    int ret;

    if (source_path == NULL)
    {
        return -EINVAL;
    }

    if (source_path[0] == '\0')
    {
        return -EINVAL;
    }

    if (matches == NULL)
    {
        return -EINVAL;
    }

    *matches = false;

    ret = linkg_file_read_all(source_path, WIFI_DRIVER_INI_MAX_SIZE, &source_content, &source_length);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_file_read_all(WIFI_DRIVER_ACTIVE_INI_PATH, WIFI_DRIVER_INI_MAX_SIZE, &active_content, &active_length);
    if (ret == -ENOENT)
    {
        ret = 0;
        goto cleanup;
    }

    if (ret != 0)
    {
        goto cleanup;
    }

    if (source_length != active_length)
    {
        goto cleanup;
    }

    *matches = memcmp(source_content, active_content, source_length) == 0;

cleanup:
    free(active_content);
    free(source_content);

    return ret;
}

/****************************** 驱动加载 ******************************/

/**
 * @brief 检查HI1105平台模块和Wi-Fi模块是否已经全部加载。
 */
bool wifi_driver_is_loaded(void)
{
    bool platform_loaded;
    bool wifi_loaded;

    platform_loaded = _wifi_driver_module_loaded(WIFI_DRIVER_PLATFORM_MODULE_NAME);
    wifi_loaded = _wifi_driver_module_loaded(WIFI_DRIVER_WIFI_MODULE_NAME);

    return platform_loaded && wifi_loaded;
}

/**
 * @brief 按依赖顺序确保HI1105平台模块和Wi-Fi模块已经加载。
 */
int wifi_driver_load(void)
{
    bool platform_loaded;
    bool wifi_loaded;
    int ret;

    if (wifi_driver_is_loaded())
    {
        WIFI_DRIVER_DEBUG("driver modules already loaded");
        return 0;
    }

    /**
     * HI1105 Wi-Fi模块依赖平台模块，
     * 必须先加载plat_1105，再加载wifi_1105。
     */
    ret = _wifi_driver_load_module(WIFI_DRIVER_PLATFORM_MODULE_PATH, WIFI_DRIVER_PLATFORM_MODULE_NAME);
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_load_module(WIFI_DRIVER_WIFI_MODULE_PATH, WIFI_DRIVER_WIFI_MODULE_NAME);
    if (ret != 0)
    {
        return ret;
    }

    platform_loaded = _wifi_driver_module_loaded(WIFI_DRIVER_PLATFORM_MODULE_NAME);
    wifi_loaded = _wifi_driver_module_loaded(WIFI_DRIVER_WIFI_MODULE_NAME);

    if (!platform_loaded)
    {
        WIFI_DRIVER_ERROR("driver platform module not ready, module=%s", WIFI_DRIVER_PLATFORM_MODULE_NAME);
        return -ENODEV;
    }

    if (!wifi_loaded)
    {
        WIFI_DRIVER_ERROR("driver Wi-Fi module not ready, module=%s", WIFI_DRIVER_WIFI_MODULE_NAME);
        return -ENODEV;
    }

    WIFI_DRIVER_INFO("driver modules ready, platform=%s, wifi=%s",
                     WIFI_DRIVER_PLATFORM_MODULE_NAME,
                     WIFI_DRIVER_WIFI_MODULE_NAME);

    return 0;
}

/****************************** INI配置 ******************************/

/**
 * @brief 准备指定工作模式对应的HI1105驱动INI，并输出文件内容是否发生变化。
 *
 * @note 仅在目标INI与当前生效INI内容不一致时执行原子替换；
 *       本接口只更新INI文件，不负责重新上电驱动加载新配置。
 */
int wifi_driver_prepare_ini(linkg_wifi_work_mode_t work_mode, bool *changed)
{
    const char *source_path;
    bool matches;
    int ret;

    if (changed == NULL)
    {
        return -EINVAL;
    }

    *changed = false;

    switch (work_mode)
    {
        case LINKG_WIFI_WORK_MODE_NARROW:
            source_path = WIFI_DRIVER_NARROWBAND_INI_PATH;
            break;

        case LINKG_WIFI_WORK_MODE_WIDE:
            source_path = WIFI_DRIVER_WIDEBAND_INI_PATH;
            break;

        default:
            WIFI_DRIVER_WARN("invalid Wi-Fi work mode, mode=%d", work_mode);
            return -EINVAL;
    }

    ret = _wifi_driver_ini_matches(source_path, &matches);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("compare driver INI failed, source=%s, target=%s, error=%d",
                          source_path,
                          WIFI_DRIVER_ACTIVE_INI_PATH,
                          ret);

        return ret;
    }

    if (matches)
    {
        WIFI_DRIVER_DEBUG("driver INI already matches, mode=%d, target=%s",
                          work_mode,
                          WIFI_DRIVER_ACTIVE_INI_PATH);

        return 0;
    }

    ret = linkg_file_replace_atomic(source_path, WIFI_DRIVER_ACTIVE_INI_PATH, WIFI_DRIVER_INI_MAX_SIZE);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("replace driver INI failed, mode=%d, source=%s, target=%s, error=%d",
                          work_mode,
                          source_path,
                          WIFI_DRIVER_ACTIVE_INI_PATH,
                          ret);

        return ret;
    }

    *changed = true;

    WIFI_DRIVER_INFO("driver INI replaced, mode=%d, source=%s, target=%s",
                     work_mode,
                     source_path,
                     WIFI_DRIVER_ACTIVE_INI_PATH);

    return 0;
}

/**
 * @brief 更新当前HI1105生效INI到指定宽窄带工作模式。
 *
 * @note 本接口只更新INI文件，不负责重新上电驱动；运行期切换工作模式时，
 *       调用方更新成功后仍需按生命周期流程触发驱动重新加载INI。
 */
int wifi_driver_update_ini(linkg_wifi_work_mode_t work_mode)
{
    bool changed;

    return wifi_driver_prepare_ini(work_mode, &changed);
}
