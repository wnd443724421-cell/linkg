/**
 * @file linkg_link_manager.h
 * @brief LinkG本地链路管理接口
 */

#ifndef LINKG_LINK_MANAGER_H
#define LINKG_LINK_MANAGER_H

#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 初始化配置 ******************************/

typedef struct
{
    linkg_packet_pool_t *packet_pool; // 所有本地链路共用的数据包池
} linkg_link_manager_config_t;

/****************************** 生命周期 ******************************/

int linkg_link_manager_init(const linkg_link_manager_config_t *config);
int linkg_link_manager_start(void);
int linkg_link_manager_stop(void);
int linkg_link_manager_deinit(void);

/****************************** 接收处理 ******************************/

int linkg_link_manager_register_receive_handler(linkg_link_receive_batch_func_t receive, void *user_data);
int linkg_link_manager_unregister_receive_handler(void);

/****************************** 发送清理 ******************************/

int linkg_link_manager_purge_tx_path(linkg_path_t *path, uint32_t *purged_count);

/****************************** 链路查询 ******************************/

linkg_link_t *linkg_link_manager_get(uint32_t link_id);
uint32_t      linkg_link_manager_get_id(linkg_link_access_t access);
uint32_t      linkg_link_manager_count(void);

#ifdef __cplusplus
}
#endif

#endif
