
/**
 * @file linkg_config.h
 * @brief LinkG全局配置管理接口
 */

#ifndef LINKG_CONFIG_H
#define LINKG_CONFIG_H

#include "linkg_device_config.h"
#include "linkg_links_config.h"
#include "linkg_network_config.h"
#include "linkg_paths_config.h"
#include "linkg_uart_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置路径 ******************************/

#define LINKG_CONFIG_FACTORY_PATH "/app/current/config/linkgDefault.json" // 出厂默认配置文件
#define LINKG_CONFIG_USER_PATH    "/app/current/config/linkgUser.json"    // 用户运行配置文件

/****************************** 类型定义 ******************************/

typedef struct
{
    linkg_device_config_t  device;  // 设备配置
    linkg_network_config_t network; // 网络配置
    linkg_links_config_t   links;   // 接入模块配置
    linkg_paths_config_t   paths;   // 逻辑路径配置
    linkg_uart_config_t    uart;    // 串口及网络路由配置
} linkg_config_t;

/****************************** 生命周期 ******************************/

int  linkg_config_load(void);
void linkg_config_deinit(void);

/****************************** 配置获取 ******************************/

int linkg_config_create_snapshot(linkg_config_t *out);
int linkg_config_get_device(linkg_device_config_t *out);
int linkg_config_get_network(linkg_network_config_t *out);
int linkg_config_get_links(linkg_links_config_t *out);
int linkg_config_get_paths(linkg_paths_config_t *out);
int linkg_config_get_uart(linkg_uart_config_t *out);
int linkg_config_get_wifi(linkg_wifi_config_t *out);
int linkg_config_get_cellular(linkg_cellular_config_t *out);

/****************************** 配置更新 ******************************/

int linkg_config_replace(const linkg_config_t *config);

/****************************** 配置持久化 ******************************/

int linkg_config_save(const linkg_config_t *config);

/****************************** 配置恢复 ******************************/

int linkg_config_restore_factory(void);

#ifdef __cplusplus
}
#endif

#endif
