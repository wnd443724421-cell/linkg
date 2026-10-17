/**
 * @file plan_test.c
 * @brief 使用真实计划、调度、同步及RX代码验证业务隔离。
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 直接包含实现以验证内部处理链；仅将硬件、网络发送和重排输出替换为测试适配器。
#include "../../core/switch/linkg_switch.c"
#include "../../core/switch/switch_plan.c"
#include "../../core/switch/switch_wire.c"
#include "../../core/switch/switch_maintenance.c"
#include "../../core/switch/switch_plan_reconcile.c"
#include "../../core/switch/switch_policy_wifi.c"
#include "../../core/switch/switch_policy_loss.c"
#include "../../core/scheduler/linkg_scheduler.c"
#include "../../core/transport/transport_rx.c"
#include "../../core/discovery/discovery_peer_runtime.c"
#include "../../service/web/handlers/web_handler_switch_status.c"
#include "../../service/web/handlers/web_handler_overview.c"

/****************************** 测试状态 ******************************/

#define TEST_WIFI_LINK     11U // 模拟Wi-Fi实例
#define TEST_CELLULAR_LINK 22U // 模拟Cellular实例

linkg_transport_context_t g_transport; // 测试适配器状态，仅测试进程使用

static linkg_packet_pool_t           g_test_pool;                                 // 测试适配器状态，仅测试进程使用
static linkg_switch_wire_plan_sync_t g_test_sync;                                 // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_message_id;                           // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_sync_count;                           // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_ack_count;                            // 测试适配器状态，仅测试进程使用
static int32_t                       g_test_ack_status;                           // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_missing_link;                         // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_path_refs;                            // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_single[LINKG_TRANSPORT_CLASS_COUNT];  // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_reorder[LINKG_TRANSPORT_CLASS_COUNT]; // 测试适配器状态，仅测试进程使用
static uint8_t                       g_test_link_storage[2];                      // 测试适配器状态，仅测试进程使用
static uint8_t                       g_test_path_storage[2];                      // 测试适配器状态，仅测试进程使用
static linkg_transport_class_t       g_test_sent_class;                           // 测试适配器状态，仅测试进程使用
static uint32_t                      g_test_target_count;                         // 测试适配器状态，仅测试进程使用
static void (*g_test_acquire_hook)(void); // 注入Path检查与计划提交之间的并发更新

/****************************** 测试适配器 ******************************/

/**
 * @brief 返回两个固定的模拟链路。
 */
linkg_link_t *linkg_link_manager_get(uint32_t link_id)
{
    if (link_id == TEST_WIFI_LINK)
    {
        return (linkg_link_t *)&g_test_link_storage[0];
    }

    return link_id == TEST_CELLULAR_LINK ? (linkg_link_t *)&g_test_link_storage[1] : NULL;
}

/**
 * @brief 根据接入类型返回模拟链路编号。
 */
uint32_t linkg_link_manager_get_id(linkg_link_access_t access)
{
    return access == LINKG_LINK_ACCESS_WIFI ? TEST_WIFI_LINK : TEST_CELLULAR_LINK;
}

/**
 * @brief 查询模拟接入链路。
 */
linkg_link_t *linkg_link_manager_get_by_access(linkg_link_access_t access)
{
    return linkg_link_manager_get(linkg_link_manager_get_id(access));
}

/**
 * @brief 模拟已启动的有效链路。
 */
bool linkg_link_is_running(const linkg_link_t *link)
{
    return link != NULL;
}

/**
 * @brief 返回模拟链路的运行编号。
 */
uint32_t linkg_link_get_id(const linkg_link_t *link)
{
    return link == (linkg_link_t *)&g_test_link_storage[0] ? TEST_WIFI_LINK : TEST_CELLULAR_LINK;
}

/**
 * @brief 返回模拟链路的接入类型。
 */
linkg_link_access_t linkg_link_get_access(const linkg_link_t *link)
{
    return link == (linkg_link_t *)&g_test_link_storage[0] ? LINKG_LINK_ACCESS_WIFI : LINKG_LINK_ACCESS_CELLULAR;
}

