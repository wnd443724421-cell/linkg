/**
 * @file switch_trace.c
 * @brief STA每250ms把Wi-Fi质量摘要写入/tmp，不输出终端日志
 *
 * 仅复用Observation快照，不读取驱动、不修改Policy。
 */
#include "switch_trace.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/****************************** 诊断配置 ******************************/

#define LINKG_SWITCH_TRACE_ENABLE_FILE "/tmp/linkg_switch_trace.enable"
#define LINKG_SWITCH_TRACE_OUTPUT_FILE "/tmp/linkg_switch_wifi_250ms.log"
#define LINKG_SWITCH_TRACE_MAX_BYTES   (2U * 1024U * 1024U)
#define LINKG_SWITCH_TRACE_FLUSH_US    1000000ULL

#define LINKG_SWITCH_TRACE_UPLINK      0U
#define LINKG_SWITCH_TRACE_DOWNLINK    1U
#define LINKG_SWITCH_TRACE_DIRECTIONS  2U

/****************************** 记录状态 ******************************/

typedef struct
{
    bool     initialized;
    uint32_t count;
    uint64_t updated_us;
} linkg_switch_trace_off_base_t;

typedef struct
{
    FILE                          *file;
    size_t                         bytes_written;
    uint64_t                       last_flush_us;
    uint64_t                       loss_us[LINKG_SWITCH_TRACE_DIRECTIONS][LINKG_TRANSPORT_CLASS_COUNT];
    uint64_t                       overflow_us[LINKG_SWITCH_TRACE_DIRECTIONS];
    linkg_switch_trace_off_base_t off[LINKG_SWITCH_TRACE_DIRECTIONS];
    uint8_t                        peer_node_id;
    uint32_t                       wifi_link_id;
    bool                           source_initialized;
    bool                           stopped;
} linkg_switch_trace_runtime_t;

static linkg_switch_trace_runtime_t g_switch_trace;

/****************************** 文件控制 ******************************/

static void _linkg_switch_trace_close(void)
{
    if (g_switch_trace.file != NULL)
    {
        (void)fclose(g_switch_trace.file);
    }

    memset(&g_switch_trace, 0, sizeof(g_switch_trace));
}

static void _linkg_switch_trace_stop(void)
{
    if (g_switch_trace.file != NULL)
    {
        (void)fclose(g_switch_trace.file);
        g_switch_trace.file = NULL;
    }

    // 写入失败或达到容量上限后停止；需先删除标记，再创建才能重新启动。
    g_switch_trace.stopped = true;
}

/****************************** 单窗口差分 ******************************/

/**
 * @brief 对本方向的新HCC样本计算FLOWCTRL_OFF增量。
 *
 * 首次样本、重复报告、采样无效、计数回退统一返回未知。
 */
static void _linkg_switch_trace_off_text(linkg_switch_trace_off_base_t *base,
                                         const linkg_switch_wifi_hcc_observation_t *hcc,
                                         char output[16])
{
    (void)snprintf(output, 16U, "--");

    if (!hcc->valid || hcc->updated_us == 0U)
    {
        memset(base, 0, sizeof(*base));
        return;
    }

    if (base->initialized && hcc->updated_us <= base->updated_us)
    {
        return; // 同一AP报告重复出现在STA的下一轮观测中。
    }

    if (base->initialized && hcc->flowctrl_off_count >= base->count)
    {
        (void)snprintf(output, 16U, "%u", hcc->flowctrl_off_count - base->count);
    }

    base->initialized = true;
    base->count       = hcc->flowctrl_off_count;
    base->updated_us  = hcc->updated_us;
}

/**
 * @brief 当前窗口必须valid且updated_us是新值，否则不能重复计算丢包。
 */
static bool _linkg_switch_trace_new_loss(uint64_t *last_us,
                                         const linkg_switch_loss_observation_t *sample)
{
    if (!sample->valid || sample->updated_us == 0U || sample->sample_packets == 0U)
    {
        return false;
    }

    if (sample->updated_us <= *last_us)
    {
        return false;
    }

    *last_us = sample->updated_us;
    return true;
}

/**
 * @brief 整数计算丢包率，保留一位小数，避免浮点printf开销。
 */
static void _linkg_switch_trace_loss_text(uint64_t *last_us,
                                          const linkg_switch_loss_observation_t *sample,
                                          char output[16])
{
    uint64_t tenths;

    (void)snprintf(output, 16U, "--");

    if (!_linkg_switch_trace_new_loss(last_us, sample) ||
        sample->lost_packets > sample->sample_packets)
    {
        return;
    }

    tenths = ((uint64_t)sample->lost_packets * 1000ULL + sample->sample_packets / 2U) /
             sample->sample_packets;

    (void)snprintf(output, 16U, "%u.%u",
                   (unsigned int)(tenths / 10U),
                   (unsigned int)(tenths % 10U));
}

/**
 * @brief 格式化一次新的Path队列溢出窗口；没有新有效窗口输出--。
 */
static void _linkg_switch_trace_overflow_text(uint64_t *last_us,
                                              const linkg_switch_loss_observation_t *sample,
                                              char output[16])
{
    (void)snprintf(output, 16U, "--");

    if (_linkg_switch_trace_new_loss(last_us, sample))
    {
        (void)snprintf(output, 16U, "%u", sample->lost_packets);
    }
}

/****************************** 文件写入 ******************************/

/**
 * @brief 在Switch锁外记录本轮250ms快照，日志仅写入/tmp。
 */
