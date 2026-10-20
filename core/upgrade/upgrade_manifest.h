/**
 * @file upgrade_manifest.h
 * @brief LinkG升级文件清单校验内部接口
 */

#ifndef UPGRADE_MANIFEST_H
#define UPGRADE_MANIFEST_H

/****************************** 清单校验 ******************************/

int upgrade_manifest_verify(const char *upgrade_dir);

#endif // UPGRADE_MANIFEST_H
