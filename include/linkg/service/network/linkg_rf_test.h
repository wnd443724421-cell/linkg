/**
 * @file linkg_rf_test.h
 * @brief LinkG蜂窝射频独占诊断接口
 */

#ifndef LINKG_RF_TEST_H
#define LINKG_RF_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************** 类型定义 ******************************/

typedef struct
{
    uint32_t rat;             // 1为LTE，2为NR5G
    uint32_t band;            // 频段编号
    uint32_t low_khz;         // 上行频段下限
    uint32_t high_khz;        // 上行频段上限
    uint32_t first_channel;   // 文档强发信道起点
    uint32_t last_channel;    // 文档强发信道终点
    uint32_t channel_step;    // 信道栅格编号步进
    uint32_t step_khz;        // 全局信道编号频率步进
    uint32_t bandwidth_mask;  // 候选FTM带宽编码位图
    uint32_t default_khz;     // 推荐中心频率
} linkg_rf_band_t;

typedef struct
{
    uint32_t rat;             // 网络制式
    uint32_t band;            // 频段编号
    uint32_t bandwidth_code;  // FTM带宽编码
    uint32_t frequency_khz;   // 请求中心频率
    uint32_t duration_sec;    // 强发最长时长
} linkg_rf_params_t;

typedef enum
{
    LINKG_RF_IDLE = 0,        // 尚未测试
    LINKG_RF_STARTING,        // 等待资源或初始化
    LINKG_RF_OPENING,         // 正在初始化独立AT通道
    LINKG_RF_CHECKING,        // 正在查询初始FTM状态
    LINKG_RF_ENTERING,        // 进入FTM
    LINKG_RF_STARTING_TX,     // 正在发送启用强发指令
    LINKG_RF_RUNNING,         // 强发命令已确认
    LINKG_RF_STOPPING_TX,     // 正在发送禁用强发指令
    LINKG_RF_EXITING,         // 正在退出FTM并确认Online
    LINKG_RF_RELEASING,       // 正在释放独立AT通道
    LINKG_RF_RESTORING,       // 按原配置恢复Cellular Owner
    LINKG_RF_COMPLETED,       // 控制流程完成
    LINKG_RF_CANCELLED,       // 已取消
    LINKG_RF_FAILED,          // 测试控制失败
    LINKG_RF_RECOVERY_FAILED  // 清理或蜂窝恢复失败
} linkg_rf_state_t;

typedef struct
{
    linkg_rf_params_t params;          // 本次请求参数
    linkg_rf_state_t  test_failed_at;  // 测试首次失败阶段
    linkg_rf_state_t  cleanup_failed_at; // 清理失败阶段
    linkg_rf_state_t  state;           // 任务状态
    uint32_t          job_id;          // 任务编号
    uint32_t          channel;         // 实际配置的信道号
    uint32_t          actual_khz;      // 实际配置的标称中心频率
    uint64_t          started_ms;      // 强发确认时间
    uint64_t          elapsed_ms;      // 已确认强发时长
    int               test_error;      // 测试控制错误
    int               cleanup_error;   // 关闭强发或退出FTM错误
    int               restore_error;   // 正常Owner恢复错误
    int               recovery_error;  // 独立恢复最后一次执行结果
    bool              recovery_active; // 独立恢复正在执行
    bool              recovery_blocked; // 未确认安全释放，禁止交给正常Owner
    bool              active;          // 尚未完整收尾
    bool              tx_confirmed;    // 强发指令曾收到OK
    bool              mode_verified;   // 已确认FTM退出
} linkg_rf_status_t;

/****************************** 参数及任务 ******************************/

const linkg_rf_band_t *linkg_rf_test_bands(size_t *count);
uint32_t               linkg_rf_test_bandwidth_khz(uint32_t code);
int                    linkg_rf_test_resolve(const linkg_rf_params_t *params, uint32_t *channel, uint32_t *actual_khz);
int                    linkg_rf_test_start(const linkg_rf_params_t *params, uint32_t *job_id);
int                    linkg_rf_test_cancel(uint32_t job_id);
void                   linkg_rf_test_get(linkg_rf_status_t *status);
bool                   linkg_rf_test_active(void);
int                    linkg_rf_test_recover_pending(void);

#endif
