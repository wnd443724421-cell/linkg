/**
 * @file upgrade_manifest.h
 * @brief LinkG升级文件清单校验内部接口
 */

#ifndef UPGRADE_MANIFEST_H
#define UPGRADE_MANIFEST_H

#include <stdbool.h>

/****************************** 清单校验 ******************************/

int upgrade_manifest_verify(const char *upgrade_dir);

/****************************** 目录比较 ******************************/

int upgrade_manifest_group_equal(const char *upgrade_dir, const char *manifest_prefix, const char *target_dir, bool *same);

#endif // UPGRADE_MANIFEST_H