/**
 * @brief 模拟Path获取并统计引用平衡。
 */
int linkg_node_acquire_path(uint8_t node_id, uint32_t link_id, linkg_path_t **path, linkg_path_endpoint_t *next_hop)
{
    assert(node_id != 0U);
    if (g_test_acquire_hook != NULL)
    {
        void (*hook)(void) = g_test_acquire_hook;
        g_test_acquire_hook = NULL;
        hook();
    }
    if (link_id == g_test_missing_link)
    {
        return -ENETDOWN;
    }

    *path = (linkg_path_t *)&g_test_path_storage[link_id == TEST_WIFI_LINK ? 0 : 1];
    memset(next_hop, 0, sizeof(*next_hop));
    g_test_path_refs++;
    return 0;
}

/**
 * @brief 校验每个获取的Path均被释放。
 */
void linkg_path_release(linkg_path_t *path)
{
    assert(path != NULL && g_test_path_refs != 0U);
    g_test_path_refs--;
}

/**
 * @brief 捕获完整计划同步消息。
 */
int linkg_switch_tx_send_plan_sync(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan)
{
    assert(pool != NULL && peer_node_id == 2U);
    g_test_sync       = *plan;
    g_test_message_id = message_id;
    g_test_sync_count++;
    return 0;
}

/**
 * @brief 捕获AP计划确认结果。
 */
int linkg_switch_tx_send_plan_ack(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack)
{
    assert(pool != NULL && peer_node_id == 2U && message_id != 0U);
    g_test_ack_status = ack->status;
    g_test_ack_count++;
    return 0;
}

/**
 * @brief 捕获单链路类别的同步接收路径。
 */
int linkg_transport_reorder_note_single(linkg_transport_peer_t *peer, uint32_t peer_epoch, uint8_t peer_node_id, linkg_transport_class_t traffic_class, uint32_t sequence)
{
    (void)peer;
    (void)peer_epoch;
    (void)peer_node_id;
    (void)sequence;
    g_test_single[traffic_class]++;
    return 0;
}

/**
 * @brief 捕获需要重排的类别。
 */
int linkg_transport_reorder_submit(const linkg_transport_reorder_frame_t *frame)
{
    g_test_reorder[frame->traffic_class]++;
    return 0;
}

/**
 * @brief 捕获普通发送使用的类别和Target数。
 */
int linkg_transport_send_batch(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *items, uint32_t count, int *results)
{
    (void)items;
    g_test_sent_class   = context->traffic_class;
    g_test_target_count = context->target_count;
    for (uint32_t index = 0U; index < count; index++)
    {
        results[index] = 0;
    }
    return (int)count;
}

/**
 * @brief 捕获AP中继发送使用的类别和Target数。
 */
int linkg_transport_forward_batch(const linkg_transport_tx_context_t *context, const linkg_transport_forward_item_t *items, uint32_t count, int *results)
{
    (void)items;
    g_test_sent_class   = context->traffic_class;
    g_test_target_count = context->target_count;
    for (uint32_t index = 0U; index < count; index++)
    {
        results[index] = 0;
    }
    return (int)count;
}

/**
 * @brief 测试中不访问日志设备。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

/****************************** 测试场景 ******************************/

/**
 * @brief 构造三类使用不同模式与主链路的计划。
 */
static linkg_send_plan_t _test_mixed_plan(void)
{
    linkg_send_plan_t plan =
    {
        .classes =
        {
            {LINKG_SEND_MODE_SINGLE, TEST_WIFI_LINK, TEST_CELLULAR_LINK},
            {LINKG_SEND_MODE_REDUNDANT, TEST_CELLULAR_LINK, TEST_WIFI_LINK},
            {LINKG_SEND_MODE_SINGLE, TEST_CELLULAR_LINK, LINKG_LINK_ID_INVALID}
        }
    };
    return plan;
}

/**
 * @brief 重置模拟环境，真实Switch互斥锁保持初始化状态。
 */
