/**
 * @file audit_test.c
 * @brief 复现计划同步、代际、维护及并发提交审查问题。
 */

// 复用实际模块和测试适配器，单独执行本轮审查场景。
#define main linkg_original_plan_test_main // 测试编译入口重命名
#include "plan_test.c"
#undef main

#include "../../core/switch/switch_event.c"
#include "../../core/switch/switch_status.c"

/****************************** 测试状态 ******************************/

static uint64_t g_audit_now_us = 1000000ULL; // 测试使用的确定性单调时间
static uint32_t g_audit_maintenance_count;     // 捕获维护通知数量
static uint32_t g_audit_maintenance_ack_count; // 捕获维护确认数量

/****************************** 外部适配 ******************************/

/**
 * @brief 测试中使用确定性时间，不访问实际时钟。
 */
uint64_t linkg_time_monotonic_us(void)
{
    return g_audit_now_us;
}

/**
 * @brief 测试直接驱动Worker事务，不创建后台线程。
 */
int linkg_thread_wakeup(linkg_thread_t *thread)
{
    (void)thread;
    return 0;
}

/**
 * @brief 测试没有启动后台线程，允许验证实际Switch反初始化路径。
 */
bool linkg_thread_is_started(const linkg_thread_t *thread)
{
    (void)thread;
    return false;
}

/**
 * @brief 测试不创建线程资源，反初始化适配器无需释放对象。
 */
void linkg_thread_deinit(linkg_thread_t *thread)
{
    (void)thread;
}

/**
 * @brief 捕获实际Maintenance通知。
 */
int linkg_switch_tx_send_maintenance(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_maintenance_t *maintenance)
{
    assert(pool != NULL && peer_node_id == 2U && message_id != 0U && maintenance != NULL);
    g_audit_maintenance_count++;
    return 0;
}

/**
 * @brief 捕获实际Maintenance确认。
 */
int linkg_switch_tx_send_maintenance_ack(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_maintenance_ack_t *ack)
{
    assert(pool != NULL && peer_node_id == 2U && message_id != 0U && ack != NULL);
    g_audit_maintenance_ack_count++;
    return 0;
}

/****************************** 场景辅助 ******************************/

/**
 * @brief 构造三个业务默认Wi-Fi单发且Cellular作为候选的完整计划。
 */
static linkg_send_plan_t _audit_wifi_plan(void)
{
    linkg_send_plan_t plan;

    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        plan.classes[index] = (linkg_class_send_plan_t){LINKG_SEND_MODE_SINGLE, TEST_WIFI_LINK, TEST_CELLULAR_LINK};
    }
    return plan;
}

/**
 * @brief 按当前Peer代际构造真实计划ACK事件。
 */
static linkg_switch_event_t _audit_ack(uint32_t message_id, int32_t status)
{
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_event_t         event = {0};

    peer                          = linkg_switch_find_peer_locked(2U);
    event.type                    = LINKG_SWITCH_EVENT_PLAN_ACK_RX;
    event.peer_node_id            = 2U;
    event.peer_generation         = peer->generation;
    event.message_id              = message_id;
    event.payload.plan_ack.status = status;
    return event;
}

/**
 * @brief 在测试进程中切换两个独立Peer快照，模拟真实AP与STA。
 */
static void _audit_restore(linkg_device_role_t role, const linkg_switch_peer_runtime_t *peer)
{
    g_switch.role     = role;
    g_switch.peers[0] = *peer;
}

/****************************** 同步恢复 ******************************/

/**
 * @brief AP拒绝后保持整组不变，退避新事务可重新校验并最终确认完整计划。
 */
