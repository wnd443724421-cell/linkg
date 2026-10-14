/**
 * @file uart_internal.h
 * @brief LinkG串口服务内部结构与分模块接口
 */

#ifndef UART_INTERNAL_H
#define UART_INTERNAL_H

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#include "linkg_thread.h"
#include "linkg_uart_config.h"
#include "linkg_uart_manager.h"

/****************************** 模块常量 ******************************/

#define LINKG_UART_IO_SIZE               1400U // 单轮串口或网络读入长度
#define LINKG_UART_RAW_TX_CAPACITY       4096U // 每串口普通数据发送队列
#define LINKG_UART_TCP_TX_CAPACITY       4096U // 每路TCP网络发送队列
#define LINKG_UART_SBUS_FRAME_SIZE       25U   // SBUS固定帧长度
#define LINKG_UART_SBUS_FIFO_CAPACITY    8U    // SBUS最大FIFO容量
#define LINKG_UART_SBUS_INTERVAL_US      7600U // SBUS基础周期
#define LINKG_UART_SBUS_MIN_US           7000U // SBUS最短周期
#define LINKG_UART_SBUS_MAX_US           8200U // SBUS最长周期
#define LINKG_UART_SBUS_ADJUST_US        1U    // 单次周期调整
#define LINKG_UART_SBUS_WIFI_LEVEL       2U    // Wi-Fi模式目标水位
#define LINKG_UART_SBUS_BACKUP_LEVEL     5U    // 冗余/5G目标水位
#define LINKG_UART_RETRY_INITIAL_MS      500U  // 故障首次恢复间隔
#define LINKG_UART_RETRY_MAX_MS          8000U // 最大恢复间隔
#define LINKG_UART_POLL_TIMEOUT_MS       100U  // Worker最大poll周期
#define LINKG_UART_DEBUG_PERIOD_MS       5000U // 调试统计周期
#define LINKG_UART_DEBUG_PREVIEW_BYTES   16U   // 每次调试输出的字节数
#define LINKG_UART_DEBUG_DEFAULT         0     // 出厂关闭调试
#define LINKG_UART_MAX_FDS (1U + LINKG_UART_CONFIG_PORT_COUNT_MAX + 2U * LINKG_UART_CONFIG_PORT_COUNT_MAX * LINKG_UART_CONFIG_ROUTE_COUNT_MAX)

/****************************** 运行状态 ******************************/

typedef enum
{
    LINKG_UART_LIFE_UNINITIALIZED = 0, // 未初始化
    LINKG_UART_LIFE_STOPPED,           // 初始化但未运行
    LINKG_UART_LIFE_RUNNING,           // 运行中
    LINKG_UART_LIFE_STOPPING           // 停止中
} linkg_uart_life_t;

typedef enum
{
    LINKG_UART_ROUTE_IDLE = 0,    // 等待建链
    LINKG_UART_ROUTE_CONNECTING,  // TCP异步连接
    LINKG_UART_ROUTE_LISTENING,   // TCP等待客户端
    LINKG_UART_ROUTE_READY        // UDP或TCP可用
} linkg_uart_route_state_t;

/****************************** 缓存和统计 ******************************/

typedef struct
{
    uint8_t  bytes[LINKG_UART_RAW_TX_CAPACITY]; // 字节队列
    uint16_t head;                              // 读取偏移
    uint16_t count;                             // 已占用字节
} linkg_uart_raw_queue_t;

typedef struct
{
    uint8_t frames[LINKG_UART_SBUS_FIFO_CAPACITY][LINKG_UART_SBUS_FRAME_SIZE]; // 真实SBUS帧FIFO
    uint8_t head;                                                         	   // 读取位置
    uint8_t count;                                                       	   // 当前帧数
} linkg_uart_sbus_fifo_t;