static void _test_reset(linkg_device_role_t role)
{
    memset(g_switch.peers, 0, sizeof(g_switch.peers));
    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));
    g_switch.initialized = true;
    g_switch.running     = true;
    g_switch.role        = role;
    g_switch.packet_pool = &g_test_pool;
    g_test_sync_count    = 0U;
    g_test_ack_count     = 0U;
    g_test_missing_link  = LINKG_LINK_ID_INVALID;
    g_test_path_refs     = 0U;
    g_test_acquire_hook  = NULL;
    assert(linkg_switch_set_plan(2U, &(linkg_send_plan_t){0}) == 0);
}

/**
 * @brief 验证整组写入、单类读取、对端隔离和非法类别原子拒绝。
 */
static void _test_plan_atomicity(void)
{
    linkg_send_plan_t       plan;
    linkg_send_plan_t       before;
    linkg_send_plan_t       current;
    linkg_class_send_plan_t selected;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    assert(linkg_switch_get_plan(2U, &before) == 0);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(linkg_switch_get_class_plan(2U, (linkg_transport_class_t)index, &selected) == 0);
        assert(selected.mode == before.classes[index].mode);
        assert(selected.primary_link_id == before.classes[index].primary_link_id);
    }

    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].primary_link_id   = TEST_WIFI_LINK;
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_link_id = TEST_WIFI_LINK;
    assert(linkg_switch_set_plan(2U, &plan) == -EINVAL);
    assert(linkg_switch_get_plan(2U, &current) == 0);
    assert(_linkg_switch_plan_equal(&before, &current));
    assert(linkg_switch_get_class_plan(2U, (linkg_transport_class_t)-1, &selected) == -EINVAL);
    assert(linkg_switch_set_plan(3U, &(linkg_send_plan_t){0}) == 0);
    assert(linkg_switch_get_plan(2U, &current) == 0);
    assert(_linkg_switch_plan_equal(&before, &current));
    puts("PASS plan atomicity / class lookup / peer isolation");
}

/**
 * @brief 验证三业务Wire往返、停发业务、严格长度和所有类别字段校验。
 */
static void _test_wire(void)
{
    uint8_t                       buffer[LINKG_SWITCH_WIRE_MAX_SIZE];
    uint8_t                       altered[LINKG_SWITCH_WIRE_MAX_SIZE];
    linkg_switch_wire_message_t   decoded;
    linkg_switch_wire_plan_sync_t wire;
    linkg_send_plan_t             plan;
    linkg_send_plan_t             roundtrip;
    size_t                        length;

    plan                                     = _test_mixed_plan();
    plan.classes[LINKG_TRANSPORT_CLASS_DATA] = (linkg_class_send_plan_t){0};
    assert(_linkg_switch_plan_local_to_wire(&plan, &wire) == 0);
    assert(linkg_switch_wire_encode_plan_sync(7U, &wire, buffer, sizeof(buffer), &length) == 0);
    assert(length == 20U && buffer[0] == LINKG_SWITCH_WIRE_VERSION);
    assert(linkg_switch_wire_decode(buffer, length, &decoded) == 0);
    assert(_linkg_switch_plan_wire_to_local(&decoded.payload.plan_sync, &roundtrip) == 0);
    assert(_linkg_switch_plan_equal(&plan, &roundtrip));
    for (size_t size = 0U; size < length; size++)
    {
        assert(linkg_switch_wire_decode(buffer, size, &decoded) < 0);
    }

    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        memcpy(altered, buffer, length);
        altered[LINKG_SWITCH_WIRE_HEADER_SIZE + 4U * index] = 255U;
        assert(linkg_switch_wire_decode(altered, length, &decoded) == -EINVAL);
        memcpy(altered, buffer, length);
        altered[LINKG_SWITCH_WIRE_HEADER_SIZE + 4U * index + 3U] = 1U;
        assert(linkg_switch_wire_decode(altered, length, &decoded) == -EINVAL);
    }

    memcpy(altered, buffer, length);
    altered[0] = 2U;
    assert(linkg_switch_wire_decode(altered, length, &decoded) < 0);
    assert(linkg_switch_wire_encode_plan_sync(7U, &wire, buffer, length - 1U, &length) < 0);
    assert(LINKG_SWITCH_WIRE_VERSION == 1U);
    assert(LINKG_SWITCH_WIRE_HEADER_SIZE == 8U && LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE == 20U && LINKG_SWITCH_WIRE_PLAN_ACK_SIZE == 12U);
    puts("PASS Wire v1 / NONE / truncation / per-class validation / wrong version rejection");
}

