/**
 * @file linkg_os_ports.c
 * @brief Linux自动本地端口分配保护
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include "linkg_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_OS_PORTS_SYSCTL_PATH   "/proc/sys/net/ipv4/ip_local_reserved_ports" // Linux本地自动端口排除列表
#define LINKG_OS_PORTS_TEXT_SIZE     4096U                                         // Linux 4KB页面内的sysctl文本长度
#define LINKG_OS_PORTS_BITMAP_SIZE   (65536U / 8U)                                // TCP/UDP所有16位端口的位图大小

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查给定端口是否已经加入排除位图。
 */
static bool _linkg_os_port_test(const uint8_t *bitmap, uint32_t port)
{
    return (bitmap[port / 8U] & (uint8_t)(1U << (port % 8U))) != 0U;
}

/**
 * @brief 合并指定端口范围，返回是否存在新增加的端口。
 */
static bool _linkg_os_port_add(uint8_t *bitmap, uint32_t first, uint32_t last)
{
    bool changed = false;
    uint32_t port;

    for (port = first; port <= last; port++)
    {
        if (!_linkg_os_port_test(bitmap, port))
        {
            bitmap[port / 8U] |= (uint8_t)(1U << (port % 8U));
            changed = true;
        }
    }

    return changed;
}

/**
 * @brief 解析内核当前的保留端口列表，支持单端口和区间。
 *
 * 输入字符串会被strtok_r分割；遇到未知格式则失败，不覆盖已有规则。
 */
static int _linkg_os_ports_parse(char *text, uint8_t *bitmap)
{
    char         *token;
    char         *cursor;
    char         *end;
    char         *save = NULL;
    unsigned long first;
    unsigned long last;

    token = strtok_r(text, ", \t\r\n", &save);
    while (token != NULL)
    {
        if (*token < '0' || *token > '9')
        {
            return -EINVAL;
        }

        errno = 0;
        first = strtoul(token, &end, 10);
        if (errno != 0 || end == token || first == 0UL || first > UINT16_MAX)
        {
            return -EINVAL;
        }

        last = first;
        if (*end == '-')
        {
            cursor = end + 1;
            if (*cursor < '0' || *cursor > '9')
            {
                return -EINVAL;
            }

            errno = 0;
            last = strtoul(cursor, &end, 10);
            if (errno != 0 || end == cursor || last > UINT16_MAX || last < first)
            {
                return -EINVAL;
            }
        }

        if (*end != '\0')
        {
            return -EINVAL;
        }

        (void)_linkg_os_port_add(bitmap, (uint32_t)first, (uint32_t)last);
        token = strtok_r(NULL, ", \t\r\n", &save);
    }

    return 0;
}

/**
 * @brief 从内核读取端口排除配置，不允许静默截断已有列表。
 */
static int _linkg_os_ports_read(char *buffer, size_t capacity)
{
    ssize_t count;
    int     fd;
    int     ret;

    fd = open(LINKG_OS_PORTS_SYSCTL_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    count = read(fd, buffer, capacity - 1U);
    if (count < 0)
    {
        ret = -errno;
    }
    else if ((size_t)count >= capacity - 1U)
    {
        ret = -E2BIG;
    }
    else
    {
        buffer[count] = '\0';
        ret = 0;
    }

    if (close(fd) != 0 && ret == 0)
    {
        ret = -errno;
    }

    return ret;
}

/**
 * @brief 将排除位图转换为有序且互不重叠的sysctl端口区间。
 */
static int _linkg_os_ports_format(const uint8_t *bitmap, char *buffer, size_t capacity)
{
    uint32_t first;
    uint32_t last;
    uint32_t port;
    size_t   offset = 0U;
    int      count;

    for (port = 1U; port <= UINT16_MAX;)
    {
        if (!_linkg_os_port_test(bitmap, port))
        {
            port++;
            continue;
        }

        first = port;
        do
        {
            port++;
        } while (port <= UINT16_MAX && _linkg_os_port_test(bitmap, port));
        last = port - 1U;

        if (first == last)
        {
            count = snprintf(buffer + offset, capacity - offset, "%s%u",
                             offset == 0U ? "" : ",", first);
        }
        else
        {
            count = snprintf(buffer + offset, capacity - offset, "%s%u-%u",
                             offset == 0U ? "" : ",", first, last);
        }

        if (count < 0 || (size_t)count >= capacity - offset)
        {
            return -E2BIG;
        }

        offset += (size_t)count;
    }

    return offset > 0U ? 0 : -EINVAL;
}

/**
 * @brief 写入一次内核sysctl配置；该配置对整个网络命名空间生效。
 */
static int _linkg_os_ports_write(const char *text)
{
    ssize_t count;
    size_t  size;
    int     fd;
    int     ret;

    fd = open(LINKG_OS_PORTS_SYSCTL_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    size = strlen(text);
    count = write(fd, text, size);
    if (count < 0)
    {
        ret = -errno;
    }
    else
    {
        ret = (size_t)count == size ? 0 : -EIO;
    }

    if (close(fd) != 0 && ret == 0)
    {
        ret = -errno;
    }

    return ret;
}

/****************************** 系统管理 ******************************/

/**
 * @brief 从现有内核列表合并LinkG固定端口与NAT端口池。
 *
 * 仅约束Linux自动本地源端口分配；不限制显式bind或NAT内部选端口。
 * 系统级配置不在LinkG退出时清除，避免影响其他进程和并发业务。
 */
int linkg_os_reserve_local_ports(void)
{
    uint8_t bitmap[LINKG_OS_PORTS_BITMAP_SIZE] = {0};
    char    existing[LINKG_OS_PORTS_TEXT_SIZE];
    char    updated[LINKG_OS_PORTS_TEXT_SIZE];
    uint32_t port;
    bool     changed;
    int      ret;

    ret = _linkg_os_ports_read(existing, sizeof(existing));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_os_ports_parse(existing, bitmap);
    if (ret != 0)
    {
        return ret;
    }

    changed = _linkg_os_port_add(bitmap, LINKG_RESOURCE_PORT_RESERVED_START,
                                LINKG_RESOURCE_PORT_RESERVED_END);
    changed = _linkg_os_port_add(bitmap, LINKG_RESOURCE_NAT_SNAT_PORT_START,
                                 LINKG_RESOURCE_NAT_SNAT_PORT_END) || changed;
    if (!changed)
    {
        return 0;
    }

    memset(updated, 0, sizeof(updated));
    ret = _linkg_os_ports_format(bitmap, updated, sizeof(updated));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_os_ports_write(updated);
    if (ret != 0)
    {
        return ret;
    }

    // 写入后回读，确认内核实际接受了两个LinkG保留区间。
    memset(bitmap, 0, sizeof(bitmap));
    ret = _linkg_os_ports_read(existing, sizeof(existing));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_os_ports_parse(existing, bitmap);
    if (ret != 0)
    {
        return ret;
    }

    for (port = LINKG_RESOURCE_PORT_RESERVED_START; port <= LINKG_RESOURCE_PORT_RESERVED_END; port++)
    {
        if (!_linkg_os_port_test(bitmap, port))
        {
            return -EIO;
        }
    }

    for (port = LINKG_RESOURCE_NAT_SNAT_PORT_START; port <= LINKG_RESOURCE_NAT_SNAT_PORT_END; port++)
    {
        if (!_linkg_os_port_test(bitmap, port))
        {
            return -EIO;
        }
    }

    return 0;
}