static void _audit_rejection_recovery(void)
{
    linkg_switch_peer_runtime_t  sta;
    linkg_switch_peer_runtime_t  ap;
    linkg_switch_peer_runtime_t *peer;
    linkg_send_plan_t base = _audit_wifi_plan();
    linkg_send_plan_t desired = _test_mixed_plan();
    linkg_switch_event_t event = {0};
    linkg_switch_wire_plan_sync_t wire;
    uint32_t                      old_message_id;
    uint32_t                      new_message_id;
    uint64_t                      due_us;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(linkg_switch_set_plan(2U, &base) == 0);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &desired, 1000U) == 0);
    old_message_id = g_test_message_id;
    wire           = g_test_sync;
    sta            = *peer;

    _test_reset(LINKG_DEVICE_ROLE_AP);
    assert(linkg_switch_set_plan(2U, &base) == 0);
    peer                    = linkg_switch_find_peer_locked(2U);
    event.type              = LINKG_SWITCH_EVENT_PLAN_SYNC_RX;
    event.peer_node_id      = 2U;
    event.peer_generation   = peer->generation;
    event.message_id        = old_message_id;
    event.payload.plan_sync = wire;
    g_test_missing_link     = TEST_CELLULAR_LINK;
    assert(linkg_switch_plan_process_event(&event, 2000U) == 0);
    assert(g_test_ack_status == -ENETDOWN && _linkg_switch_plan_equal(&peer->plan, &base));
    ap = *peer;

    _audit_restore(LINKG_DEVICE_ROLE_STA, &sta);
    peer  = &g_switch.peers[0];
    event = _audit_ack(old_message_id, -ENETDOWN);
    assert(linkg_switch_plan_process_event(&event, 3000U) == 0);
    assert(!peer->role.sta.plan_sync.active && peer->role.sta.plan_sync.recovery_pending && !peer->role.sta.plan_sync.confirmed);
    assert(_linkg_switch_plan_equal(&peer->plan, &desired));
    due_us = peer->role.sta.plan_sync.next_retry_us;
    uint32_t sends = g_test_sync_count;
    assert(linkg_switch_plan_process(due_us - 1U) == 0 && g_test_sync_count == sends);
    assert(linkg_switch_plan_next_deadline_locked() == due_us);
    assert(linkg_switch_plan_process(due_us) == 0 && g_test_sync_count == sends + 1U);
    new_message_id = g_test_message_id;
    assert(new_message_id != old_message_id && peer->role.sta.plan_sync.recovery_count == 1U);
    sta = *peer;

    _audit_restore(LINKG_DEVICE_ROLE_AP, &ap);
    peer                    = &g_switch.peers[0];
    event                   = (linkg_switch_event_t){.type = LINKG_SWITCH_EVENT_PLAN_SYNC_RX, .peer_node_id = 2U, .peer_generation = peer->generation, .message_id = old_message_id};
    event.payload.plan_sync = wire;
    g_test_missing_link     = 0U;
    assert(linkg_switch_plan_process_event(&event, due_us) == 0);
    assert(g_test_ack_status == -ENETDOWN && _linkg_switch_plan_equal(&peer->plan, &base));
    event.message_id = new_message_id;
    assert(linkg_switch_plan_process_event(&event, due_us + 1U) == 0);
    assert(g_test_ack_status == 0 && _linkg_switch_plan_equal(&peer->plan, &desired));
    ap = *peer;

    _audit_restore(LINKG_DEVICE_ROLE_STA, &sta);
    event = _audit_ack(new_message_id, 0);
    assert(linkg_switch_plan_process_event(&event, due_us + 2U) == 0);
    assert(g_switch.peers[0].role.sta.plan_sync.confirmed && !g_switch.peers[0].role.sta.plan_sync.recovery_pending);
    assert(_linkg_switch_plan_equal(&g_switch.peers[0].plan, &ap.plan));
    puts("PASS T05/T06/T07 AP rejection / cached ACK / backoff new ID / eventual full-plan agreement");
}

/**
 * @brief AP已应用但ACK丢失时不回滚，重复事务返回缓存成功ACK。
 */