/**
 * @brief 验证单次同步完整计划以及任一Path失败不产生部分提交。
 */
static void _test_sync(void)
{
    linkg_send_plan_t plan;
    linkg_send_plan_t before;
    linkg_switch_event_t     event = {0};
    linkg_switch_peer_runtime_t *peer;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plan                = _test_mixed_plan();
    peer                = linkg_switch_find_peer_locked(2U);
    g_test_missing_link = TEST_CELLULAR_LINK;
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == -ENETDOWN);
    assert(_linkg_switch_plan_empty(&peer->plan) && g_test_sync_count == 0U);
    g_test_missing_link = 0U;
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    assert(g_test_sync_count == 1U && _linkg_switch_plan_equal(&plan, &peer->plan));
    before                        = peer->plan;
    event.peer_node_id            = 2U;
    event.peer_generation         = peer->generation;
    event.message_id              = g_test_message_id;
    event.payload.plan_ack.status = 0;
    event.message_id++;
    assert(_linkg_switch_plan_process_ack_event(&event, 2000U) == 0);
    assert(peer->role.sta.plan_sync.active);
    event.message_id = g_test_message_id;
    assert(_linkg_switch_plan_process_ack_event(&event, 2000U) == 0);
    assert(!peer->role.sta.plan_sync.active);
    assert(_linkg_switch_plan_equal(&before, &peer->plan));

    _test_reset(LINKG_DEVICE_ROLE_AP);
    peer                    = linkg_switch_find_peer_locked(2U);
    event.peer_generation   = peer->generation;
    event.message_id        = 100U;
    event.payload.plan_sync = g_test_sync;
    g_test_missing_link     = TEST_CELLULAR_LINK;
    assert(_linkg_switch_plan_process_sync_event(&event) == 0);
    assert(g_test_ack_status == -ENETDOWN && _linkg_switch_plan_empty(&peer->plan));
    g_test_missing_link = 0U;
    event.message_id++;
    assert(_linkg_switch_plan_process_sync_event(&event) == 0);
    assert(g_test_ack_status == 0 && _linkg_switch_plan_equal(&plan, &peer->plan));
    assert(_linkg_switch_plan_process_sync_event(&event) == 0);
    assert(g_test_ack_count == 3U && _linkg_switch_plan_equal(&plan, &peer->plan));
    assert(g_test_path_refs == 0U);
    puts("PASS single full-plan sync / ACK / duplicate / failed target atomicity");
}

/**
 * @brief 验证普通发送、中继、指定链路和停发均按业务选择Target。
 */
