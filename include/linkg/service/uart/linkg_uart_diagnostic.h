/**
 * @file linkg_uart_diagnostic.h
 * @brief LinkG串口硬件诊断公共接口
 */

#ifndef LINKG_UART_DIAGNOSTIC_H
#define LINKG_UART_DIAGNOSTIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 测试参数 ******************************/

#define LINKG_UART_DIAGNOSTIC_MAX_TESTS  4U // 单次诊断最多测试项目数量
#define LINKG_UART_DIAGNOSTIC_UART_COUNT 3U // 当前硬件实际提供的逻辑串口数量

typedef enum
{
    LINKG_UART_DIAGNOSTIC_MODE_SINGLE = 1, // 单串口诊断
    LINKG_UART_DIAGNOSTIC_MODE_ALL    = 2  // 全部串口诊断
} linkg_uart_diagnostic_mode_t;

typedef enum
{
    LINKG_UART_DIAGNOSTIC_STATE_IDLE = 0,  // 尚未执行
    LINKG_UART_DIAGNOSTIC_STATE_STARTING,  // 正在接管串口资源
    LINKG_UART_DIAGNOSTIC_STATE_RUNNING,   // 正在执行测试
    LINKG_UART_DIAGNOSTIC_STATE_RESTORING, // 正在恢复串口业务
    LINKG_UART_DIAGNOSTIC_STATE_COMPLETED, // 测试完成且资源恢复
    LINKG_UART_DIAGNOSTIC_STATE_CANCELLED, // 用户取消且资源恢复
    LINKG_UART_DIAGNOSTIC_STATE_FAILED     // 测试流程或资源恢复失败
} linkg_uart_diagnostic_state_t;

typedef enum
{
    LINKG_UART_DIAGNOSTIC_PATH_NORMAL = 0, // T113普通UART直通路径
    LINKG_UART_DIAGNOSTIC_PATH_SBUS        // T113经STM32的SBUS桥接路径
} linkg_uart_diagnostic_path_t;

typedef enum
{
    LINKG_UART_DIAGNOSTIC_TEST_NOT_RUN = 0, // 尚未执行
    LINKG_UART_DIAGNOSTIC_TEST_RUNNING,     // 正在执行
    LINKG_UART_DIAGNOSTIC_TEST_PASSED,      // 数据完整一致
    LINKG_UART_DIAGNOSTIC_TEST_FAILED,      // 测试失败
    LINKG_UART_DIAGNOSTIC_TEST_CANCELLED,   // 当前测试被取消
    LINKG_UART_DIAGNOSTIC_TEST_SKIPPED      // 因取消或初始化失败而跳过
} linkg_uart_diagnostic_test_state_t;

/****************************** 结果定义 ******************************/

typedef struct
{
    uint8_t                            uart_num;           // 逻辑串口编号
    linkg_uart_diagnostic_path_t       path;               // 测试路径
    linkg_uart_diagnostic_test_state_t state;              // 当前测试状态
    uint32_t                           tx_bytes;           // 已提交发送字节数
    uint32_t                           rx_bytes;           // 已接收字节数
    uint32_t                           error_count;        // 数据不一致字节数
    uint32_t                           first_error_offset; // 首个错误字节偏移，无错误时为UINT32_MAX
    uint32_t                           frames_total;       // SBUS测试总帧数
    uint32_t                           frames_ok;          // SBUS成功回环帧数
    int                                error;              // 测试系统调用错误，负errno
    uint64_t                           elapsed_ms;         // 当前测试耗时
} linkg_uart_diagnostic_test_result_t;

typedef struct
{
    linkg_uart_diagnostic_mode_t mode;     // 单串口或全部串口
    uint8_t                      uart_num; // 单串口模式选择的逻辑串口
} linkg_uart_diagnostic_params_t;

typedef struct
{
    uint32_t                            job_id;                                 // 本进程内诊断任务编号
    linkg_uart_diagnostic_mode_t        mode;                                   // 任务模式
    linkg_uart_diagnostic_state_t       state;                                  // 任务状态
    uint8_t                             selected_uart;                          // 单串口模式选择的串口
    uint32_t                            current_test;                           // 当前测试项目索引
    uint32_t                            test_count;                             // 本次任务测试项目数量
    bool                                cancel_requested;                       // 是否已经请求取消
    bool                                uart_was_initialized;                   // 诊断前UART Manager是否已初始化
    bool                                uart_was_running;                       // 诊断前UART Manager是否运行
    bool                                uart_restored;                          // 原UART生命周期是否已恢复
    bool                                tests_passed;                           // 所有计划测试是否全部通过
    int                                 error;                                  // 任务准备或执行错误
    int                                 restore_error;                          // UART或GPIO恢复错误
    uint64_t                            elapsed_ms;                             // 整个任务耗时
    linkg_uart_diagnostic_test_result_t tests[LINKG_UART_DIAGNOSTIC_MAX_TESTS]; // 各项测试结果
} linkg_uart_diagnostic_status_t;

/****************************** 生命周期 ******************************/

int linkg_uart_diagnostic_init(void);
int linkg_uart_diagnostic_deinit(void);

/****************************** 任务控制 ******************************/

int linkg_uart_diagnostic_start(const linkg_uart_diagnostic_params_t *params, uint32_t *job_id);
int linkg_uart_diagnostic_stop(uint32_t job_id);
int linkg_uart_diagnostic_get_status(uint32_t job_id, linkg_uart_diagnostic_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