static void _audit_lost_ack(void)
{
    linkg_send_plan_t plan = _test_mixed_plan();
    linkg_switch_peer_runtime_t  sta;
    linkg_switch_peer_runtime_t  ap;
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_event_t event = {0};
    linkg_switch_wire_plan_sync_t wire;
    uint32_t                      old_id;
    uint32_t                      new_id;
    uint64_t                      now_us;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    old_id = g_test_message_id;
    wire   = g_test_sync;
    sta    = *peer;
    _test_reset(LINKG_DEVICE_ROLE_AP);
    peer                    = linkg_switch_find_peer_locked(2U);
    event                   = (linkg_switch_event_t){.type = LINKG_SWITCH_EVENT_PLAN_SYNC_RX, .peer_node_id = 2U, .peer_generation = peer->generation, .message_id = old_id};
    event.payload.plan_sync = wire;
    assert(linkg_switch_plan_process_event(&event, 2000U) == 0 && g_test_ack_status == 0);
    ap = *peer;
    _audit_restore(LINKG_DEVICE_ROLE_STA, &sta);
    peer   = &g_switch.peers[0];
    now_us = peer->role.sta.plan_sync.deadline_us;
    assert(linkg_switch_plan_process(now_us) == 0);
    assert(peer->role.sta.plan_sync.last_status == -ETIMEDOUT && !peer->role.sta.plan_sync.confirmed);
    assert(_linkg_switch_plan_equal(&peer->plan, &plan));

    // 同一事务的迟到成功ACK在尚未启动新事务时仍可完成确认。
    event = _audit_ack(old_id, 0);
    assert(linkg_switch_plan_process_event(&event, now_us + 1U) == 0 && peer->role.sta.plan_sync.confirmed);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, now_us + 2U) == 0);
    old_id = g_test_message_id;
    now_us = peer->role.sta.plan_sync.deadline_us;
    assert(linkg_switch_plan_process(now_us) == 0);
    now_us = peer->role.sta.plan_sync.next_retry_us;
    assert(linkg_switch_plan_process(now_us) == 0);
    new_id = g_test_message_id;
    sta    = *peer;
    assert(old_id != new_id);

    _audit_restore(LINKG_DEVICE_ROLE_AP, &ap);
    event                   = (linkg_switch_event_t){.type = LINKG_SWITCH_EVENT_PLAN_SYNC_RX, .peer_node_id = 2U, .peer_generation = ap.generation, .message_id = new_id};
    event.payload.plan_sync = wire;
    assert(linkg_switch_plan_process_event(&event, now_us) == 0 && g_test_ack_status == 0);
    assert(linkg_switch_plan_process_event(&event, now_us + 1U) == 0 && g_test_ack_status == 0);
    _audit_restore(LINKG_DEVICE_ROLE_STA, &sta);
    event = _audit_ack(old_id, 0);
    assert(linkg_switch_plan_process_event(&event, now_us + 2U) == 0 && !g_switch.peers[0].role.sta.plan_sync.confirmed);
    event = _audit_ack(new_id, 0);
    assert(linkg_switch_plan_process_event(&event, now_us + 3U) == 0 && g_switch.peers[0].role.sta.plan_sync.confirmed);
    puts("PASS T08 ACK loss / uncertain result / no rollback / late and recovered ACK");
}

/**
 * @brief 连续拒绝和连续超时均有硬上限，达到上限后仅显式新提交重新启动。
 */
static void _audit_bounded_recovery(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_switch_peer_runtime_t      *peer;
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_event_t              event;
    uint64_t                          now_us;
    uint32_t                          previous_id;

    for (uint32_t rejected = 0U; rejected < 2U; rejected++)
    {
        _test_reset(LINKG_DEVICE_ROLE_STA);
        peer = linkg_switch_find_peer_locked(2U);
        assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
        sync   = &peer->role.sta.plan_sync;
        now_us = 2000U;
        for (uint32_t round = 0U; round <= LINKG_SWITCH_PLAN_SYNC_RECOVERY_MAX; round++)
        {
            previous_id = sync->message_id;
            if (rejected != 0U)
            {
                event = _audit_ack(previous_id, -EBUSY);
                assert(linkg_switch_plan_process_event(&event, now_us) == 0);
                assert(sync->last_status == -EBUSY);
            }
            else
            {
                while (sync->retry_count < LINKG_SWITCH_PLAN_SYNC_RETRY_MAX)
                {
                    assert(linkg_switch_plan_process(sync->next_retry_us) == 0);
                }
                assert(linkg_switch_plan_process(sync->next_retry_us - 1U) == 0 && sync->active);
                now_us = sync->next_retry_us;
                assert(linkg_switch_plan_process(now_us) == 0 && sync->last_status == -ETIMEDOUT);
            }
            assert(!sync->active && !sync->confirmed);
            if (round < LINKG_SWITCH_PLAN_SYNC_RECOVERY_MAX)
            {
                assert(sync->recovery_pending && sync->next_retry_us == now_us + (LINKG_SWITCH_PLAN_SYNC_RECOVERY_BASE_US << round));
                now_us = sync->next_retry_us;
                assert(linkg_switch_plan_process(now_us) == 0 && sync->active && sync->message_id != previous_id);
                assert(sync->recovery_count == round + 1U);
            }
        }
        assert(!sync->recovery_pending && linkg_switch_plan_next_deadline_locked() == UINT64_MAX);
        uint32_t sends = g_test_sync_count;
        assert(linkg_switch_plan_process(now_us + 3600000000ULL) == 0 && g_test_sync_count == sends);
        assert(sends <= (LINKG_SWITCH_PLAN_SYNC_RECOVERY_MAX + 1U) * LINKG_SWITCH_PLAN_SYNC_RETRY_MAX);
        assert(_linkg_switch_plan_equal(&peer->plan, &plan));
        assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, now_us + 3600000001ULL) == 0);
        assert(sync->active && sync->recovery_count == 0U && sync->message_id != previous_id);
    }
    puts("PASS T09 bounded timeout and rejection recovery / 1-2-4 second backoff / explicit restart");
}

