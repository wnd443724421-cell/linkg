/**
 * @file switch_policy_wifi.c
 * @brief LinkG Wi-Fi主链路切换策略框架
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-08
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"

#include "switch_plan.h"

/****************************** 链路基础条件 ******************************/

/**
 * @brief 判断Cellular是否可作为本轮Wi-Fi切换的目标接入。
 *
 * 只判断观测可用性和Maintenance；具体业务是否已配置Cellular备份，
 * 由各业务Plan的Secondary独立决定，不在Policy中注册Path。
 */
static bool _linkg_switch_policy_cellular_usable(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_cellular_observation_t *cellular;

    if (input == NULL || input->cellular_blocked)
    {
        return false;
    }

    cellular = &input->observation.cellular;

    return cellular->available && cellular->link_id != LINKG_LINK_ID_INVALID;
}

/**
 * @brief 判断指定业务当前是否以Wi-Fi为主链路。
 */
static bool _linkg_switch_policy_wifi_primary(const linkg_class_send_plan_t *current, uint32_t wifi_link_id)
{
    if (current == NULL || wifi_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    return current->mode != LINKG_SEND_MODE_NONE && current->primary_link_id == wifi_link_id;
}

/**
 * @brief 判断指定Wi-Fi主链路业务是否已有本轮可用的Cellular备用Link。
 *
 * Secondary由Plan Reconcile维护；Policy不能临时补建备用Link。
 */
static bool _linkg_switch_policy_wifi_has_cellular_backup(const linkg_switch_policy_input_t *input,
                                                          const linkg_class_send_plan_t *current)
{
    return _linkg_switch_policy_cellular_usable(input) && current != NULL &&
           _linkg_switch_policy_wifi_primary(current, input->observation.wifi.link_id) &&
           current->secondary_link_id == input->observation.cellular.link_id;
}

/**
 * @brief 判断Wi-Fi是否发生已确认的物理断开或Path不可用。
 *
 * 此处只处理明确硬故障，不以RSSI或瞬时丢包代替硬故障判定。
 */
static bool _linkg_switch_policy_wifi_hard_failed(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_wifi_observation_t *wifi;

    if (input == NULL)
    {
        return false;
    }

    wifi = &input->observation.wifi;

    return !wifi->available || (wifi->radio.valid && !wifi->radio.connected);
}

/****************************** 完整Plan提交 ******************************/

/**
 * @brief 将本轮策略生成的完整Plan原子提交到STA并发起AP同步。
 *
 * 比较previous与写入由Plan模块在同一把锁内完成；遇到Policy、
 * Reconcile或Maintenance的并发更新时，返回-EAGAIN而不覆盖新计划。
 */
static int _linkg_switch_policy_wifi_commit(const linkg_switch_policy_input_t *input,
                                           const linkg_send_plan_t *plan, uint64_t now_us)
{
    return linkg_switch_plan_update_if_current(input->peer_node_id,
                                               input->peer_generation,
                                               &input->plan,
                                               plan,
                                               true,
                                               now_us);
}

/****************************** Wi-Fi硬故障 ******************************/

/**
 * @brief Wi-Fi硬故障时，仅将已有Cellular备份的Wi-Fi主链路业务降为Cellular单发。
 *
 * 不覆盖其它业务原有的Cellular主链路计划；没有备用Link的业务交给
 * Reconcile处理资源兜底，Policy不凭观测临时创建新Path。
 */
static int _linkg_switch_policy_wifi_hard_failover(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_send_plan_t        plan;
    linkg_class_send_plan_t *current;
    uint32_t                 class_index;
    bool                     changed;

    plan    = input->plan;
    changed = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan.classes[class_index];

        // 混合计划中，Cellular为主的冗余业务同样要停止使用故障Wi-Fi。
        if (current->mode == LINKG_SEND_MODE_REDUNDANT &&
            current->primary_link_id == input->observation.cellular.link_id &&
            current->secondary_link_id == input->observation.wifi.link_id)
        {
            current->mode              = LINKG_SEND_MODE_SINGLE;
            current->secondary_link_id = LINKG_LINK_ID_INVALID;
            changed                    = true;
            continue;
        }

        if (!_linkg_switch_policy_wifi_has_cellular_backup(input, current))
        {
            continue;
        }

        current->mode              = LINKG_SEND_MODE_SINGLE;
        current->primary_link_id   = input->observation.cellular.link_id;
        current->secondary_link_id = LINKG_LINK_ID_INVALID;
        changed                    = true;
    }

    return changed ? _linkg_switch_policy_wifi_commit(input, &plan, now_us) : 0;
}

/****************************** Wi-Fi单发退化 ******************************/

/**
 * @brief 对具备Cellular备份的Wi-Fi单发业务开启冗余验证。
 *
 * 只修改满足条件的业务类别，完整Plan仍一次性原子提交。
 * 当前仅提供整体进入冗余的框架；分级启用时序留给后续策略设计。
 */