static void _test_scheduler(void)
{
    linkg_send_plan_t           plan;
    linkg_transport_tx_target_t targets[LINKG_TRANSPORT_TX_TARGET_MAX];
    linkg_scheduler_tx_context_t   context = {0};
    linkg_scheduler_tx_item_t      item = {0};
    linkg_scheduler_peer_group_t   group = {.item_indices = {0U}, .count = 1U, .peer_node_id = 2U};
    linkg_transport_forward_item_t forwarded = {0};
    uint32_t count;
    int      result;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(_linkg_scheduler_build_targets(2U, (linkg_transport_class_t)index, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == 0);
        assert(count == (index == LINKG_TRANSPORT_CLASS_VIDEO ? 2U : 1U));
        assert(targets[0].link == linkg_link_manager_get(plan.classes[index].primary_link_id));
        _linkg_scheduler_release_targets(targets, count);
        context.traffic_class = (linkg_transport_class_t)index;
        assert(_linkg_scheduler_submit_tx_group(&context, &item, &group) == 1);
        assert(g_test_sent_class == (linkg_transport_class_t)index);
        assert(g_test_target_count == (index == LINKG_TRANSPORT_CLASS_VIDEO ? 2U : 1U));
        assert(_linkg_scheduler_submit_forward_group((linkg_transport_class_t)index, &forwarded, &result, &group) == 1);
        assert(g_test_target_count == (index == LINKG_TRANSPORT_CLASS_VIDEO ? 2U : 1U));
    }

    plan.classes[LINKG_TRANSPORT_CLASS_DATA] = (linkg_class_send_plan_t){0};
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_DATA, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == -ENETDOWN);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_DATA, LINKG_SCHEDULER_POLICY_SPECIFIED, TEST_WIFI_LINK, targets, &count) == 0);
    assert(count == 1U);
    _linkg_scheduler_release_targets(targets, count);
    plan.classes[LINKG_TRANSPORT_CLASS_REALTIME] = (linkg_class_send_plan_t){0};
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    group.control         = true;
    context.traffic_class = LINKG_TRANSPORT_CLASS_REALTIME;
    item.type             = LINKG_TRANSPORT_TYPE_SWITCH;
    assert(_linkg_scheduler_submit_tx_group(&context, &item, &group) == 1);
    assert(g_test_target_count == 2U);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_REALTIME, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == -ENETDOWN);
    assert(g_test_path_refs == 0U);
    puts("PASS ordinary / AP relay / specified Probe / NONE scheduling");
}

/**
 * @brief 验证同一接收Chunk内不同类别独立选择Reorder。
 */
static void _test_rx(void)
{
    linkg_send_plan_t plan;
    linkg_transport_rx_state_t states[LINKG_TRANSPORT_CLASS_COUNT] = {0};
    linkg_link_rx_item_t        items[LINKG_TRANSPORT_CLASS_COUNT] = {0};

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    memset(g_test_single, 0, sizeof(g_test_single));
    memset(g_test_reorder, 0, sizeof(g_test_reorder));
    g_transport.local_node_id = 1U;
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        states[index].window_result              = LINKG_TRANSPORT_WINDOW_ACCEPT;
        states[index].header.type                = LINKG_TRANSPORT_TYPE_USER_DATA;
        states[index].header.destination_node_id = 1U;
        states[index].peer_node_id               = 2U;
        states[index].traffic_class              = (linkg_transport_class_t)index;
    }

    _linkg_transport_rx_stage_user_reorder(TEST_WIFI_LINK, items, states, LINKG_TRANSPORT_CLASS_COUNT);
    assert(g_test_single[LINKG_TRANSPORT_CLASS_REALTIME] == 1U);
    assert(g_test_reorder[LINKG_TRANSPORT_CLASS_VIDEO] == 1U);
    assert(g_test_single[LINKG_TRANSPORT_CLASS_DATA] == 1U);
    assert(g_test_reorder[LINKG_TRANSPORT_CLASS_REALTIME] == 0U);
    assert(g_test_single[LINKG_TRANSPORT_CLASS_VIDEO] == 0U);
    puts("PASS mixed-class RX cache / independent Reorder entry");
}

/**
 * @brief 验证维护和失效校准不会覆盖其它类别的有效计划。
 */
