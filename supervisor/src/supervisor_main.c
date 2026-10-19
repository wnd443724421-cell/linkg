/**
 * @file supervisor_main.c
 * @brief LinkG独立进程守护和Release管理入口
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "supervisor_config.h"
#include "supervisor_process.h"

/****************************** 主程序 ******************************/

/**
 * @brief 解析状态和安装路径，默认启用Release管理，显式程序保留V0.1入口。
 */
int main(int argc, char **argv)
{
    supervisor_release_paths_t paths;
    const char                *lock_path;
    const char               **destination;
    int                        index;

    lock_path = SUPERVISOR_DEFAULT_LOCK_FILE;
    index     = 1;
    paths.state_path = SUPERVISOR_DEFAULT_STATE_FILE;
    paths.app_root   = SUPERVISOR_DEFAULT_APP_ROOT;
    paths.web_root   = SUPERVISOR_DEFAULT_WEB_ROOT;

    if (index < argc && strcmp(argv[index], "--help") == 0)
    {
        printf("Usage: %s [--lock-file PATH] [--state-file PATH] [--app-root PATH] [--web-root PATH]\n"
               "       %s [--lock-file PATH] -- /absolute/program [args...]  (V0.1 process-only)\n", argv[0], argv[0]);
        return EXIT_SUCCESS;
    }

    while (index < argc && strncmp(argv[index], "--", 2U) == 0 && strcmp(argv[index], "--") != 0)
    {
        destination = NULL;
        if (strcmp(argv[index], "--lock-file") == 0)
        {
            destination = &lock_path;
        }
        else if (strcmp(argv[index], "--state-file") == 0)
        {
            destination = &paths.state_path;
        }
        else if (strcmp(argv[index], "--app-root") == 0)
        {
            destination = &paths.app_root;
        }
        else if (strcmp(argv[index], "--web-root") == 0)
        {
            destination = &paths.web_root;
        }

        if (destination == NULL || index + 1 >= argc || argv[index + 1][0] != '/')
        {
            fprintf(stderr, "Supervisor invalid path option\n");
            return EXIT_FAILURE;
        }

        *destination = argv[index + 1];
        index       += 2;
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

    if (index < argc)
    {
        return supervisor_process_run(&argv[index], lock_path) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    return supervisor_process_manage(&paths, lock_path) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