/****************************** 代际与并发 ******************************/

/**
 * @brief 已入队旧代际ACK和重连后才收到的旧消息编号均不能确认新事务。
 */
static void _audit_generation_and_supersession(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_event_t         old_event;
    linkg_switch_event_t         event;
    uint32_t                     old_generation;
    uint32_t                     old_id;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    old_event      = _audit_ack(g_test_message_id, 0);
    old_id         = old_event.message_id;
    old_generation = peer->generation;
    assert(linkg_switch_event_post(&old_event) == 0);
    assert(linkg_switch_remove_plan(2U) == 0);
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    peer = linkg_switch_find_peer_locked(2U);
    assert(peer->generation != old_generation);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 2000U) == 0 && g_test_message_id != old_id);
    assert(linkg_switch_event_pop_locked(&event));
    assert(linkg_switch_plan_process_event(&event, 3000U) == -ESTALE && peer->role.sta.plan_sync.active);
    event.peer_generation = peer->generation;
    assert(linkg_switch_plan_process_event(&event, 3000U) == 0 && peer->role.sta.plan_sync.active);

    old_event                                                  = _audit_ack(g_test_message_id, 0);
    plan.classes[LINKG_TRANSPORT_CLASS_DATA].primary_link_id   = TEST_CELLULAR_LINK;
    plan.classes[LINKG_TRANSPORT_CLASS_DATA].secondary_link_id = TEST_WIFI_LINK;
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 4000U) == 0);
    assert(linkg_switch_plan_process_event(&old_event, 5000U) == 0 && peer->role.sta.plan_sync.active);
    event = _audit_ack(g_test_message_id, -EBUSY);
    assert(linkg_switch_plan_process_event(&event, 5000U) == 0 && peer->role.sta.plan_sync.recovery_pending);
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO] = (linkg_class_send_plan_t){LINKG_SEND_MODE_SINGLE, TEST_CELLULAR_LINK, 0U};
    assert(linkg_switch_set_plan(2U, &plan) == 0);
    assert(!peer->role.sta.plan_sync.active && !peer->role.sta.plan_sync.recovery_pending);
    event.payload.plan_ack.status = 0;
    assert(linkg_switch_plan_process_event(&event, 6000U) == 0 && !peer->role.sta.plan_sync.confirmed);
    uint32_t sends = g_test_sync_count;
    assert(linkg_switch_plan_process(1000000000ULL) == 0 && g_test_sync_count == sends);
    assert(_linkg_switch_plan_equal(&peer->plan, &plan));
    old_generation = peer->generation;
    old_id         = g_switch.next_plan_message_id;
    pthread_mutex_lock(&g_switch.lock);
    _linkg_switch_reset_all_locked();
    pthread_mutex_unlock(&g_switch.lock);
    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(peer->generation != old_generation);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1100000000ULL) == 0 && g_test_message_id != old_id);
    old_generation   = peer->generation;
    old_id           = g_test_message_id;
    g_switch.running = false;
    assert(linkg_switch_deinit() == 0);
    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(peer->generation != old_generation);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1200000000ULL) == 0 && g_test_message_id != old_id);
    puts("PASS T13/T14 queued old generation / reconnect message IDs / old ACK / canceled stale recovery");
}

