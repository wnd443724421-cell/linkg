/**
 * @file switch_policy_verify.h
 * @brief LinkG冗余阶段Wi-Fi恢复积分及双链路质量验证接口
 */

#ifndef SWITCH_POLICY_VERIFY_H
#define SWITCH_POLICY_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_observation.h"
#include "switch_policy_event.h"
#include "switch_policy_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 验证参数 ******************************/

#define LINKG_SWITCH_POLICY_VERIFY_DURATION_US            3000000ULL // 冗余ACK后固定验证3秒
#define LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US           250000U    // Cellular RTT参考质量阈值
#define LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US       1500000ULL // 截止时成功Probe最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US 1000000ULL // 截止时观测快照最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES       20U        // 单方向单轮积分最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES    100U       // 双链路分类质量比较最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE            5U         // LOSS验证初始Wi-Fi无线异常积分
#define LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE          3U         // 积分严格小于3允许进入Wi-Fi恢复判决

/****************************** 流量方向 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_UPLINK = 0,     // STA到AP
    LINKG_SWITCH_POLICY_VERIFY_DOWNLINK,       // AP到STA
    LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT // 流量方向数量
} linkg_switch_policy_verify_direction_t;

/****************************** Wi-Fi恢复积分窗口 ******************************/

/**
 * @brief Wi-Fi整链路Wire单方向评分窗口。
 *
 * 首个有效窗口仅建立时间基线。负载压力优先时仍消费Wire源时间，
 * 但丢弃未完成的评分批次，不将拥塞期Wire混入下一次无线质量评分。
 */
typedef struct
{
    uint64_t last_updated_us; // 本方向最后消费的全局Wire源时间
    uint64_t lost_packets;    // 待评分批次累计丢包数量
    uint64_t sample_packets;  // 待评分批次累计样本数量
    bool     primed;          // ACK后已建立首个有效Wire窗口基线
} linkg_switch_policy_verify_score_window_t;

/****************************** HCC负载采样窗口 ******************************/

/**
 * @brief Wi-Fi本机或远端的设备级HCC负载证据。
 *
 * off_events及tx_blocked_seen记录整个验证期；*_this_round仅记录本轮
 * 新样本，用于负载优先积分，不能将历史压力重复扣分。
 */
typedef struct
{
    uint64_t last_updated_us;      // 本方向最后消费的HCC源时间
    uint64_t off_events;           // 验证期累计新增FLOWCTRL_OFF次数
    uint32_t last_off_count;       // 上一有效HCC源累计OFF计数
    uint32_t max_vo_queue_length;  // 验证期VO队列最大水位
    uint32_t max_vi_queue_length;  // 验证期VI队列最大水位
    uint32_t max_be_queue_length;  // 验证期BE队列最大水位
    uint32_t off_delta_this_round; // 本轮新增FLOWCTRL_OFF次数
    bool     initialized;          // 已取得初始OFF计数基线
    bool     valid_this_round;     // 本轮消费了新的有效HCC样本
    bool     tx_blocked_seen;      // 验证期曾观测到TX不允许发送
    bool     tx_blocked_this_round;// 本轮新HCC样本显示TX不允许发送
} linkg_switch_policy_verify_hcc_window_t;

/****************************** 全局Load采样窗口 ******************************/

/**
 * @brief Wi-Fi整链路单方向Load窗口，仅用于全局负载优先判决。
 *
 * 分类Load独立保存在Path中；全局OVF不分配给某个业务类别。
 */
typedef struct
{
    uint64_t last_updated_us; // 最后消费的全局Load源时间
    bool     primed;          // ACK后已建立首个有效Load窗口基线
    bool     observed;        // ACK后已实际消费至少一个可判断的Load窗口
    bool     valid_this_round;// 本轮有新的有效Load窗口
    bool     overflow_this_round; // 本轮全局Load确认TX Queue Overflow
    bool     overflow_seen;       // 整个验证期曾确认全局OVF
} linkg_switch_policy_verify_load_window_t;

/****************************** 单链路验证样本 ******************************/

/**
 * @brief 保存单条物理链路验证期间三业务、双方向的Wire和Load质量。
 *
 * Wire与Queue Overflow均按业务类别和上下行独立累计，不混合样本。
 * *_this_round仅供当前轮积分使用，其他累计字段保留三秒期历史。
 * LOSS验证的首个有效Wire/Load窗口只建立采样边界。
 */