typedef struct
{
    atomic_uint_fast64_t uart_rx_bytes;  // 物理串口收到的字节
    atomic_uint_fast64_t uart_tx_bytes;  // 已提交物理串口的字节
    atomic_uint_fast64_t net_rx_bytes;   // 网络收到的字节
    atomic_uint_fast64_t net_tx_bytes;   // 已提交网络的字节
    atomic_uint_fast64_t sbus_rx_frames; // 串口解析出的真实帧
    atomic_uint_fast64_t sbus_tx_frames; // 向串口完整提交的帧
    atomic_uint_fast64_t drops;          // 丢弃帧或数据块次数
    atomic_uint_fast64_t errors;         // IO错误次数
    atomic_uint_fast64_t fifo_empty;     // SBUS FIFO欠载次数
    atomic_uint_fast64_t fifo_samples;   // SBUS水位采样数
    atomic_uint_fast64_t fifo_sum;       // SBUS水位采样总和
    atomic_uint          fifo_min;       // 本周期最小水位
    atomic_uint          fifo_max;       // 本周期最大水位
} linkg_uart_stats_t;

/****************************** 网络路由 ******************************/

struct linkg_uart_port;

typedef struct
{
    struct linkg_uart_port          *parent;         						 // 归属串口
    const linkg_uart_route_config_t *config;         						 // 指向Manager私有配置
    linkg_uart_route_state_t     	 state;          						 // 网络路由状态
    int                          	 fd;             						 // UDP/连接TCP/监听TCP
    int                          	 client_fd;      						 // TCP服务端唯一客户端
    struct sockaddr_in           	 remote;         						 // 远端地址
    struct sockaddr_in           	 peer;           						 // UDP本地路由最近对端
    bool                         	 peer_valid;     						 // UDP已学习对端
    uint64_t                     	 retry_at_ms;    						 // 下次重建期限
    uint32_t                     	 retry_delay_ms; 						 // 退避间隔
    uint8_t                      	 tcp_tx[LINKG_UART_TCP_TX_CAPACITY]; 	 // TCP待发字节
    uint16_t                     	 tcp_tx_count;     						 // TCP待发长度
    uint8_t                      	 sbus_stream[LINKG_UART_SBUS_FRAME_SIZE];// TCP入站SBUS组帧缓存
    uint8_t                      	 sbus_stream_count;						 // TCP组帧缓存字节数
} linkg_uart_route_t;

/****************************** 物理串口 ******************************/

typedef struct linkg_uart_port
{
    const linkg_uart_item_config_t  *config;                                    // Manager私有配置引用
    const char                      *device_path;                               // 固定设备节点
    atomic_int                       fd;                                        // 物理UART描述符
    atomic_uint_fast64_t             retry_at_ms;                               // 物理串口重开期限
    atomic_uint                      retry_delay_ms;                            // 物理串口重开退避
    atomic_uint                      generation;                                // 设备重开版本号
    pthread_mutex_t                  io_lock;                                   // Worker与SBUS线程共享串口锁
    pthread_mutex_t                  sbus_lock;                                 // SBUS FIFO锁
    linkg_thread_t                   sbus_thread;                               // 定时输出线程对象
    atomic_uint                      sbus_target_level;                         // 当前模式目标FIFO水位
    atomic_uint                      sbus_interval_us;                          // SBUS运行输出周期
    linkg_uart_sbus_fifo_t           sbus_fifo;                                 // 网络入站真实帧FIFO
    uint8_t                          sbus_rx[LINKG_UART_SBUS_FRAME_SIZE];       // 串口SBUS帧解析缓存
    uint8_t                          sbus_rx_count;                       		// 已积累字节
    linkg_uart_raw_queue_t           raw_tx;                              		// 普通串口待发字节
    linkg_uart_route_t               routes[LINKG_UART_CONFIG_ROUTE_COUNT_MAX]; // 路由运行态
    uint32_t                         route_count;                     		    // 路由数量
    linkg_uart_stats_t               stats;                           		    // 运行计数器
    uint64_t                         last_debug_ms;                   		    // 最近一次调试统计时间
    uint64_t                         last_uart_rx_bytes;              		    // 最近一次串口输入快照
    uint64_t                         last_uart_tx_bytes;              		    // 最近一次串口输出快照
    uint64_t                         last_net_rx_bytes;               		    // 最近一次网络输入快照
    uint64_t                         last_net_tx_bytes;               		    // 最近一次网络输出快照
    uint64_t                         last_drops;                      		    // 最近一次丢弃快照
    uint64_t                         last_errors;                     		    // 最近一次错误快照
    uint64_t                         last_fifo_empty;                 		    // 最近一次欠载快照
    uint64_t                         last_sbus_rx_frames;             		    // 最近一次串口SBUS组帧数
    uint64_t                         last_sbus_tx_frames;             		    // 最近一次串口SBUS输出数
    atomic_uint                      debug_preview_budget;            		    // 本统计周期最大数据预览数
} linkg_uart_port_t;