static void _test_maintenance_reconcile_discovery(void)
{
    linkg_send_plan_t            plan;
    linkg_send_plan_t            escaped;
    linkg_send_plan_t            expected;
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_reconcile_paths_t paths = {.wifi_link_id = TEST_WIFI_LINK, .cellular_link_id = TEST_CELLULAR_LINK};
    linkg_discovery_report_t report = {.path_flags = LINKG_DISCOVERY_PATH_WIFI_VALID};

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    peer                                          = linkg_switch_find_peer_locked(2U);
    peer->role.sta.observation.valid              = true;
    peer->role.sta.observation.cellular.available = true;
    assert(_linkg_switch_maintenance_build_escape_plan_locked(peer, LINKG_LINK_ACCESS_WIFI, TEST_WIFI_LINK, LINKG_LINK_ACCESS_CELLULAR, TEST_CELLULAR_LINK, &escaped));
    assert(escaped.classes[LINKG_TRANSPORT_CLASS_REALTIME].primary_link_id == TEST_CELLULAR_LINK);
    assert(escaped.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode == LINKG_SEND_MODE_SINGLE);
    assert(escaped.classes[LINKG_TRANSPORT_CLASS_DATA].primary_link_id == plan.classes[LINKG_TRANSPORT_CLASS_DATA].primary_link_id);
    assert(escaped.classes[LINKG_TRANSPORT_CLASS_DATA].secondary_link_id == plan.classes[LINKG_TRANSPORT_CLASS_DATA].secondary_link_id);
    _linkg_switch_reconcile_build(&plan, &paths, &expected);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(expected.classes[index].mode == plan.classes[index].mode);
        assert(expected.classes[index].primary_link_id == plan.classes[index].primary_link_id);
    }
    assert(expected.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_link_id == TEST_WIFI_LINK);
    paths.wifi_link_id = 0U;
    _linkg_switch_reconcile_build(&plan, &paths, &expected);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(expected.classes[index].mode == LINKG_SEND_MODE_SINGLE);
        assert(expected.classes[index].primary_link_id == TEST_CELLULAR_LINK);
    }
    paths.cellular_link_id = 0U;
    _linkg_switch_reconcile_build(&plan, &paths, &expected);
    assert(_linkg_switch_reconcile_plan_empty(&expected));
    _linkg_discovery_build_default_send_plan(&report, LINKG_LINK_ACCESS_WIFI, &expected);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(expected.classes[index].mode == LINKG_SEND_MODE_SINGLE);
        assert(expected.classes[index].primary_link_id == TEST_WIFI_LINK);
    }
    puts("PASS maintenance / independent reconciliation / discovery bootstrap");
}

/**
 * @brief 验证已有硬故障处理一次提交整组并移除其它类别失效的备用链路。
 */
static void _test_existing_hard_failure(void)
{
    linkg_switch_policy_input_t input = {0};
    linkg_switch_peer_runtime_t *peer;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    input.plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &input.plan) == 0);
    peer                               = linkg_switch_find_peer_locked(2U);
    input.peer_node_id                 = 2U;
    input.peer_generation              = peer->generation;
    input.observation.wifi.link_id     = TEST_WIFI_LINK;
    input.observation.cellular.link_id = TEST_CELLULAR_LINK;
    g_test_missing_link                = TEST_WIFI_LINK;
    assert(_linkg_switch_policy_switch_to_cellular(&input, 1000U) == 0);
    assert(g_test_sync_count == 1U);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(peer->plan.classes[index].mode == LINKG_SEND_MODE_SINGLE);
        assert(peer->plan.classes[index].primary_link_id == TEST_CELLULAR_LINK);
        assert(peer->plan.classes[index].secondary_link_id == LINKG_LINK_ID_INVALID);
    }
    puts("PASS existing hard failure / single full-plan commit / stale backup removal");
}

/**
 * @brief 验证Web状态返回完整类别对象且每类模式和链路互不覆盖。
 */
static void _test_web_json(void)
{
    linkg_send_plan_t plan = _test_mixed_plan();
    cJSON *root = cJSON_CreateObject();
    cJSON *classes;
    cJSON *current;

    assert(root != NULL);
    assert(_linkg_web_switch_status_json_add_plan(root, "plan", &plan, true) == LINKG_JSON_OK);
    classes = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "plan"), "classes");
    assert(cJSON_GetArraySize(classes) == 3);
    current = cJSON_GetObjectItemCaseSensitive(classes, "realtime");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(current, "mode")->valuestring, "single") == 0);
    current = cJSON_GetObjectItemCaseSensitive(classes, "video");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(current, "mode")->valuestring, "redundant") == 0);
    current = cJSON_GetObjectItemCaseSensitive(classes, "data");
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(current, "primary"), "link_id")->valueint == TEST_CELLULAR_LINK);
    assert(_linkg_web_switch_status_json_add_plan(root, "invalid", &plan, false) == LINKG_JSON_OK);
    classes = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "invalid"), "classes");
    current = cJSON_GetObjectItemCaseSensitive(classes, "video");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(current, "mode")->valuestring, "none") == 0);
    cJSON_Delete(root);
    puts("PASS Web JSON / complete classes / invalid-state normalization");
}

