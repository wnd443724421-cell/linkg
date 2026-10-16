/**
 * @file linkg_device_info.c
 * @brief LinkG设备身份、构建信息和运行角色统一查询实现
 */
#include "linkg_device_info.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_build_info.h"
#include "linkg_config.h"
#include "linkg_file.h"
#include "linkg_gpio_manager.h"
#include "linkg_log.h"
#include "linkg_product.h"

/****************************** Private配置 ******************************/

#define LINKG_DEVICE_PRIVATE_DEV     "/dev/by-name/private"                      		// NOR私有分区设备
#define LINKG_DEVICE_PRIVATE_MOUNT   "/tmp/linkg-private"                        		// 模块专属临时挂载目录
#define LINKG_DEVICE_SN_PATH         LINKG_DEVICE_PRIVATE_MOUNT "/ULI/factory/SN.txt"   // DragonSN SN
#define LINKG_DEVICE_HW_PATH         LINKG_DEVICE_PRIVATE_MOUNT "/ULI/factory/HW.txt"   // DragonSN HW

/****************************** 全局缓存 ******************************/

typedef struct
{
    pthread_mutex_t            lock;                                      			// 并发保护
    bool                       initialized;                               			// 是否已经加载过Private
    char                       serial_number[LINKG_DEVICE_SERIAL_MAX];    			// SN只读缓存
    char                       hardware_version[LINKG_DEVICE_HARDWARE_VERSION_MAX]; // HW只读缓存
    linkg_device_field_state_t serial_state;                            			// SN状态
    linkg_device_field_state_t hardware_state;                          			// HW状态
} linkg_device_info_context_t;

