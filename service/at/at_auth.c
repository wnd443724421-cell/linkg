/**
 * @file at_auth.c
 * @brief LinkG AT工厂权限管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_AT_FACTORY_PASSWORD_PATH  "/etc/linkg/factory_password" // 工厂密码文件，仅root可读
#define LINKG_AT_FACTORY_PASSWORD_SIZE  64U                           // 32字节随机数的十六进制文本长度
#define LINKG_AT_FACTORY_TIMEOUT_US     60000000ULL                   // 工厂授权有效期，60秒
#define LINKG_AT_FACTORY_RETRY_US       1000000ULL                    // 认证失败重试间隔，1秒

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_at_request_context_t owner;            // 当前工厂会话所属UDP客户端
    uint64_t                   expires_us;       // 授权截止时间
    uint64_t                   retry_us;         // 失败后允许再次认证的时间
    bool                       authorized;       // 是否存在已授权会话
} linkg_at_auth_context_t;

/****************************** 全局上下文 ******************************/

// 运行期间仅由AT服务线程访问，线程回收后由生命周期接口清理。
static linkg_at_auth_context_t g_at_auth;

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取受保护的固定工厂密码，不接受符号链接和非普通文件。
 */
static int _linkg_at_auth_read_password(char *password)
{
    struct stat status;
    char        buffer[LINKG_AT_FACTORY_PASSWORD_SIZE + 2U];
    size_t      length;
    ssize_t     count;
    int         fd;
    int         ret;

    fd = open(LINKG_AT_FACTORY_PASSWORD_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
    {
        return -errno;
    }

    ret = 0;
    length = 0U;

    if (fstat(fd, &status) != 0)
    {
        ret = -errno;
    }
    else if (!S_ISREG(status.st_mode) || status.st_uid != 0U || (status.st_mode & 0077U) != 0U)
    {
        ret = -EACCES;
    }

    while (ret == 0 && length < sizeof(buffer))
    {
        count = read(fd, buffer + length, sizeof(buffer) - length);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            ret = -errno;
            break;
        }

        if (count == 0)
        {
            break;
        }

        length += (size_t)count;
    }

    close(fd);

    if (ret == 0)
    {
        if (length == LINKG_AT_FACTORY_PASSWORD_SIZE + 1U && buffer[length - 1U] == '\n')
        {
            length--;
        }

        if (length != LINKG_AT_FACTORY_PASSWORD_SIZE)
        {
            ret = -EINVAL;
        }
        else
        {
            buffer[length] = '\0';

            if (strspn(buffer, "0123456789abcdefABCDEF") != length)
            {
                ret = -EINVAL;
            }
            else
            {
                memcpy(password, buffer, length + 1U);
            }
        }
    }

    return ret;
}

/**
 * @brief 比较当前请求来源与已授权会话。
 */
static bool _linkg_at_auth_same_client(const linkg_at_request_context_t *context)
{
    return context->peer.sin_addr.s_addr == g_at_auth.owner.peer.sin_addr.s_addr &&
           context->peer.sin_port == g_at_auth.owner.peer.sin_port &&
           context->local_address.s_addr == g_at_auth.owner.local_address.s_addr &&
           context->interface_index == g_at_auth.owner.interface_index;
}

/****************************** 权限管理 ******************************/

/**
 * @brief 清除工厂会话及认证限频状态。
 */
void _linkg_at_auth_reset(void)
{
    memset(&g_at_auth, 0, sizeof(g_at_auth));
}

/**
 * @brief 验证固定密码并建立单个有时限的工厂会话。
 */
int _linkg_at_auth_grant(const linkg_at_request_context_t *context, const char *arguments)
{
    char         password[LINKG_AT_FACTORY_PASSWORD_SIZE + 1U];
    uint64_t     now_us;
    unsigned int difference;
    size_t       index;
    int          ret;

    if (context == NULL || arguments == NULL)
    {
        return -EINVAL;
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U)
    {
        return -EIO;
    }

    if (now_us < g_at_auth.retry_us)
    {
        return -EAGAIN;
    }

    if (strlen(arguments) != LINKG_AT_FACTORY_PASSWORD_SIZE)
    {
        g_at_auth.retry_us = now_us + LINKG_AT_FACTORY_RETRY_US;
        return -EACCES;
    }

    ret = _linkg_at_auth_read_password(password);
    if (ret != 0)
    {
        g_at_auth.retry_us = now_us + LINKG_AT_FACTORY_RETRY_US;
        return ret;
    }

    difference = 0U;
    for (index = 0U; index < LINKG_AT_FACTORY_PASSWORD_SIZE; index++)
    {
        difference |= (unsigned char)password[index] ^ (unsigned char)arguments[index];
    }

    if (difference != 0U)
    {
        g_at_auth.retry_us = now_us + LINKG_AT_FACTORY_RETRY_US;
        return -EACCES;
    }

    g_at_auth.owner      = *context;
    g_at_auth.expires_us = now_us + LINKG_AT_FACTORY_TIMEOUT_US;
    g_at_auth.retry_us   = 0U;
    g_at_auth.authorized = true;

    return 0;
}

/**
 * @brief 检查当前UDP客户端是否持有未过期的工厂会话。
 */
bool _linkg_at_auth_allowed(const linkg_at_request_context_t *context)
{
    uint64_t now_us;

    if (context == NULL || !g_at_auth.authorized)
    {
        return false;
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U || now_us >= g_at_auth.expires_us)
    {
        g_at_auth.authorized = false;
        return false;
    }

    return _linkg_at_auth_same_client(context);
}