typedef struct
{
    uint64_t lost_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];        // 三业务双方向Wire确认丢失数量
    uint64_t sample_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];      // 三业务双方向Wire样本数量
    uint64_t last_loss_us[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];        // 最后消费的分类Wire源时间

    uint64_t load_lost_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];   // 三业务双方向TX队列溢出数量
    uint64_t load_sample_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT]; // 三业务双方向Load样本数量
    uint64_t last_load_us[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];        // 最后消费的分类Load源时间

    bool     loss_primed[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];         // 分类Wire已建立首个有效窗口基线
    bool     load_primed[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];         // 分类Load已建立首个有效窗口基线
    bool     load_observed[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];       // 验证期曾取得有效分类Load窗口
    bool     load_valid_this_round[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];// 本轮取得新分类Load窗口

    uint32_t overflow_class_mask;                                                                           // 验证期确认OVF的参与业务集合
    uint32_t overflow_class_mask_this_round;                                                                // 本轮新增OVF业务集合
    bool     queue_overflow;                                                                                // 验证期出现过分类队列溢出
} linkg_switch_policy_verify_path_t;

/****************************** Peer级验证会话 ******************************/

/**
 * @brief 一个Peer在同一时刻最多维护一组LOSS或RECOVERY验证会话。
 *
 * ACK后固定3秒，在同一会话内按业务分别累计Wire、Load和Cellular Probe。
 * Wi-Fi无线异常积分为组级；当前轮HCC/Load优先于Wire评分。
 * operation_ids保存各参与业务的事务身份，非参与业务的ID必须为0。
 * 最终分类目标由截止判决生成，发送到Operation的事件协议另行对齐。
 */
typedef struct
{
    /****************************** 事务身份 ******************************/

    uint8_t                                peer_node_id;                                             // 验证所属Peer节点ID
    uint32_t                               peer_generation;                                          // Peer运行代际
    uint32_t                               operation_id;                                             // VERIFY_DONE代表事务ID
    uint32_t                               operation_ids[LINKG_TRANSPORT_CLASS_COUNT];               // 每个参与业务的Operation ID
    uint32_t                               class_mask;                                               // 本组参与验证的业务集合
    linkg_switch_policy_reason_t           reason;                                                   // LOSS或RECOVERY
    linkg_switch_policy_migration_reason_t recovery_reason;                                          // RECOVERY对应的原始迁移原因

    /****************************** 链路与时间 ******************************/

    uint32_t                               wifi_link_id;                                             // 启动时Wi-Fi Link实例
    uint32_t                               cellular_link_id;                                         // 启动时Cellular Link实例
    uint64_t                               started_us;                                               // PlanSync冗余ACK确认时间
    uint64_t                               deadline_us;                                              // 固定3秒验证截止时间
    uint64_t                               finished_us;                                              // 最终结果固化时间
    uint64_t                               last_processed_observation_us;                            // 上一次处理的观测快照时间

    /****************************** Cellular Probe ******************************/

    uint64_t                               cellular_last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];      // 各业务最后消费的Cellular Probe时间
    uint64_t                               cellular_last_good_probe_us[LINKG_TRANSPORT_CLASS_COUNT]; // 各业务最近成功Probe时间
    uint64_t                               cellular_rtt_sum_us[LINKG_TRANSPORT_CLASS_COUNT];         // 各业务有效RTT累计和
    uint32_t                               cellular_rtt_count[LINKG_TRANSPORT_CLASS_COUNT];          // 各业务有效RTT样本数量
    uint32_t                               cellular_max_rtt_us[LINKG_TRANSPORT_CLASS_COUNT];         // 各业务最大有效RTT
    uint32_t                               cellular_probe_unreachable_mask;                          // 验证期间报告不可达的业务掩码

        /****************************** Wi-Fi Probe ******************************/

    uint64_t                               wifi_last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];          // 各业务最后消费的Wi-Fi Probe时间
    uint64_t                               wifi_last_good_probe_us[LINKG_TRANSPORT_CLASS_COUNT];     // 各业务最近成功的Wi-Fi Probe时间
    uint32_t                               wifi_probe_seen_mask;                                     // 验证期间已获得有效Wi-Fi Probe的业务集合
    uint32_t                               wifi_probe_reachable_mask;                                // 每业务最新Wi-Fi Probe确认可达的集合

    /****************************** 质量与负载状态 ******************************/

    linkg_switch_policy_verify_score_window_t wifi_score[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT]; // Wi-Fi全局Wire方向评分窗口
    linkg_switch_policy_verify_hcc_window_t   wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];   // Wi-Fi设备级HCC方向压力窗口
    linkg_switch_policy_verify_load_window_t  wifi_global_load[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT]; // Wi-Fi全局Load方向压力窗口

    uint8_t                                wifi_loss_score;                                          // Wi-Fi异常积分，LOSS初始5，范围0～5
    bool                                   wifi_pressure_this_round;                                 // 本轮确认出现Wi-Fi负载压力
    bool                                   wifi_pressure_known_this_round;                           // 本轮具备判断Wi-Fi负载压力的有效来源
    bool                                   path_interrupted;                                         // 物理Path失效或Link实例发生变化
    bool                                   wifi_traffic_seen;                                        // LOAD恢复期间已采集Wi-Fi流量观测
    bool                                   wifi_tx_active;                                           // LOAD恢复期间Wi-Fi存在发送活动
    bool                                   wifi_rx_active;                                           // LOAD恢复期间Wi-Fi存在接收活动

    /****************************** 业务活动与分类结果 ******************************/

    uint32_t                               active_class_mask;                                        // 已确认存在USER_DATA活动的业务集合
    uint32_t                               comparable_class_mask;                                    // 双链路达到分类比较条件的业务集合
    uint32_t                               cellular_preferred_mask;                                  // 独立质量比较偏向Cellular的业务集合
    uint32_t                               wifi_target_mask;                                         // QoS约束后计划Wi-Fi单发的参与业务
    uint32_t                               cellular_target_mask;                                     // QoS约束后计划Cellular单发的参与业务
    bool                                   class_activity_valid;                                     // 真实USER_DATA活跃状态已确认

    /****************************** 生命周期与结果 ******************************/

    bool                                   active;                                                   // 验证会话已启动
    bool                                   result_pending;                                           // 固定结果尚待Operation消费
    linkg_switch_policy_verify_result_t    result;                                                   // 旧组级事件结果，待分类事件契约升级

    /****************************** 双链路采样 ******************************/

    linkg_switch_policy_verify_path_t      wifi;                                                     // Wi-Fi独立Wire/Load样本
    linkg_switch_policy_verify_path_t      cellular;                                                 // Cellular独立Wire/Load样本
} linkg_switch_policy_verify_runtime_t;

