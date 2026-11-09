/**
 * @file web_speedtest.h
 * @brief LinkG网络吞吐量诊断任务内部接口
 */

#ifndef WEB_SPEEDTEST_H
#define WEB_SPEEDTEST_H

#include <stdbool.h>
#include <stdint.h>

/****************************** 宏定义 ******************************/

#define LINKG_WEB_SPEEDTEST_SAMPLE_MAX 64U // 单次测试最多保留64个每秒样本

/****************************** 测试类型 ******************************/

typedef enum
{
    LINKG_WEB_SPEEDTEST_MODE_CLIENT = 0, // 连接远端iperf3服务端
    LINKG_WEB_SPEEDTEST_MODE_SERVER      // 在本机组网IPv4地址上监听
} linkg_web_speedtest_mode_t;

typedef enum
{
    LINKG_WEB_SPEEDTEST_TCP = 0, // TCP吞吐量测试
    LINKG_WEB_SPEEDTEST_UDP      // UDP指定目标速率测试
} linkg_web_speedtest_protocol_t;

typedef enum
{
    LINKG_WEB_SPEEDTEST_UPLOAD = 0, // 本机向服务器发送
    LINKG_WEB_SPEEDTEST_DOWNLOAD    // 服务器向本机发送
} linkg_web_speedtest_direction_t;

typedef enum
{
    LINKG_WEB_SPEEDTEST_IDLE = 0,   // 尚未启动
    LINKG_WEB_SPEEDTEST_STARTING,   // 正在准备子进程
    LINKG_WEB_SPEEDTEST_RUNNING,    // 客户端传输中或服务端已接入测试
    LINKG_WEB_SPEEDTEST_LISTENING,  // 服务端正在等待客户端
    LINKG_WEB_SPEEDTEST_COMPLETED,  // 客户端已获得完整接收端结果
    LINKG_WEB_SPEEDTEST_FAILED,     // 子进程或控制连接失败
    LINKG_WEB_SPEEDTEST_CANCELLED   // 用户或模块停止任务
} linkg_web_speedtest_state_t;

/****************************** 参数与状态 ******************************/

typedef struct
{
    char                            server[128];    // 客户端服务器IP或域名，服务端模式不使用
    uint16_t                        port;           // 客户端连接端口或服务端监听端口
    uint32_t                        duration_sec;   // 客户端测试持续秒数
    uint32_t                        packet_length;  // 客户端UDP应用层数据报长度
    double                          bandwidth_mbps; // 客户端UDP目标发送速率
    linkg_web_speedtest_mode_t      mode;           // 客户端或服务端模式
    linkg_web_speedtest_protocol_t  protocol;       // 客户端TCP或UDP
    linkg_web_speedtest_direction_t direction;      // 客户端上传或下载
    bool                            auto_port;      // 公共节点繁忙时递增5201至5220
} linkg_web_speedtest_params_t;

typedef struct
{
    double second; // 当前区间结束时间，秒
    double mbps;   // 本地区间发送或接收速率
} linkg_web_speedtest_sample_t;

typedef struct
{
    uint32_t                     job_id;                                  // 本进程内任务编号
    linkg_web_speedtest_state_t  state;                                   // 当前任务状态
    linkg_web_speedtest_params_t params;                                  // 当前任务参数，port可更新为重试端口
    linkg_web_speedtest_sample_t samples[LINKG_WEB_SPEEDTEST_SAMPLE_MAX]; // 最近一次测试的每秒样本
    uint32_t                     sample_count;                            // 有效样本数
    uint32_t                     interval_count;                          // 累计收到的区间统计次数
    uint32_t                     session_id;                              // 服务端当前/最近一次连接的序号
    double                       current_mbps;                            // 最新1秒速率
    double                       average_mbps;                            // 最终接收端平均速率或服务端本地汇总
    double                       packet_loss_percent;                     // UDP接收端丢包率
    uint64_t                     elapsed_ms;                              // 当前任务耗时
    bool                         average_valid;                           // 是否有有效汇总
    bool                         packet_loss_valid;                       // 是否有有效丢包统计
    bool                         cancel_requested;                        // 是否请求取消
    bool                         server_connected;                        // 服务端当前有测试连接
    bool                         rate_is_sender;                          // 实时速率是否为发送端统计
    bool                         average_is_sender;                       // 最终速率是否为发送端汇总
    char                         bind_address[48];                        // 服务端实际监听IP或客户端绑定源地址
    char                         message[192];                            // 错误信息或状态说明
} linkg_web_speedtest_status_t;

/****************************** 生命周期 ******************************/

int  _linkg_web_speedtest_init(void);
int  _linkg_web_speedtest_start(void);
int  _linkg_web_speedtest_stop(void);
void _linkg_web_speedtest_deinit(void);

/****************************** 任务控制 ******************************/

int  _linkg_web_speedtest_submit(const linkg_web_speedtest_params_t *params, uint32_t *job_id);
int  _linkg_web_speedtest_get(uint32_t job_id, linkg_web_speedtest_status_t *status);
int  _linkg_web_speedtest_cancel(uint32_t job_id);

#endif // WEB_SPEEDTEST_H
