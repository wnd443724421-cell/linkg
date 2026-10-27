/**
 * @file wifi_driver_loader.c
 * @brief LinkG HI1105 Wi-Fi驱动加载与国产/进口资源选择
 * @author Dawn
 * @version 1.1.0
 * @date 2026-10-13
 */

#include "wifi_driver_loader.h"

#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_os.h"
#include "linkg_text.h"
#include "linkg_wifi_hardware.h"
#include "wifi_platform_internal.h"

/****************************** 模块常量 ******************************/

#define WIFI_DRIVER_MODULE_LINE_MAX              256U               // /proc/modules单行缓冲区长度
#define WIFI_DRIVER_SYMLINK_RETRY_COUNT          16U                // 临时软链接名称冲突重试次数
#define WIFI_DRIVER_AX_INI_KEY                   "11ax_switch_mask" // AX配置键
#define WIFI_DRIVER_AX_MASK_AP_ENABLED           "0x2B"             // AP开启AX
#define WIFI_DRIVER_AX_MASK_AP_DISABLED          "0x00"             // AP关闭AX
#define WIFI_DRIVER_AX_MASK_STA_ONLY             "0x23"             // STA固定AX掩码


/****************************** 内部辅助 ******************************/

/**
 * @brief 检查指定内核模块是否已经加载。
 */
static bool _wifi_driver_module_loaded(const char *module_name)
{
    FILE   *stream;
    char    line[WIFI_DRIVER_MODULE_LINE_MAX];
    size_t  name_length;
    bool    loaded;

    if (module_name == NULL || module_name[0] == '\0')
    {
        return false;
    }

    stream      = NULL;
    name_length = strlen(module_name);
    loaded      = false;

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
 * @brief 加载指定内核模块，已加载时直接返回成功。
 */
static int _wifi_driver_load_module(const char *module_path, const char *module_name)
{
    int saved_errno;
    int ret;

    if (module_path == NULL || module_path[0] == '\0' || module_name == NULL || module_name[0] == '\0')
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
        WIFI_DRIVER_ERROR("module file unavailable, module=%s, path=%s, error=%d", module_name, module_path, saved_errno);
        return -saved_errno;
    }

    WIFI_DRIVER_DEBUG("loading module, module=%s, path=%s", module_name, module_path);

    ret = linkg_os_run("insmod", module_path, NULL);
    if (ret != 0 && !_wifi_driver_module_loaded(module_name))
    {
        WIFI_DRIVER_ERROR("load module failed, module=%s, path=%s, error=%d", module_name, module_path, ret);
        return ret;
    }

    return 0;
}

/**
 * @brief 拼接一个资源目录下的相对路径。
 */
static int _wifi_driver_join_path(const char *directory, const char *relative, char *path, size_t capacity)
{
    int length;

    if (directory == NULL || relative == NULL || path == NULL || capacity == 0U)
    {
        return -EINVAL;
    }

    length = snprintf(path, capacity, "%s/%s", directory, relative);
    if (length < 0 || (size_t)length >= capacity)
    {
        return -ENAMETOOLONG;
    }

    return 0;
}

/**
 * @brief 验证文件存在、可读取且为普通文件。
 *
 * @note stat跟随资源目录中的软链接；max_size为0时不检查大小。
 */