/**
 * @brief 在Path检查期间注入另一份新计划，复现原检查与提交之间的竞争窗口。
 */
static void _audit_replace_during_path_check(void)
{
    linkg_send_plan_t plan = _test_mixed_plan();
    assert(linkg_switch_set_plan(2U, &plan) == 0);
}

/**
 * @brief 只改DATA时保留另外两类，竞争提交不会覆盖更新后的完整计划。
 */
static void _audit_conditional_commit(void)
{
    linkg_send_plan_t previous = _audit_wifi_plan();
    linkg_send_plan_t desired = previous;
    linkg_send_plan_t current;
    linkg_send_plan_t fallback = _audit_wifi_plan();
    linkg_switch_peer_runtime_t *peer;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(linkg_switch_set_plan(2U, &previous) == 0);
    desired.classes[LINKG_TRANSPORT_CLASS_DATA] = (linkg_class_send_plan_t){LINKG_SEND_MODE_SINGLE, TEST_CELLULAR_LINK, TEST_WIFI_LINK};
    assert(linkg_switch_plan_update_if_current(2U, peer->generation, &previous, &desired, true, 1000U) == 0);
    assert(memcmp(&peer->plan.classes[0], &previous.classes[0], sizeof(linkg_class_send_plan_t) * 2U) == 0);
    assert(_linkg_switch_plan_equal(&peer->plan, &desired));
    previous            = desired;
    g_test_acquire_hook = _audit_replace_during_path_check;
    assert(linkg_switch_plan_update_if_current(2U, peer->generation, &previous, &fallback, true, 2000U) == -EAGAIN);
    current = _test_mixed_plan();
    assert(_linkg_switch_plan_equal(&peer->plan, &current));
    assert(linkg_switch_plan_update_if_current(2U, peer->generation, &previous, &desired, false, 3000U) == -EAGAIN);
    assert(_linkg_switch_plan_equal(&peer->plan, &current));
    linkg_switch_policy_input_t input = {0};
    input.peer_node_id                   = 2U;
    input.peer_generation                = peer->generation;
    input.plan                           = previous;
    input.observation.wifi.link_id       = TEST_WIFI_LINK;
    input.observation.wifi.available     = false;
    input.observation.cellular.link_id   = TEST_CELLULAR_LINK;
    input.observation.cellular.available = true;
    assert(linkg_switch_policy_wifi_process(&input, 4000U) == -EAGAIN);
    assert(_linkg_switch_plan_equal(&peer->plan, &current));
    puts("PASS T02/T15 DATA-only full commit / compare and write atomicity / injected concurrent update");
}

/****************************** 维护与调度 ******************************/

/**
 * @brief 候选备用不可用不阻止SINGLE提交，主链路失效不擅自由Scheduler启用备用。
 */
static void _audit_single_backup_and_none(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_switch_peer_runtime_t *peer;
    linkg_transport_tx_target_t  targets[LINKG_TRANSPORT_TX_TARGET_MAX];
    uint32_t                     count;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer                = linkg_switch_find_peer_locked(2U);
    g_test_missing_link = TEST_CELLULAR_LINK;
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_VIDEO, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == 0 && count == 1U);
    _linkg_scheduler_release_targets(targets, count);
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode = LINKG_SEND_MODE_REDUNDANT;
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 2000U) == -ENETDOWN);
    assert(peer->plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode == LINKG_SEND_MODE_SINGLE);
    g_test_missing_link = TEST_WIFI_LINK;
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_VIDEO, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == -ENETDOWN && count == 0U);

    memset(&plan, 0, sizeof(plan));
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 3000U) == 0);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(_linkg_scheduler_build_targets(2U, (linkg_transport_class_t)index, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == -ENETDOWN);
    }
    assert(_linkg_scheduler_build_control_targets(2U, targets, &count) == 0 && count == 1U);
    assert(targets[0].link == linkg_link_manager_get(TEST_CELLULAR_LINK));
    _linkg_scheduler_release_targets(targets, count);
    assert(g_test_path_refs == 0U);
    puts("PASS T04/T17 unavailable SINGLE backup / no implicit failover / all NONE keeps control path");
}