/**
 * @brief 并发写入两种完整计划，验证读取端不会观察到部分业务新旧混合。
 */
static void *_test_plan_writer(void *user_data)
{
    const linkg_send_plan_t *plans = user_data;

    for (uint32_t index = 0U; index < 10000U; index++)
    {
        assert(linkg_switch_set_plan(2U, &plans[index % 2U]) == 0);
    }

    return NULL;
}

/**
 * @brief 验证概览保留三类业务信息，并且仅为直连对端返回本机计划。
 */
static void _test_overview_json(void)
{
    linkg_web_overview_node_info_t info;
    cJSON                         *node;
    cJSON                         *plan;
    cJSON                         *classes;

    memset(&info, 0, sizeof(info));
    info.node_id           = 2U;
    info.role              = LINKG_DEVICE_ROLE_STA;
    info.relation          = LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT;
    info.virtual_ip.s_addr = inet_addr("172.28.0.2");
    info.plan              = _test_mixed_plan();
    info.plan_valid        = true;
    memset(&info.plan.classes[LINKG_TRANSPORT_CLASS_DATA], 0, sizeof(info.plan.classes[LINKG_TRANSPORT_CLASS_DATA]));
    assert(_linkg_web_overview_build_node_json(&info, &node) == 0);
    plan    = cJSON_GetObjectItemCaseSensitive(node, "plan");
    classes = cJSON_GetObjectItemCaseSensitive(plan, "classes");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "valid")));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(classes, "realtime"), "primary_link")->valuestring, "wifi") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(classes, "video"), "primary_link")->valuestring, "cellular") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(classes, "data"), "mode")->valuestring, "none") == 0);
    cJSON_Delete(node);

    info.relation = LINKG_WEB_OVERVIEW_NODE_RELATION_VIA_AP;
    assert(_linkg_web_overview_build_node_json(&info, &node) == 0);
    assert(cJSON_GetObjectItemCaseSensitive(node, "plan") == NULL);
    cJSON_Delete(node);
    puts("PASS Overview JSON / mixed class plans / direct-peer scope");
}

/**
 * @brief 验证整组计划的互斥锁快照语义。
 */
static void _test_concurrent_snapshot(void)
{
    linkg_send_plan_t plans[2];
    linkg_send_plan_t current;
    pthread_t         writer;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    plans[0] = _test_mixed_plan();
    memset(&plans[1], 0, sizeof(plans[1]));
    assert(linkg_switch_set_plan(2U, &plans[0]) == 0);
    assert(pthread_create(&writer, NULL, _test_plan_writer, plans) == 0);
    for (uint32_t index = 0U; index < 10000U; index++)
    {
        assert(linkg_switch_get_plan(2U, &current) == 0);
        assert(_linkg_switch_plan_equal(&current, &plans[0]) || _linkg_switch_plan_equal(&current, &plans[1]));
    }
    assert(pthread_join(writer, NULL) == 0);
    puts("PASS concurrent full-plan snapshots / 10000 writes and reads");
}

/**
 * @brief 执行完整业务计划回归测试。
 */
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    _test_plan_atomicity();
    _test_wire();
    _test_sync();
    _test_scheduler();
    _test_rx();
    _test_maintenance_reconcile_discovery();
    _test_existing_hard_failure();
    _test_web_json();
    _test_concurrent_snapshot();
    _test_overview_json();
    puts("All class send-plan tests passed");
    return 0;
}
