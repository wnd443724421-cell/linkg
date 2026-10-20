/**
 * @file upgrade_system.h
 * @brief LinkG可选系统文件安装接口
 */

#ifndef UPGRADE_SYSTEM_H
#define UPGRADE_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 安装接口 ******************************/

int upgrade_system_install(const char *upgrade_dir);

#ifdef __cplusplus
}
#endif

#endif // UPGRADE_SYSTEM_H