/**
 * @brief Wi-Fi维护仅迁移受影响类别，实际BEGIN/END保留确认流程和Cellular业务。
 */
static void _audit_maintenance(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_event_t event = {0};
    linkg_transport_tx_target_t targets[LINKG_TRANSPORT_TX_TARGET_MAX];
    uint32_t                    count;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer                                      = linkg_switch_find_peer_locked(2U);
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO] = (linkg_class_send_plan_t){LINKG_SEND_MODE_SINGLE, TEST_CELLULAR_LINK, 0U};
    plan.classes[LINKG_TRANSPORT_CLASS_DATA]  = plan.classes[LINKG_TRANSPORT_CLASS_VIDEO];
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    peer->role.sta.observation.valid              = true;
    peer->role.sta.observation.wifi.available     = true;
    peer->role.sta.observation.cellular.available = true;
    for (uint32_t class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        assert(_linkg_scheduler_build_targets(2U, (linkg_transport_class_t)class_index, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == 0 && count == 1U);
        assert(targets[0].link == linkg_link_manager_get(class_index == 0U ? TEST_WIFI_LINK : TEST_CELLULAR_LINK));
        _linkg_scheduler_release_targets(targets, count);
    }
    assert(linkg_switch_begin_maintenance(LINKG_LINK_ACCESS_WIFI) == 0);
    assert(linkg_switch_access_blocked(2U, LINKG_LINK_ACCESS_WIFI));
    assert(!peer->role.sta.plan_sync.active && !peer->role.sta.plan_sync.recovery_pending);
    assert(memcmp(&plan.classes[1], &peer->plan.classes[1], sizeof(linkg_class_send_plan_t) * 2U) == 0);
    assert(peer->plan.classes[0].primary_link_id == TEST_CELLULAR_LINK);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_VIDEO, LINKG_SCHEDULER_POLICY_DEFAULT, 0U, targets, &count) == 0 && count == 1U);
    _linkg_scheduler_release_targets(targets, count);
    assert(_linkg_scheduler_build_targets(2U, LINKG_TRANSPORT_CLASS_REALTIME, LINKG_SCHEDULER_POLICY_SPECIFIED, TEST_WIFI_LINK, targets, &count) == -EBUSY);
    assert(linkg_switch_end_maintenance(LINKG_LINK_ACCESS_WIFI) == 0);
    assert(!linkg_switch_access_blocked(2U, LINKG_LINK_ACCESS_WIFI) && peer->maintenance.end_tx.active);
    event.type                           = LINKG_SWITCH_EVENT_MAINTENANCE_ACK_RX;
    event.peer_node_id                   = 2U;
    event.peer_generation                = peer->generation;
    event.message_id                     = peer->maintenance.end_tx.message_id;
    event.payload.maintenance_ack.access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    assert(linkg_switch_maintenance_process_event(&event, g_audit_now_us + 1000U) == 0 && !peer->maintenance.end_tx.active);

    event.type                       = LINKG_SWITCH_EVENT_MAINTENANCE_RX;
    event.message_id                 = 600U;
    event.payload.maintenance.access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    event.payload.maintenance.phase  = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN;
    assert(linkg_switch_maintenance_process_event(&event, g_audit_now_us + 2000U) == 0 && peer->maintenance.remote.active);
    event.payload.maintenance.phase = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END;
    uint32_t ack_count = g_audit_maintenance_ack_count;
    assert(linkg_switch_maintenance_process_event(&event, g_audit_now_us + 3000U) == 0 && !peer->maintenance.remote.active);
    assert(linkg_switch_maintenance_process_event(&event, g_audit_now_us + 4000U) == 0 && g_audit_maintenance_ack_count == ack_count + 2U);
    event.payload.maintenance.phase = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN;
    assert(linkg_switch_maintenance_process_event(&event, g_audit_now_us + 5000U) == 0 && !peer->maintenance.remote.active);
    assert(g_test_path_refs == 0U);
    puts("PASS T01/T12 Wi-Fi maintenance BEGIN/END / Cellular classes unchanged / blocked target guard / duplicate END ACK");
}

