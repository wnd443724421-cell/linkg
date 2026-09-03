/**
 * @file cellular_runtime.h
 * @brief LinkG蜂窝网络内部运行状态协调接口
 */

#ifndef CELLULAR_RUNTIME_H
#define CELLULAR_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 运行状态 ******************************/

typedef enum
{
    CELLULAR_RUNTIME_STATE_NONE = 0,              // 无有效运行状态，仅用于未记录字段
    CELLULAR_RUNTIME_STATE_IDLE,                  // 蜂窝Owner尚未进入连接流程
    CELLULAR_RUNTIME_STATE_WAIT_SIM,              // 等待SIM卡插入并可被模组识别
    CELLULAR_RUNTIME_STATE_CHECK_SIM,             // 确认SIM逻辑状态和PIN要求
    CELLULAR_RUNTIME_STATE_ENTER_PIN,             // 使用用户配置PIN执行一次解锁
    CELLULAR_RUNTIME_STATE_WAIT_PIN,              // 缺少有效PIN或PIN尝试失败，等待外部条件变化
    CELLULAR_RUNTIME_STATE_WAIT_PUK,              // SIM需要PUK，禁止自动继续尝试
    CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION,     // 等待移动网络注册成功
    CELLULAR_RUNTIME_STATE_PREPARE_PDP,           // 确认并准备默认PDP上下文
    CELLULAR_RUNTIME_STATE_ACTIVATE_PDP,          // 请求激活默认PDP上下文
    CELLULAR_RUNTIME_STATE_WAIT_PDP,              // 等待并确认默认PDP上下文已经激活
    CELLULAR_RUNTIME_STATE_START_NETDEV,          // 请求启动RG255 USB网络设备连接
    CELLULAR_RUNTIME_STATE_WAIT_NETDEV,           // 等待并确认USB网络设备已经连接
    CELLULAR_RUNTIME_STATE_PREPARE_HOST,          // 准备Linux Host蜂窝接口运行环境
    CELLULAR_RUNTIME_STATE_WAIT_HOST,             // 等待Linux Host网络配置收敛
    CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY,   // 验证IPv4和IPv6实际互联网连通性
    CELLULAR_RUNTIME_STATE_ONLINE,                // 蜂窝数据链已经验证可用并进入维护阶段
    CELLULAR_RUNTIME_STATE_RETRY_WAIT             // 当前连接流程失败后的统一退避等待
} cellular_runtime_state_t;

/****************************** 状态执行结果 ******************************/

typedef enum
{
    CELLULAR_RUNTIME_STEP_WAIT = 0, // 当前状态正常等待，暂时保持当前状态
    CELLULAR_RUNTIME_STEP_DONE,     // 当前状态处理成功，可以推进后续状态
    CELLULAR_RUNTIME_STEP_FAILED,   // 当前状态失败，由Owner执行对应失败策略
    CELLULAR_RUNTIME_STEP_FATAL     // 发生不可恢复运行错误，Owner应退出run
} cellular_runtime_step_result_t;

/****************************** 运行上下文 ******************************/

/**
 * @brief 蜂窝连接状态机内部运行状态。
 *
 * @note 仅允许network-cell Owner线程通过cellular_fsm修改；
 *       Monitor、Status及platform层均不得直接修改该结构体，
 *       cellular_runtime本身不得执行任何硬件副作用。
 */
