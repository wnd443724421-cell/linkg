/**
 * @file system_install_test.c
 * @brief 隔离系统安装测试入口，通过链接包装将目标根目录限定在临时目录
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "upgrade_system.h"

/****************************** 测试上下文 ******************************/

static const char *g_test_root; // 仅测试目标根目录，不改变产品安装器路径

/****************************** 链接包装 ******************************/

int __real_open(const char *path, int flags, ...);

/**
 * @brief 仅重定向安装器打开的根目录，其余源文件继续使用显式临时路径。
 */
int __wrap_open(const char *path, int flags, ...)
{
    va_list arguments;
    mode_t  mode;

    mode = 0;
    if ((flags & O_CREAT) != 0)
    {
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }

    return __real_open(strcmp(path, "/") == 0 ? g_test_root : path, flags, mode);
}

/**
 * @brief 对隔离目录执行真实安装逻辑，向Python返回实际错误码。
 */
int main(int argc, char **argv)
{
    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/')
    {
        return 2;
    }

    g_test_root = argv[2];
    printf("%d\n", upgrade_system_install(argv[1]));
    return 0;
}