static int _wifi_driver_check_file(const char *path, size_t max_size)
{
    struct stat file_stat;

    if (path == NULL)
    {
        return -EINVAL;
    }

    if (stat(path, &file_stat) != 0)
    {
        return -errno;
    }

    if (!S_ISREG(file_stat.st_mode))
    {
        return -EINVAL;
    }

    if (max_size != 0U && (file_stat.st_size < 0 || (uint64_t)file_stat.st_size > max_size))
    {
        return -EFBIG;
    }

    if (access(path, R_OK) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 检查软链接是否恰好指向预期相对目标。
 *
 * @note allow_regular仅用于旧版INI普通文件迁移；固件active不允许覆盖真实目录。
 */
static int _wifi_driver_symlink_matches(const char *path, const char *expected, bool allow_regular, bool *matches)
{
    struct stat path_stat;
    char        actual[PATH_MAX];
    ssize_t     length;

    if (path == NULL || expected == NULL || matches == NULL)
    {
        return -EINVAL;
    }

    *matches = false;

    if (lstat(path, &path_stat) != 0)
    {
        return errno == ENOENT ? 0 : -errno;
    }

    if (!S_ISLNK(path_stat.st_mode))
    {
        return allow_regular && S_ISREG(path_stat.st_mode) ? 0 : -EINVAL;
    }

    length = readlink(path, actual, sizeof(actual) - 1U);
    if (length < 0)
    {
        return -errno;
    }

    actual[length] = '\0';
    *matches = strcmp(actual, expected) == 0;

    return 0;
}

/**
 * @brief 使用临时软链接加rename原子替换一个链接目标。
 *
 * @note 临时链接与目标位于同一目录；不删除正在使用的旧链接。
 */
static int _wifi_driver_replace_symlink_atomic(const char *path, const char *target)
{
    char         temporary[PATH_MAX];
    unsigned int attempt;
    int          length;
    int          ret;

    if (path == NULL || target == NULL)
    {
        return -EINVAL;
    }

    for (attempt = 0U; attempt < WIFI_DRIVER_SYMLINK_RETRY_COUNT; attempt++)
    {
        length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%u", path, (long)getpid(), attempt);
        if (length < 0 || (size_t)length >= sizeof(temporary))
        {
            return -ENAMETOOLONG;
        }

        if (symlink(target, temporary) == 0)
        {
            break;
        }

        if (errno != EEXIST)
        {
            return -errno;
        }
    }

    if (attempt == WIFI_DRIVER_SYMLINK_RETRY_COUNT)
    {
        return -EEXIST;
    }

    if (rename(temporary, path) != 0)
    {
        ret = -errno;
        (void)unlink(temporary);
        return ret;
    }

    return 0;
}

/****************************** 驱动加载 ******************************/

/**
 * @brief 检查HI1105平台和Wi-Fi模块是否均已加载。
 */
bool wifi_driver_is_loaded(void)
{
    bool platform_loaded;
    bool wifi_loaded;

    platform_loaded = _wifi_driver_module_loaded(WIFI_DRIVER_PLATFORM_MODULE_NAME);
    wifi_loaded     = _wifi_driver_module_loaded(WIFI_DRIVER_WIFI_MODULE_NAME);

    return platform_loaded && wifi_loaded;
}

/**
 * @brief 按依赖顺序加载HI1105内核模块。
 */
int wifi_driver_load(void)
{
    bool platform_loaded;
    bool wifi_loaded;
    int  ret;

    if (wifi_driver_is_loaded())
    {
        WIFI_DRIVER_DEBUG("driver modules already loaded");
        return 0;
    }

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
    wifi_loaded     = _wifi_driver_module_loaded(WIFI_DRIVER_WIFI_MODULE_NAME);

    if (!platform_loaded || !wifi_loaded)
    {
        WIFI_DRIVER_ERROR("driver modules not ready, platform=%d, wifi=%d", platform_loaded, wifi_loaded);
        return -ENODEV;
    }

    WIFI_DRIVER_INFO("driver modules ready, platform=%s, wifi=%s", WIFI_DRIVER_PLATFORM_MODULE_NAME, WIFI_DRIVER_WIFI_MODULE_NAME);

    return 0;
}

/****************************** INI配置 ******************************/

/**
 * @brief 根据Wi-Fi硬件类型及工作模式选择INI文件。
 */
static int _wifi_driver_ini_target(uint8_t hardware_id, linkg_wifi_work_mode_t work_mode, const char **target)
{
    linkg_wifi_hardware_type_t type;

    if (target == NULL)
    {
        return -EINVAL;
    }

    *target = NULL;

    type = linkg_wifi_hardware_from_id(hardware_id);

    switch (type)
    {
        case LINKG_WIFI_HARDWARE_DOMESTIC:
        {
            if (work_mode == LINKG_WIFI_WORK_MODE_NARROW)
            {
                *target = WIFI_DRIVER_DOMESTIC_NARROW_INI_TARGET;
            }
            else if (work_mode == LINKG_WIFI_WORK_MODE_WIDE)
            {
                *target = WIFI_DRIVER_DOMESTIC_WIDE_INI_TARGET;
            }

            break;
        }

        case LINKG_WIFI_HARDWARE_IMPORTED:
        {
            if (work_mode == LINKG_WIFI_WORK_MODE_NARROW)
            {
                *target = WIFI_DRIVER_IMPORTED_NARROW_INI_TARGET;
            }
            else if (work_mode == LINKG_WIFI_WORK_MODE_WIDE)
            {
                *target = WIFI_DRIVER_IMPORTED_WIDE_INI_TARGET;
            }

            break;
        }

        default:
            return -EINVAL;
    }

    return *target != NULL ? 0 : -EINVAL;
}

/**
 * @brief 选择硬件版本和宽窄带对应的INI软链接，并报告链接是否变更。
 *
 * @note 仅变更磁盘上的链接；驱动已经加载时，由调用方决定何时重新读取INI。
 */
int wifi_driver_prepare_ini(uint8_t hardware_id, linkg_wifi_work_mode_t work_mode, bool *changed)
{
    const char *target;
    char        path[PATH_MAX];
    bool        matches;
    int         ret;

    if (changed == NULL)
    {
        return -EINVAL;
    }

    *changed = false;

    ret = _wifi_driver_ini_target(hardware_id, work_mode, &target);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("invalid INI selector, hardware_id=%u, work_mode=%d", (unsigned int)hardware_id, (int)work_mode);
        return ret;
    }

    ret = _wifi_driver_join_path(WIFI_DRIVER_INI_DIRECTORY, target, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_check_file(path, WIFI_DRIVER_INI_MAX_SIZE);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("INI file unavailable, path=%s, error=%d", path, ret);
        return ret;
    }

    ret = _wifi_driver_symlink_matches(WIFI_DRIVER_ACTIVE_INI_PATH, target, true, &matches);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("check active INI link failed, error=%d", ret);
        return ret;
    }

    if (matches)
    {
        WIFI_DRIVER_DEBUG("driver INI already selected, target=%s", target);
        return 0;
    }

    ret = _wifi_driver_replace_symlink_atomic(WIFI_DRIVER_ACTIVE_INI_PATH, target);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("select driver INI failed, target=%s, error=%d", target, ret);
        return ret;
    }

    *changed = true;
    WIFI_DRIVER_INFO("driver INI selected, hardware_id=%u, mode=%d, target=%s", (unsigned int)hardware_id, (int)work_mode, target);

    return 0;
}

