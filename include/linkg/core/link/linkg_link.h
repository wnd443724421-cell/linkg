/**
 * @file linkg_link.h
 * @brief LinkG链路基类定义及操作接口
 */

#ifndef LINKG_LINK_H
#define LINKG_LINK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkg_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_LINK_ID_INVALID             0U  // 无效链路运行实例标识
#define LINKG_LINK_NAME_MAX               12U // 链路名称最大长度，包含结束符
#define LINKG_LINK_TX_BATCH_SIZE_DEFAULT  32U // 默认内部发送批次最大包数
#define LINKG_LINK_RX_BATCH_SIZE_DEFAULT  32U // 默认内部接收批次最大包数

/****************************** 前置声明 ******************************/

typedef struct linkg_link         linkg_link_t;
typedef struct linkg_link_ops     linkg_link_ops_t;
typedef struct linkg_link_runtime linkg_link_runtime_t;
typedef struct linkg_packet       linkg_packet_t;
typedef struct linkg_packet_pool  linkg_packet_pool_t;

/****************************** 接入类型 ******************************/

typedef enum
{
    LINKG_LINK_ACCESS_NONE     = 0, // 未指定接入类型
    LINKG_LINK_ACCESS_WIFI,         // Wi-Fi接入
    LINKG_LINK_ACCESS_CELLULAR      // 蜂窝网络接入
} linkg_link_access_t;

/****************************** 运行状态 ******************************/

typedef enum
{
    LINKG_LINK_STATE_STOPPED  = 0, // 链路已经停止
    LINKG_LINK_STATE_STARTING,     // 链路正在启动
    LINKG_LINK_STATE_RUNNING,      // 链路正在运行
    LINKG_LINK_STATE_STOPPING,     // 链路正在停止
    LINKG_LINK_STATE_FAILED        // 链路运行失败
} linkg_link_state_t;

/****************************** 发送类别 ******************************/

typedef enum
{
    LINKG_LINK_TX_CLASS_REALTIME = 0, // 实时低延时数据
    LINKG_LINK_TX_CLASS_VIDEO,        // 视频实时媒体数据
    LINKG_LINK_TX_CLASS_DATA,         // 普通数据
    LINKG_LINK_TX_CLASS_COUNT         // 发送类别数量
} linkg_link_tx_class_t;

/****************************** 接收数据 ******************************/

typedef struct
{
    linkg_packet_t       *packet; // 已接收数据包
    linkg_path_endpoint_t source; // 逻辑来源网络端点，具体链路可规范化
} linkg_link_rx_item_t;

typedef void (*linkg_link_receive_batch_func_t)(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data);

/****************************** 链路配置 ******************************/

typedef struct
{
    const char                     *name;              // 链路名称
    linkg_link_access_t             access;            // 链路接入类型
    uint32_t                        tx_batch_size;     // 内部发送批次最大包数
    uint32_t                        rx_batch_size;     // 内部接收批次最大包数
    linkg_packet_pool_t            *packet_pool;       // 链路接收使用的数据包池
    linkg_link_receive_batch_func_t receive;           // 批量接收处理函数
    void                           *receive_user_data; // 接收处理私有数据
} linkg_link_config_t;

/****************************** 具体链路操作 ******************************/

struct linkg_link_ops
{
    size_t instance_size; // 完整具体链路对象大小

    int  (*init)(linkg_link_t *link, const void *config); // 初始化具体链路私有状态
    void (*deinit)(linkg_link_t *link);                   // 反初始化具体链路私有状态

    int  (*open)(linkg_link_t *link);  // 打开具体链路运行资源
    int  (*close)(linkg_link_t *link); // 关闭具体链路运行资源

    int  (*get_rx_fd)(linkg_link_t *link); // 获取接收等待描述符

    int  (*send_batch)(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results);
    int  (*receive_batch)(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t capacity);
};

/****************************** 链路基类 ******************************/

struct linkg_link
{
    uint32_t                    id;                        // 链路管理模块分配的运行实例标识
    char                        name[LINKG_LINK_NAME_MAX]; // 链路名称
    linkg_link_access_t         access;                    // 链路接入类型
    const linkg_link_ops_t     *ops;                       // 具体链路操作接口
    linkg_packet_pool_t        *packet_pool;               // 链路接收使用的数据包池
    linkg_link_runtime_t       *runtime;                   // 链路基类私有运行资源
    _Atomic linkg_link_state_t  state;                     // 链路当前运行状态
};

/****************************** 生命周期 ******************************/

int  linkg_link_create(const linkg_link_config_t *config, const linkg_link_ops_t *ops, const void *private_config, linkg_link_t **out);
int  linkg_link_start(linkg_link_t *link);
int  linkg_link_stop(linkg_link_t *link);
void linkg_link_destroy(linkg_link_t *link);

/****************************** 数据发送 ******************************/

int linkg_link_submit(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *packet);
int linkg_link_submit_batch(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results);

/****************************** 属性查询 ******************************/

uint32_t            linkg_link_get_id(const linkg_link_t *link);
const char         *linkg_link_get_name(const linkg_link_t *link);
linkg_link_access_t linkg_link_get_access(const linkg_link_t *link);
linkg_link_state_t  linkg_link_get_state(const linkg_link_t *link);
bool                linkg_link_is_running(const linkg_link_t *link);

#ifdef __cplusplus
}
#endif

#endif
