/**
 * @file web_speedtest.h
 * @brief LinkG 5G网络测速任务内部接口
 */

#ifndef LINKG_WEB_SPEEDTEST_H
#define LINKG_WEB_SPEEDTEST_H

#include <stdbool.h>
#include <stdint.h>

/****************************** 测试类型 ******************************/

typedef enum
{
    LINKG_WEB_SPEEDTEST_TCP = 0,  // TCP可靠传输测速
    LINKG_WEB_SPEEDTEST_UDP       // UDP指定目标速率测速
} linkg_web_speedtest_protocol_t;

typedef enum
{
    LINKG_WEB_SPEEDTEST_UPLOAD = 0, // 设备向服务器上行
    LINKG_WEB_SPEEDTEST_DOWNLOAD    // 服务器向设备下行
} linkg_web_speedtest_direction_t;

typedef enum
{
    LINKG_WEB_SPEEDTEST_IDLE = 0,  // 尚无测速任务
    LINKG_WEB_SPEEDTEST_STARTING,  // 等待Worker启动测试
    LINKG_WEB_SPEEDTEST_RUNNING,   // iperf3测试正在运行
    LINKG_WEB_SPEEDTEST_COMPLETED, // 已产生有效接收端汇总
    LINKG_WEB_SPEEDTEST_FAILED,    // 测试执行失败
    LINKG_WEB_SPEEDTEST_CANCELLED  // 用户或模块停止取消任务
} linkg_web_speedtest_state_t;

/****************************** 参数与状态 ******************************/

typedef struct
{
    char                            server[128];     // 测速服务器IP地址或域名
    uint16_t                        port;            // iperf3服务端口
    uint32_t                        duration_sec;    // 测试持续时间，单位秒
    uint32_t                        packet_length;   // UDP应用层数据长度，单位字节
    double                          bandwidth_mbps;  // UDP目标发送速率，单位Mbps
    linkg_web_speedtest_protocol_t  protocol;        // TCP或UDP测速协议
    linkg_web_speedtest_direction_t direction;       // 上行或下行测速方向
} linkg_web_speedtest_params_t;

typedef struct
{
    uint32_t                        job_id;              // 本进程测速任务编号
    linkg_web_speedtest_state_t      state;               // 当前任务状态
    linkg_web_speedtest_params_t     params;              // 本次测速参数快照
    double                          current_mbps;        // 最近区间速率，单位Mbps
    double                          average_mbps;        // 最终接收端平均速率，单位Mbps
    double                          packet_loss_percent; // 最终UDP接收端丢包率
    uint32_t                        interval_count;      // 已读取的区间样本数量
    uint64_t                        elapsed_ms;          // 任务运行耗时，单位毫秒
    bool                            average_valid;       // 最终接收端汇总是否有效
    bool                            packet_loss_valid;   // UDP丢包率是否有效
    bool                            cancel_requested;    // 是否请求取消任务
    char                            message[192];        // 最近错误或结果说明
} linkg_web_speedtest_status_t;

/****************************** 生命周期 ******************************/

int  _linkg_web_speedtest_init(void);
int  _linkg_web_speedtest_start(void);
int  _linkg_web_speedtest_stop(void);
void _linkg_web_speedtest_deinit(void);

/****************************** 任务控制 ******************************/

int _linkg_web_speedtest_submit(const linkg_web_speedtest_params_t *params, uint32_t *job_id);
int _linkg_web_speedtest_get(uint32_t job_id, linkg_web_speedtest_status_t *status);
int _linkg_web_speedtest_cancel(uint32_t job_id);

#endif // LINKG_WEB_SPEEDTEST_H
