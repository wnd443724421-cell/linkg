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

/****************************** 生命周期 ******************************/

int linkg_link_manager_init(void);
int linkg_link_manager_deinit(void);

/****************************** 链路注册 ******************************/

int linkg_link_manager_register(linkg_link_t *link);
int linkg_link_manager_unregister(linkg_link_t *link);

/****************************** 接收处理 ******************************/

int linkg_link_manager_register_receive_handler(linkg_link_receive_batch_func_t receive, void *user_data);
int linkg_link_manager_unregister_receive_handler(void);

/****************************** 发送清理 ******************************/

int linkg_link_manager_purge_tx_path(linkg_path_t *path, uint32_t *purged_count);

/****************************** 链路查询 ******************************/

linkg_link_t *linkg_link_manager_get(uint32_t link_id);
linkg_link_t *linkg_link_manager_get_by_access(linkg_link_access_t access);
uint32_t      linkg_link_manager_get_id(linkg_link_access_t access);
uint32_t      linkg_link_manager_count(void);

#ifdef __cplusplus
}
#endif

#endif
