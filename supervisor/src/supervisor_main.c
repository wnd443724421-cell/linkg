/**
 * @file supervisor_main.c
 * @brief LinkG独立进程守护入口
 * @author Dawn
 * @version 0.1.0
 * @date 2026-10-19
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "supervisor_config.h"
#include "supervisor_process.h"

/****************************** 主程序 ******************************/

/**
 * @brief 解析测试路径与互斥锁配置并启动守护循环。
 */
int main(int argc, char **argv)
{
    char       *default_arguments[] = {SUPERVISOR_DEFAULT_PROGRAM, NULL};
    const char *lock_path;
    int         index;

    lock_path = SUPERVISOR_DEFAULT_LOCK_FILE;
    index     = 1;

    if (index < argc && strcmp(argv[index], "--help") == 0)
    {
        printf("Usage: %s [--lock-file PATH] [--] [/absolute/program [args...]]\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (index < argc && strcmp(argv[index], "--lock-file") == 0)
    {
        if (index + 1 >= argc)
        {
            return EXIT_FAILURE;
        }

        lock_path = argv[index + 1];
        index    += 2;
    }

    if (index < argc && strcmp(argv[index], "--") == 0)
    {
        index++;
    }

    if (index < argc && argv[index][0] != '/')
    {
        fprintf(stderr, "Supervisor child path must be absolute\n");
        return EXIT_FAILURE;
    }

    return supervisor_process_run(index < argc ? &argv[index] : default_arguments, lock_path) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