void linkg_switch_trace_record(const linkg_switch_observation_t *observation)
{
    const linkg_switch_wifi_hcc_observation_t *hcc[LINKG_SWITCH_TRACE_DIRECTIONS];
    const linkg_switch_loss_observation_t *classes[LINKG_SWITCH_TRACE_DIRECTIONS];
    const linkg_switch_loss_observation_t *overflow[LINKG_SWITCH_TRACE_DIRECTIONS];
    char     line[LINKG_SWITCH_TRACE_DIRECTIONS][160];
    char     off_text[16];
    char     overflow_text[16];
    char     loss_text[LINKG_TRANSPORT_CLASS_COUNT][16];
    char     queue_text[48];
    uint32_t direction;
    uint32_t index;
    int      n;
    size_t   size;
    size_t   total_size;

    if (observation == NULL || !observation->valid)
    {
        return;
    }

    if (access(LINKG_SWITCH_TRACE_ENABLE_FILE, F_OK) != 0)
    {
        _linkg_switch_trace_close();
        return;
    }

    if (g_switch_trace.stopped)
    {
        return;
    }

    if (g_switch_trace.file == NULL)
    {
        g_switch_trace.file = fopen(LINKG_SWITCH_TRACE_OUTPUT_FILE, "w");
        if (g_switch_trace.file == NULL)
        {
            _linkg_switch_trace_stop();
            return;
        }

        (void)setvbuf(g_switch_trace.file, NULL, _IOFBF, 4096U);
        g_switch_trace.last_flush_us = observation->collected_us;
    }

    // Peer或Wi-Fi Link变更时清除差分基线，禁止把不同实例拼接。
    if (!g_switch_trace.source_initialized ||
        g_switch_trace.peer_node_id != observation->peer_node_id ||
        g_switch_trace.wifi_link_id != observation->wifi.link_id)
    {
        memset(g_switch_trace.loss_us, 0, sizeof(g_switch_trace.loss_us));
        memset(g_switch_trace.overflow_us, 0, sizeof(g_switch_trace.overflow_us));
        memset(g_switch_trace.off, 0, sizeof(g_switch_trace.off));
        g_switch_trace.peer_node_id       = observation->peer_node_id;
        g_switch_trace.wifi_link_id       = observation->wifi.link_id;
        g_switch_trace.source_initialized = true;
    }

    hcc[LINKG_SWITCH_TRACE_UPLINK]      = &observation->wifi.uplink_hcc;
    hcc[LINKG_SWITCH_TRACE_DOWNLINK]    = &observation->wifi.downlink_hcc;
    classes[LINKG_SWITCH_TRACE_UPLINK]   = observation->wifi.loss.uplink_classes;
    classes[LINKG_SWITCH_TRACE_DOWNLINK] = observation->wifi.loss.downlink_classes;
    overflow[LINKG_SWITCH_TRACE_UPLINK]   = &observation->wifi.loss.uplink_load;
    overflow[LINKG_SWITCH_TRACE_DOWNLINK] = &observation->wifi.loss.downlink_load;

    total_size = 0U;

    for (direction = 0U; direction < LINKG_SWITCH_TRACE_DIRECTIONS; direction++)
    {
        _linkg_switch_trace_off_text(&g_switch_trace.off[direction], hcc[direction], off_text);
        _linkg_switch_trace_overflow_text(&g_switch_trace.overflow_us[direction],
                                          overflow[direction], overflow_text);

        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            _linkg_switch_trace_loss_text(&g_switch_trace.loss_us[direction][index],
                                          &classes[direction][index], loss_text[index]);
        }

        if (hcc[direction]->valid)
        {
            n = snprintf(queue_text, sizeof(queue_text), "%u/%u/%u",
                         hcc[direction]->vo_queue_length,
                         hcc[direction]->vi_queue_length,
                         hcc[direction]->be_queue_length);
        }
        else
        {
            n = snprintf(queue_text, sizeof(queue_text), "--/--/--");
        }

        if (n < 0 || (size_t)n >= sizeof(queue_text))
        {
            _linkg_switch_trace_stop();
            return;
        }

        n = snprintf(line[direction], sizeof(line[direction]),
                     "%s Q=%s  OFF=%s  OVF=%s  LOSS=%s/%s/%s%s\n",
                     direction == LINKG_SWITCH_TRACE_UPLINK ? "UL" : "DL",
                     queue_text, off_text, overflow_text,
                     loss_text[0], loss_text[1], loss_text[2],
                     strcmp(loss_text[0], "--") == 0 &&
                     strcmp(loss_text[1], "--") == 0 &&
                     strcmp(loss_text[2], "--") == 0 ? "" : "%");
        if (n < 0 || (size_t)n >= sizeof(line[direction]))
        {
            _linkg_switch_trace_stop();
            return;
        }

        total_size += (size_t)n;
    }

    if (g_switch_trace.bytes_written + total_size + 1U > LINKG_SWITCH_TRACE_MAX_BYTES)
    {
        _linkg_switch_trace_stop();
        return;
    }

    for (direction = 0U; direction < LINKG_SWITCH_TRACE_DIRECTIONS; direction++)
    {
        size = strlen(line[direction]);
        if (fwrite(line[direction], 1U, size, g_switch_trace.file) != size)
        {
            _linkg_switch_trace_stop();
            return;
        }
    }

    if (fputc('\n', g_switch_trace.file) == EOF)
    {
        _linkg_switch_trace_stop();
        return;
    }

    g_switch_trace.bytes_written += total_size + 1U;

    // 每秒flush一次；关闭标记后下一轮fclose也会刷新剩余缓冲。
    if (observation->collected_us >= g_switch_trace.last_flush_us &&
        observation->collected_us - g_switch_trace.last_flush_us >= LINKG_SWITCH_TRACE_FLUSH_US)
    {
        if (fflush(g_switch_trace.file) != 0)
        {
            _linkg_switch_trace_stop();
            return;
        }

        g_switch_trace.last_flush_us = observation->collected_us;
    }
}