static linkg_device_info_context_t g_device_info =
{
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取DragonSN单个Key，不限制SN/HW的业务格式。
 * @note 只裁剪末尾CR/LF；拒绝超长、内嵌NUL和控制字符。
 */
static linkg_device_field_state_t _linkg_device_info_read_key(const char *path, char *out, size_t capacity)
{
    char   *contents;
    size_t  length;
    size_t  index;
    int     ret;

    if (path == NULL || out == NULL || capacity < 2U)
    {
        return LINKG_DEVICE_FIELD_ERROR;
    }

    out[0] = '\0';
    contents = NULL;
    length   = 0U;

    /* 多读一个字节以区分长度恰好等于缓冲区上限的非法情况。 */
    ret = linkg_file_read_all(path, capacity, &contents, &length);
    if (ret == -ENOENT)
    {
        return LINKG_DEVICE_FIELD_MISSING;
    }

    if (ret != 0)
    {
        return LINKG_DEVICE_FIELD_ERROR;
    }

    while (length > 0U && (contents[length - 1U] == '\n' || contents[length - 1U] == '\r'))
    {
        length--;
    }

    if (length == 0U)
    {
        free(contents);
        return LINKG_DEVICE_FIELD_MISSING;
    }

    if (length >= capacity)
    {
        free(contents);
        return LINKG_DEVICE_FIELD_ERROR;
    }

    for (index = 0U; index < length; index++)
    {
        unsigned char value = (unsigned char)contents[index];

        if (value < 0x20U || value == 0x7FU)
        {
            free(contents);
            return LINKG_DEVICE_FIELD_ERROR;
        }
    }

    memcpy(out, contents, length);
    out[length] = '\0';
    free(contents);

    return LINKG_DEVICE_FIELD_VALID;
}

/**
 * @brief 判断未初始化的NOR Private分区是否全部处于擦除态。
 * @note 仅在VFAT挂载失败时执行；读失败不能等同于分区未烧录。
 */
static bool _linkg_device_info_private_is_erased(void)
{
    unsigned char buffer[512];
    size_t        count;
    size_t        index;
    size_t        total;
    FILE         *stream;
    bool          erased;

    stream = fopen(LINKG_DEVICE_PRIVATE_DEV, "rb");
    if (stream == NULL)
    {
        return false;
    }

    erased = true;
    total  = 0U;

    while ((count = fread(buffer, 1U, sizeof(buffer), stream)) > 0U)
    {
        total += count;
        for (index = 0U; index < count; index++)
        {
            if (buffer[index] != 0xFFU)
            {
                erased = false;
                break;
            }
        }

        if (!erased)
        {
            break;
        }
    }

    if (ferror(stream))
    {
        erased = false;
    }

    fclose(stream);
    return erased && total > 0U;
}

/**
 * @brief 只读加载Private数据。挂载失败不阻止程序运行。
 */
static void _linkg_device_info_load_private(void)
{
    struct stat st;
    int         ret;

    g_device_info.serial_state   = LINKG_DEVICE_FIELD_ERROR;
    g_device_info.hardware_state = LINKG_DEVICE_FIELD_ERROR;

    if (mkdir(LINKG_DEVICE_PRIVATE_MOUNT, 0700) != 0 && errno != EEXIST)
    {
        LINKG_LOG_WARN("DEVICE: create private mount dir failed, error=%d", errno);
        return;
    }

    if (lstat(LINKG_DEVICE_PRIVATE_MOUNT, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        LINKG_LOG_WARN("DEVICE: private mount path is not a directory");
        return;
    }

    /* 严禁调用mount失败后直接读取挂载目录中的旧文件。 */
    if (mount(LINKG_DEVICE_PRIVATE_DEV,
              LINKG_DEVICE_PRIVATE_MOUNT,
              "vfat",
              MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC,
              NULL) != 0)
    {
        int mount_error = errno;
        if (_linkg_device_info_private_is_erased())
        {
            g_device_info.serial_state   = LINKG_DEVICE_FIELD_MISSING;
            g_device_info.hardware_state = LINKG_DEVICE_FIELD_MISSING;
            LINKG_LOG_INFO("DEVICE: private not provisioned");
        }
        else
        {
            LINKG_LOG_WARN("DEVICE: mount private readonly failed, error=%d", mount_error);
        }
        return;
    }

    g_device_info.serial_state   = _linkg_device_info_read_key(LINKG_DEVICE_SN_PATH, g_device_info.serial_number, sizeof(g_device_info.serial_number));
    g_device_info.hardware_state = _linkg_device_info_read_key(LINKG_DEVICE_HW_PATH, g_device_info.hardware_version, sizeof(g_device_info.hardware_version));

    ret = umount(LINKG_DEVICE_PRIVATE_MOUNT);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DEVICE: umount private failed, error=%d", errno);
    }
	LINKG_LOG_INFO("DEVICE: initialized, sn_state=%d, hw_state=%d, version=%s, build=%s",
               (int)g_device_info.serial_state,
               (int)g_device_info.hardware_state,
               LINKG_SOFTWARE_VERSION,
               LINKG_BUILD_DATE);
}

/****************************** 对外接口 ******************************/

int linkg_device_info_init(void)
{
    pthread_mutex_lock(&g_device_info.lock);

    if (!g_device_info.initialized)
    {
        memset(g_device_info.serial_number, 0, sizeof(g_device_info.serial_number));
        memset(g_device_info.hardware_version, 0, sizeof(g_device_info.hardware_version));
        _linkg_device_info_load_private();
        g_device_info.initialized = true;
    }

    pthread_mutex_unlock(&g_device_info.lock);

    LINKG_LOG_INFO("DEVICE: initialized, version=%s, build=%s", LINKG_SOFTWARE_VERSION, LINKG_BUILD_DATE);

    return 0;
}

void linkg_device_info_deinit(void)
{
    pthread_mutex_lock(&g_device_info.lock);

    memset(g_device_info.serial_number, 0, sizeof(g_device_info.serial_number));
    memset(g_device_info.hardware_version, 0, sizeof(g_device_info.hardware_version));
    g_device_info.serial_state   = LINKG_DEVICE_FIELD_MISSING;
    g_device_info.hardware_state = LINKG_DEVICE_FIELD_MISSING;
    g_device_info.initialized    = false;

    pthread_mutex_unlock(&g_device_info.lock);
}

int linkg_device_info_get(linkg_device_info_t *out)
{
    linkg_device_config_t device;
    uint8_t               hardware_id;

    if (out == NULL)
    {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->model, sizeof(out->model), "%s", LINKG_PRODUCT_MODEL);
    snprintf(out->software_version, sizeof(out->software_version), "%s", LINKG_SOFTWARE_VERSION);
    snprintf(out->build_date, sizeof(out->build_date), "%s", LINKG_BUILD_DATE);

    pthread_mutex_lock(&g_device_info.lock);

    if (!g_device_info.initialized)
    {
        pthread_mutex_unlock(&g_device_info.lock);
        return -ENODEV;
    }

    memcpy(out->serial_number, g_device_info.serial_number, sizeof(out->serial_number));
    memcpy(out->hardware_version, g_device_info.hardware_version, sizeof(out->hardware_version));
    out->serial_state   = g_device_info.serial_state;
    out->hardware_state = g_device_info.hardware_state;

    pthread_mutex_unlock(&g_device_info.lock);

    out->role      = LINKG_DEVICE_ROLE_UNKNOWN;
    out->wifi_type = LINKG_WIFI_HARDWARE_UNKNOWN;

    if (linkg_config_get_device(&device) == 0)
    {
        out->role = device.role;
    }

    if (linkg_gpio_manager_get_hardware_id(&hardware_id) == 0)
    {
        out->wifi_type = linkg_wifi_hardware_from_id(hardware_id);
    }

    return 0;
}