/****************************** poll调度 ******************************/

typedef enum
{
    LINKG_UART_EVENT_WAKEUP = 0, // Worker eventfd
    LINKG_UART_EVENT_PORT,       // 物理串口fd
    LINKG_UART_EVENT_ROUTE,      // 路由主fd
    LINKG_UART_EVENT_CLIENT      // TCP服务端客户端fd
} linkg_uart_event_kind_t;

typedef struct
{
    linkg_uart_event_kind_t kind;  // 事件类型
    void                   *owner; // Port或Route指针
} linkg_uart_event_t;

typedef struct
{
    linkg_uart_config_t config;                         // 独立配置副本
    linkg_uart_port_t   ports[LINKG_UART_CONFIG_PORT_COUNT_MAX]; // 有效串口上下文
    uint32_t            port_count;                     // 当前有效串口数
    linkg_thread_t      worker;                         // 单Worker
    pthread_mutex_t     life_lock;                      // 生命周期锁
    linkg_uart_life_t   state;                          // 生命周期状态
    atomic_bool         debug_enabled;                  // 运行Debug开关
    atomic_uint         sbus_target_level;              // 所有SBUS端口目标水位
} linkg_uart_manager_context_t;

extern linkg_uart_manager_context_t g_uart_manager;

/****************************** 内部工具 ******************************/

uint64_t uart_now_ms(void);
void     uart_stats_reset(linkg_uart_port_t *port);
void     uart_stats_report(linkg_uart_manager_context_t *manager);
void     uart_stats_fifo_sample(linkg_uart_port_t *port, uint32_t depth);
void     uart_debug_data(linkg_uart_port_t *port, const char *direction, const uint8_t *data, uint32_t length);

/****************************** Port操作 ******************************/

const char *uart_port_device_path(uint8_t uart_num);
int         uart_port_open(linkg_uart_port_t *port);
void        uart_port_close(linkg_uart_port_t *port);
void        uart_port_maintenance(linkg_uart_port_t *port, uint64_t now_ms);
int         uart_port_read(linkg_uart_port_t *port, uint8_t *buffer, uint32_t capacity);
int         uart_port_write(linkg_uart_port_t *port, const uint8_t *buffer, uint32_t length);
void        uart_port_fault(linkg_uart_port_t *port, int error);
int         uart_port_raw_enqueue(linkg_uart_port_t *port, const uint8_t *buffer, uint32_t length);
void        uart_port_raw_flush(linkg_uart_port_t *port);

/****************************** Route操作 ******************************/

void uart_route_init(linkg_uart_route_t *route, linkg_uart_port_t *port, const linkg_uart_route_config_t *config);
void uart_route_close(linkg_uart_route_t *route);
void uart_route_maintenance(linkg_uart_route_t *route, uint64_t now_ms);
void uart_route_event(linkg_uart_route_t *route, bool client, short revents);
void uart_route_send(linkg_uart_route_t *route, const uint8_t *data, uint32_t length);
void uart_route_forward_serial(linkg_uart_port_t *port, const uint8_t *data, uint32_t length);
short uart_route_poll_events(const linkg_uart_route_t *route, bool client);

/****************************** SBUS操作 ******************************/

void uart_sbus_input_serial(linkg_uart_port_t *port, const uint8_t *data, uint32_t length);
void uart_sbus_input_network(linkg_uart_port_t *port, const uint8_t *data, uint32_t length, bool stream, linkg_uart_route_t *route);
void uart_sbus_thread(linkg_thread_t *thread, void *user_data);
void uart_sbus_reset(linkg_uart_port_t *port);

/****************************** Worker操作 ******************************/

void uart_worker_thread(linkg_thread_t *thread, void *user_data);

#endif