/**
 * @brief Cellular失效保留Wi-Fi类别，NONE恢复仍沿用资源校准语义。
 */
static void _audit_cellular_reconcile(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_send_plan_t expected;
    linkg_switch_reconcile_paths_t paths = {.wifi_link_id = TEST_WIFI_LINK};

    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO] = (linkg_class_send_plan_t){LINKG_SEND_MODE_SINGLE, TEST_CELLULAR_LINK, TEST_WIFI_LINK};
    plan.classes[LINKG_TRANSPORT_CLASS_DATA]  = (linkg_class_send_plan_t){LINKG_SEND_MODE_REDUNDANT, TEST_CELLULAR_LINK, TEST_WIFI_LINK};
    _linkg_switch_reconcile_build(&plan, &paths, &expected);
    assert(expected.classes[0].primary_link_id == TEST_WIFI_LINK && expected.classes[0].mode == LINKG_SEND_MODE_SINGLE);
    assert(expected.classes[1].primary_link_id == TEST_WIFI_LINK && expected.classes[1].mode == LINKG_SEND_MODE_SINGLE);
    assert(expected.classes[2].primary_link_id == TEST_WIFI_LINK && expected.classes[2].mode == LINKG_SEND_MODE_SINGLE);
    memset(&plan, 0, sizeof(plan));
    _linkg_switch_reconcile_build(&plan, &paths, &expected);
    for (uint32_t index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        assert(expected.classes[index].mode == LINKG_SEND_MODE_SINGLE && expected.classes[index].primary_link_id == TEST_WIFI_LINK);
    }
    puts("PASS T10/T11 independent unavailable-path repair / no REDUNDANT expansion / NONE recovery semantics");
}

/****************************** 状态可见性 ******************************/

/**
 * @brief 对外状态区分远端拒绝和超时未知，不再把本地更新当作双方确认。
 */
static void _audit_sync_status(void)
{
    linkg_send_plan_t plan = _audit_wifi_plan();
    linkg_switch_sta_status_t    status;
    linkg_switch_peer_runtime_t *peer;
    linkg_switch_event_t         event;
    cJSON                       *object;
    cJSON                       *sync;

    _test_reset(LINKG_DEVICE_ROLE_STA);
    peer = linkg_switch_find_peer_locked(2U);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 1000U) == 0);
    event = _audit_ack(g_test_message_id, -ETIMEDOUT);
    assert(linkg_switch_plan_process_event(&event, 2000U) == 0);
    _linkg_switch_status_build_sta_locked(&status);
    assert(status.plan_sync.recovery_pending && status.plan_sync.last_status == -ETIMEDOUT && status.plan_sync.remote_rejected && !status.plan_sync.confirmed);
    object = cJSON_CreateObject();
    assert(_linkg_web_switch_status_json_add_plan_sync(object, &status.plan_sync) == LINKG_JSON_OK);
    sync = cJSON_GetObjectItemCaseSensitive(object, "plan_sync");
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(sync, "state")->valuestring, "rejected") == 0);
    cJSON_Delete(object);
    assert(linkg_switch_plan_commit_local(2U, peer->generation, &plan, 3000U) == 0);
    assert(linkg_switch_plan_process(peer->role.sta.plan_sync.deadline_us) == 0);
    _linkg_switch_status_build_sta_locked(&status);
    assert(!status.plan_sync.remote_rejected && status.plan_sync.last_status == -ETIMEDOUT);
    object = cJSON_CreateObject();
    assert(_linkg_web_switch_status_json_add_plan_sync(object, &status.plan_sync) == LINKG_JSON_OK);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(object, "plan_sync"), "state")->valuestring, "uncertain") == 0);
    cJSON_Delete(object);
    puts("PASS sync status snapshot and JSON / rejected versus uncertain / bounded recovery visibility");
}

/**
 * @brief 执行本轮审查新增的故障与一致性回归。
 */
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    _audit_rejection_recovery();
    _audit_lost_ack();
    _audit_bounded_recovery();
    _audit_generation_and_supersession();
    _audit_conditional_commit();
    _audit_single_backup_and_none();
    _audit_maintenance();
    _audit_cellular_reconcile();
    _audit_sync_status();
    puts("All plan audit regression tests passed");
    return 0;
}
