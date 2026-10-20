/**
 * @file upgrade_install.h
 * @brief LinkG升级包App及Web Release安装接口
 */

#ifndef UPGRADE_INSTALL_H
#define UPGRADE_INSTALL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 安装接口 ******************************/

int upgrade_install_release(const char *upgrade_dir, char *installed_release, size_t release_size);

#ifdef __cplusplus
}
#endif

#endif // UPGRADE_INSTALL_H