/****************************** 验证启动参数 ******************************/

/**
 * @brief Operation在冗余PlanSync ACK后提交的启动参数。
 *
 * observation仅在begin调用期间有效，Verify不保留该指针。
 */
typedef struct
{
    uint8_t                                peer_node_id;                                // 当前Peer节点ID
    uint32_t                               peer_generation;                             // Peer运行代际
    uint32_t                               operation_ids[LINKG_TRANSPORT_CLASS_COUNT];  // 各参与业务Operation ID
    uint32_t                               class_mask;                                  // 本组参与验证业务集合
    linkg_switch_policy_reason_t           reason;                                      // 当前验证事务原因
    linkg_switch_policy_migration_reason_t recovery_reason;                             // RECOVERY的原始迁移原因
    const linkg_switch_observation_t      *observation;                                 // ACK时的最新观测快照
    uint64_t                               now_us;                                      // PlanSync ACK确认时间
} linkg_switch_policy_verify_begin_params_t;

/****************************** 验证处理参数 ******************************/

/**
 * @brief Switch Worker每轮交付的观测及实际USER_DATA活跃性。
 *
 * USER_DATA活跃状态须在Switch锁外获取；未采集时activity_valid=false，
 * 不得通过Wire样本数推断真实业务活跃性。
 */
typedef struct
{
    const linkg_switch_observation_t *observation;       // 本轮最新双链路观测
    uint64_t                          now_us;            // 本轮单调时间
    uint32_t                          active_class_mask; // 近期真实USER_DATA活跃业务集合
    bool                              activity_valid;    // 活跃性信息是否真实可用
} linkg_switch_policy_verify_process_params_t;

/****************************** 验证处理输出 ******************************/

/**
 * @brief 单轮Verify交付给Operation的结果事件。
 *
 * 最终结果一经固定，应重复返回同一VERIFY_DONE直到事务确认消费。
 */
typedef struct
{
    linkg_switch_policy_event_t event;       // 固定结果对应的事件
    bool                        event_ready; // 本轮是否有待交付事件
} linkg_switch_policy_verify_process_output_t;

/****************************** 验证生命周期 ******************************/

/**
 * @brief 清理当前验证会话，不修改Operation或Plan。
 */
void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime);

/**
 * @brief 冗余PlanSync ACK确认后启动一次固定3秒验证。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_begin_params_t *params);

/**
 * @brief 消费新观测并在截止后生成可重复交付的VERIFY_DONE事件。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_process_params_t *params, linkg_switch_policy_verify_process_output_t *output);

#ifdef __cplusplus
}
#endif

#endif // SWITCH_POLICY_VERIFY_H