typedef struct
{
    cellular_runtime_state_t state;               // 当前连接状态
    cellular_runtime_state_t previous_state;      // 最近一次不同的连接状态
    cellular_runtime_state_t failed_state;        // 最近一次失败发生的连接状态
    cellular_runtime_state_t retry_target_state;  // RETRY_WAIT结束后的目标状态

    uint64_t                 generation;          // 运行状态变化代数
    uint64_t                 session_generation;  // 已确认新SIM会话代数

    uint64_t                 updated_ms;          // 最近一次逻辑运行状态变化时间
    uint64_t                 state_entered_ms;    // 当前状态最近一次进入时间
    uint64_t                 state_deadline_ms;   // 当前状态整体绝对超时时间，0表示无超时
    uint64_t                 next_action_ms;      // 当前状态下一次主动处理时间，0表示未安排
    uint64_t                 failure_ms;          // 最近一次状态失败时间

    uint32_t                 state_attempt_count; // 当前状态已经执行的主动尝试次数
    uint32_t                 retry_count;         // 当前SIM会话累计完整连接重试次数

    int                      last_error;          // 最近一次状态失败错误码
    uint8_t                  selected_pdp_cid;    // 当前SIM会话Owner选择的数据PDP上下文ID

    bool                     session_active;      // 当前是否处于一个已确认SIM插卡会话
    bool                     failure_valid;       // 是否存在最近一次有效失败记录
    bool                     pin_attempted;       // 当前SIM会话是否已经自动尝试过PIN
    bool                     pdp_cid_valid;       // 当前SIM会话是否已经选择数据PDP上下文
} cellular_runtime_t;


/****************************** 生命周期 ******************************/

int  cellular_runtime_init(cellular_runtime_t *runtime, uint64_t now_ms);
void cellular_runtime_reset(cellular_runtime_t *runtime, uint64_t now_ms);
int  cellular_runtime_begin_session(cellular_runtime_t *runtime, uint64_t now_ms);
void cellular_runtime_end_session(cellular_runtime_t *runtime, uint64_t now_ms);

/****************************** 状态控制 ******************************/

int  cellular_runtime_enter(cellular_runtime_t *runtime, cellular_runtime_state_t state, uint64_t now_ms, uint64_t timeout_ms);
int  cellular_runtime_schedule_retry(cellular_runtime_t *runtime, cellular_runtime_state_t target_state, uint64_t now_ms, uint64_t retry_delay_ms);
int  cellular_runtime_schedule_action(cellular_runtime_t *runtime, uint64_t now_ms, uint64_t delay_ms);
void cellular_runtime_clear_action(cellular_runtime_t *runtime, uint64_t now_ms);
int  cellular_runtime_note_attempt(cellular_runtime_t *runtime, uint64_t now_ms);

/****************************** 失败状态 ******************************/

int  cellular_runtime_record_failure(cellular_runtime_t *runtime, int error, uint64_t now_ms);
void cellular_runtime_clear_failure(cellular_runtime_t *runtime, uint64_t now_ms);

/****************************** PIN控制 ******************************/

int cellular_runtime_mark_pin_attempted(cellular_runtime_t *runtime, uint64_t now_ms);

/****************************** PDP上下文 ******************************/

int  cellular_runtime_set_pdp_cid(cellular_runtime_t *runtime, uint8_t cid, uint64_t now_ms);
void cellular_runtime_clear_pdp_cid(cellular_runtime_t *runtime, uint64_t now_ms);
bool cellular_runtime_get_pdp_cid(const cellular_runtime_t *runtime, uint8_t *cid);

/****************************** 状态查询 ******************************/

const char *cellular_runtime_state_name(cellular_runtime_state_t state);
uint64_t    cellular_runtime_get_deadline(const cellular_runtime_t *runtime);
bool        cellular_runtime_state_timed_out(const cellular_runtime_t *runtime, uint64_t now_ms);
bool        cellular_runtime_action_due(const cellular_runtime_t *runtime, uint64_t now_ms);
bool        cellular_runtime_retry_due(const cellular_runtime_t *runtime, uint64_t now_ms);
bool        cellular_runtime_online(const cellular_runtime_t *runtime);
bool        cellular_runtime_session_active(const cellular_runtime_t *runtime);
bool        cellular_runtime_has_failure(const cellular_runtime_t *runtime);
bool        cellular_runtime_pin_attempted(const cellular_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