static int _linkg_switch_policy_wifi_enter_dual_verify(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_send_plan_t        plan;
    linkg_class_send_plan_t *current;
    uint32_t                 class_index;
    bool                     changed;

    plan    = input->plan;
    changed = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan.classes[class_index];

        if (current->mode != LINKG_SEND_MODE_SINGLE ||
            !_linkg_switch_policy_wifi_has_cellular_backup(input, current))
        {
            continue;
        }

        current->mode = LINKG_SEND_MODE_REDUNDANT;
        changed       = true;
    }

    return changed ? _linkg_switch_policy_wifi_commit(input, &plan, now_us) : 0;
}

/**
 * @brief 按DATA、VIDEO、REALTIME优先级依次迁移Wi-Fi业务。
 *
 * 每次只迁移一类业务，迁移后使用Cellular单发，
 * 保留Wi-Fi备用候选，后续由恢复策略决定何时迁回。
 */
static int _linkg_switch_policy_wifi_migrate_load(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    static const linkg_transport_class_t order[] =
    {
        LINKG_TRANSPORT_CLASS_DATA,
        LINKG_TRANSPORT_CLASS_VIDEO,
        LINKG_TRANSPORT_CLASS_REALTIME
    };

    linkg_send_plan_t        plan;
    linkg_class_send_plan_t *current;
    uint32_t                 index;

    plan = input->plan;

    for (index = 0U; index < sizeof(order) / sizeof(order[0]); index++)
    {
        current = &plan.classes[order[index]];

        if (!_linkg_switch_policy_wifi_has_cellular_backup(input, current))
        {
            continue;
        }

        current->mode              = LINKG_SEND_MODE_SINGLE;
        current->primary_link_id   = input->observation.cellular.link_id;
        current->secondary_link_id = input->observation.wifi.link_id;

        return _linkg_switch_policy_wifi_commit(input, &plan, now_us);
    }

    return 0;
}

/**
 * @brief 处理Wi-Fi单发业务的Wire丢包和Queue Overflow异常。
 *
 * Wire Loss达到BAD时进入冗余验证。
 * Load达到BAD时按优先级迁移下一类业务。
 */
static int _linkg_switch_policy_wifi_process_single(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_loss_state_t loss_state;
    linkg_switch_policy_load_state_t load_state;
    int                              ret;

    ret = linkg_switch_policy_loss_process(input, now_us, &loss_state);
    if (ret != 0)
    {
        return ret;
    }

    if (loss_state == LINKG_SWITCH_POLICY_LOSS_BAD)
    {
        return _linkg_switch_policy_wifi_enter_dual_verify(input, now_us);
    }

    ret = linkg_switch_policy_load_process(input, now_us, &load_state);
    if (ret != 0)
    {
        return ret;
    }

    if (load_state == LINKG_SWITCH_POLICY_LOAD_BAD)
    {
        return _linkg_switch_policy_wifi_migrate_load(input, now_us);
    }

    return 0;
}

/****************************** Wi-Fi冗余健康检查 ******************************/

/**
 * @brief 处理Wi-Fi主链路的冗余验证与最终切换判决。
 *
 * 保留健康检查入口；探测RTT、检查期丢包、业务速率限制与退出冗余
 * 的决策尚未实现，此处不能仅凭进入REDUNDANT就自动切至Cellular。
 */
static int _linkg_switch_policy_wifi_process_dual_verify(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    (void)input;
    (void)now_us;

    return 0;
}

/****************************** Wi-Fi主链路策略入口 ******************************/

/**
 * @brief 处理当前至少一类业务以Wi-Fi为主链路时的STA切换策略。
 *
 * Maintenance、资源登记由其它模块负责；Policy只判定质量切换。
 * 硬故障优先于软退化，所有实际变更都通过Plan模块一次性同步。
 */
int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       class_index;
    bool                           has_single;
    bool                           has_redundant;
    int                            ret;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (input->wifi_blocked || !_linkg_switch_policy_cellular_usable(input))
    {
        return 0;
    }

    has_single    = false;
    has_redundant = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &input->plan.classes[class_index];

        if (!_linkg_switch_policy_wifi_has_cellular_backup(input, current))
        {
            continue;
        }

        has_single    = has_single || current->mode == LINKG_SEND_MODE_SINGLE;
        has_redundant = has_redundant || current->mode == LINKG_SEND_MODE_REDUNDANT;
    }

    if (!has_single && !has_redundant)
    {
        return 0;
    }

    if (_linkg_switch_policy_wifi_hard_failed(input))
    {
        return _linkg_switch_policy_wifi_hard_failover(input, now_us);
    }

    if (has_redundant)
    {
        ret = _linkg_switch_policy_wifi_process_dual_verify(input, now_us);
        if (ret != 0)
        {
            return ret;
        }
    }

    if (has_single)
    {
        return _linkg_switch_policy_wifi_process_single(input, now_us);
    }

    return 0;
}
