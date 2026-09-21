/**
 * @file linkg_link_manager.h
 * @brief LinkG本地链路管理接口
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-21
 */

#ifndef LINKG_LINK_MANAGER_H
#define LINKG_LINK_MANAGER_H

#include <stdint.h>

#include "linkg_link.h"
#include "linkg_path.h"


/****************************** 模块生命周期 ******************************/

/**
 * @brief 初始化链路管理模块。
 *
 * @note 仅初始化Link注册表和管理资源。
 *       不创建Wi-Fi、蜂窝等具体业务Link。
 *
 * @param config 链路管理配置。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_init(const linkg_link_manager_config_t *config);


/**
 * @brief 反初始化链路管理模块。
 *
 * @note 调用前所有业务Link必须已经注销。
 *       Link对象生命周期由对应业务模块负责。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_deinit(void);


/****************************** 链路注册 ******************************/

/**
 * @brief 注册业务链路。
 *
 * @note Wi-Fi、蜂窝等具体链路模块创建Link对象后调用本接口。
 *       Link Manager负责分配运行实例ID并保存引用。
 *
 * @param link 已创建的业务Link对象。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_register(linkg_link_t *link);


/**
 * @brief 注销业务链路。
 *
 * @note 调用前业务模块必须保证Link已经停止运行。
 *       本接口不会释放Link对象。
 *
 * @param link 待注销业务Link对象。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_unregister(linkg_link_t *link);


/****************************** 接收分发 ******************************/

/**
 * @brief 注册统一链路接收处理函数。
 *
 * @note 所有业务Link收到的数据通过该出口交给上层处理。
 *       通常由Transport层注册。
 *
 * @param receive 接收处理函数。
 * @param user_data 用户私有数据。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_register_receive_handler(linkg_link_receive_batch_func_t receive,
                                                void *user_data);


/**
 * @brief 注销统一链路接收处理函数。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_unregister_receive_handler(void);


/****************************** 发送维护 ******************************/

/**
 * @brief 清理指定Path对应的待发送Packet引用。
 *
 * @note 用于Path注销场景。
 *       Link Manager根据Path所属Link定位目标链路，
 *       并要求对应Link清理缓存数据。
 *
 * @param path 失效Path。
 * @param purged_count 清理数量输出。
 *
 * @return 0 成功。
 *         其他值表示失败。
 */
int linkg_link_manager_purge_tx_path(linkg_path_t *path,
                                     uint32_t *purged_count);


/****************************** 链路查询 ******************************/

/**
 * @brief 根据Link ID获取业务链路对象。
 *
 * @param link_id Link运行实例ID。
 *
 * @return Link对象。
 *         不存在返回NULL。
 */
linkg_link_t *linkg_link_manager_get(uint32_t link_id);


/**
 * @brief 根据接入类型获取Link ID。
 *
 * @param access Link接入类型。
 *
 * @return Link ID。
 *         不存在返回LINKG_LINK_ID_INVALID。
 */
uint32_t linkg_link_manager_get_id(linkg_link_access_t access);


/**
 * @brief 获取当前注册业务Link数量。
 *
 * @return Link数量。
 */
uint32_t linkg_link_manager_count(void);


#endif /* LINKG_LINK_MANAGER_H */
