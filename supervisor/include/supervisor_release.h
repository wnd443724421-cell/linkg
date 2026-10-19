/**
 * @file supervisor_release.h
 * @brief Release选择、确认事务和回滚协调接口
 */

#ifndef SUPERVISOR_RELEASE_H
#define SUPERVISOR_RELEASE_H

#include <limits.h>
#include <stdbool.h>

#include "supervisor_version.h"

/****************************** 类型定义 ******************************/

typedef struct
{
    const char *state_path; // 版本JSON绝对路径，借用入口参数
    const char *app_root;   // App根目录，借用入口参数
    const char *web_root;   // Web根目录，借用入口参数
} supervisor_release_paths_t;

typedef struct
{
    supervisor_release_paths_t paths;             // 持久化和安装路径配置
    supervisor_version_t       version;           // 最近读取或持久化完成的版本状态
    char                       program[PATH_MAX]; // 唯一current可执行文件路径
    bool                       trial;             // 稳定JSON持久化完成前始终视为候选
} supervisor_release_t;

/****************************** 生命周期 ******************************/

int supervisor_release_init(supervisor_release_t *release, const supervisor_release_paths_t *paths);
int supervisor_release_prepare(supervisor_release_t *release);
int supervisor_release_confirm(supervisor_release_t *release);
int supervisor_release_rollback(supervisor_release_t *release);

#endif