/**
 * @brief INI切换便捷接口，不负责驱动重新上电。
 */
int wifi_driver_update_ini(uint8_t hardware_id, linkg_wifi_work_mode_t work_mode)
{
    bool changed;

    return wifi_driver_prepare_ini(hardware_id, work_mode, &changed);
}

/**
 * @brief 根据角色和宽带协议获取目标AX配置值。
 */
static int _wifi_driver_ax_mask_value(linkg_device_role_t role, linkg_wifi_protocol_t protocol, const char **value)
{
    if (value == NULL)
    {
        return -EINVAL;
    }

    *value = NULL;

    if (role == LINKG_DEVICE_ROLE_STA)
    {
        *value = WIFI_DRIVER_AX_MASK_STA_ONLY;
        return 0;
    }

    if (role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    switch (protocol)
    {
        case LINKG_WIFI_PROTOCOL_N:
        case LINKG_WIFI_PROTOCOL_AC:
            *value = WIFI_DRIVER_AX_MASK_AP_DISABLED;
            return 0;

        case LINKG_WIFI_PROTOCOL_AX:
            *value = WIFI_DRIVER_AX_MASK_AP_ENABLED;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 获取活动软链接选中的实际宽带INI路径。
 *
 * @note 仅接受国产/进口宽带目标，不允许修改窄带INI。
 */
static int _wifi_driver_selected_wide_ini_path(char *path, size_t capacity)
{
    char    target[PATH_MAX];
    ssize_t length;

    if (path == NULL || capacity == 0U)
    {
        return -EINVAL;
    }

    length = readlink(WIFI_DRIVER_ACTIVE_INI_PATH, target, sizeof(target) - 1U);
    if (length < 0)
    {
        return -errno;
    }

    target[length] = '\0';

    if (strcmp(target, WIFI_DRIVER_IMPORTED_WIDE_INI_TARGET) != 0 &&
        strcmp(target, WIFI_DRIVER_DOMESTIC_WIDE_INI_TARGET) != 0)
    {
        return -EINVAL;
    }

    return _wifi_driver_join_path(WIFI_DRIVER_INI_DIRECTORY, target, path, capacity);
}

/**
 * @brief 更新当前宽带INI内的11ax_switch_mask，并报告内容是否发生变化。
 *
 * @note 必须先调用wifi_driver_prepare_ini()选定宽带INI。
 * @note 仅修改磁盘内容，不执行驱动reload。调用方应在Wi-Fi Owner停止服务后执行。
 */
int wifi_driver_update_ax_ini(linkg_device_role_t role, linkg_wifi_protocol_t protocol, bool *changed)
{
    struct stat file_stat;
    const char *value;
    char        path[PATH_MAX];
    char       *content;
    char       *original;
    size_t      length;
    size_t      original_length;
    int         ret;

    if (changed == NULL)
    {
        return -EINVAL;
    }

    *changed = false;

    ret = _wifi_driver_ax_mask_value(role, protocol, &value);
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_driver_selected_wide_ini_path(path, sizeof(path));
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("wide INI is not selected, error=%d", ret);
        return ret;
    }

    if (lstat(path, &file_stat) != 0)
    {
        return -errno;
    }

    if (!S_ISREG(file_stat.st_mode))
    {
        return -EINVAL;
    }

    content = NULL;
    length  = 0U;

    ret = linkg_file_read_all(path, WIFI_DRIVER_INI_MAX_SIZE, &content, &length);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("read wide INI failed, path=%s, error=%d", path, ret);
        return ret;
    }

    original_length = length;
    original = malloc(original_length + 1U);
    if (original == NULL)
    {
        free(content);
        return -ENOMEM;
    }

    memcpy(original, content, original_length);
    original[original_length] = '\0';

    /* 目标键必须存在，缺失直接失败，避免生成不符合预期的驱动INI。 */
    ret = linkg_text_replace_key_value(&content, &length, WIFI_DRIVER_AX_INI_KEY, value);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("replace AX INI key failed, path=%s, error=%d", path, ret);
        goto cleanup;
    }

    if (length == original_length && memcmp(content, original, length) == 0)
    {
        ret = 0;
        goto cleanup;
    }

    if (length > WIFI_DRIVER_INI_MAX_SIZE)
    {
        ret = -EFBIG;
        goto cleanup;
    }

    /* 不得原子替换WIFI_DRIVER_ACTIVE_INI_PATH，否则会把软链接替换成普通文件。 */
    ret = linkg_file_write_all_atomic(path, content, length, file_stat.st_mode & 0777U);
    if (ret != 0)
    {
        WIFI_DRIVER_ERROR("write AX INI failed, path=%s, error=%d", path, ret);
        goto cleanup;
    }

    *changed = true;
    WIFI_DRIVER_INFO("AX INI updated, role=%d, protocol=%d, mask=%s, path=%s", (int)role, (int)protocol, value, path);

cleanup:
    free(original);
    free(content);
    return ret;
}

