#include "cellular_monitor.h"

#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t g_now_ms;

int at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context)
{
    if (channel == NULL)
    {
        return -1;
    }

    channel->callback = callback;
    channel->context  = context;

    return 0;
}

uint64_t linkg_time_elapsed_ms(void)
{
    return g_now_ms;
}

static void feed(at_channel_t *channel, const char *line, uint64_t now_ms)
{
    g_now_ms = now_ms;
    assert(channel->callback != NULL);
    channel->callback(line, channel->context);
}

int main(void)
{
    cellular_monitor_events_t events;
    struct pollfd             descriptor;
    at_urc_callback_t         old_callback;
    void                     *old_context;
    at_channel_t              channel;
    at_channel_t              next_channel;
    int                       event_fd;
    int                       ret;

    memset(&channel, 0, sizeof(channel));
    memset(&next_channel, 0, sizeof(next_channel));

    assert(cellular_monitor_init() == 0);
    assert(cellular_monitor_start(&channel) == 0);

    event_fd = cellular_monitor_get_event_fd();
    assert(event_fd >= 0);

    feed(&channel, "+QSIMSTAT: 1,1", 100U);
    feed(&channel, "+CPIN: READY", 101U);
    feed(&channel, "+CEREG: 1", 102U);
    feed(&channel, "+C5GREG: 1", 103U);
    feed(&channel, "+QCSQ: \"NR5G\",-85,115,-13", 104U);
    feed(&channel, "+CGEV: ME PDN ACT 1", 105U);
    feed(&channel, "+QNETDEVSTATUS: 1", 106U);
    feed(&channel, "+CFUN: 1", 107U);

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd     = event_fd;
    descriptor.events = POLLIN;

    ret = poll(&descriptor, 1U, 0);
    assert(ret == 1);
    assert((descriptor.revents & POLLIN) != 0);

    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.mask == (CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED |
                           CELLULAR_MONITOR_EVENT_SIM_STATE_CHANGED |
                           CELLULAR_MONITOR_EVENT_REGISTRATION_CHANGED |
                           CELLULAR_MONITOR_EVENT_RADIO_CHANGED |
                           CELLULAR_MONITOR_EVENT_PDP_CHANGED |
                           CELLULAR_MONITOR_EVENT_NETDEV_CHANGED |
                           CELLULAR_MONITOR_EVENT_MODEM_FUNCTION_CHANGED));
    assert(events.sim_presence_valid);
    assert(events.sim_presence == CELLULAR_MONITOR_SIM_PRESENCE_INSERTED);
    assert(events.generation == 8U);
    assert(events.updated_ms == 107U);
    assert(events.sim_presence_updated_ms == 100U);

    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.mask == CELLULAR_MONITOR_EVENT_NONE);

    feed(&channel, "+QSIMSTAT : 1,0", 200U);
    feed(&channel, "POWERED DOWN", 201U);
    assert(cellular_monitor_take_events(&events) == 0);
    assert((events.mask & CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED) != 0U);
    assert((events.mask & CELLULAR_MONITOR_EVENT_MODEM_POWERED_DOWN) != 0U);
    assert(events.sim_presence_valid);
    assert(events.sim_presence == CELLULAR_MONITOR_SIM_PRESENCE_REMOVED);

    feed(&channel, "+QSIMSTAT: 1,2", 300U);
    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.sim_presence == CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN);
    assert(!events.sim_presence_valid);

    feed(&channel, "+QSIMSTAT: bad", 400U);
    assert(cellular_monitor_take_events(&events) == 0);
    assert((events.mask & CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED) != 0U);
    assert(!events.sim_presence_valid);

    feed(&channel, "+UNKNOWN: 1", 500U);
    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.mask == CELLULAR_MONITOR_EVENT_NONE);

    old_callback = channel.callback;
    old_context  = channel.context;

    assert(cellular_monitor_stop() == 0);
    assert(channel.callback == NULL);
    assert(cellular_monitor_get_event_fd() < 0);

    assert(cellular_monitor_start(&next_channel) == 0);
    assert(old_callback != NULL);
    old_callback("+QCSQ: \"LTE\",-60,-95,100,-14", old_context);
    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.mask == CELLULAR_MONITOR_EVENT_NONE);

    feed(&next_channel, "+QCSQ: \"LTE\",-60,-95,100,-14", 600U);
    assert(cellular_monitor_take_events(&events) == 0);
    assert(events.mask == CELLULAR_MONITOR_EVENT_RADIO_CHANGED);

    assert(cellular_monitor_stop() == 0);
    cellular_monitor_deinit();

    puts("cellular_monitor tests passed");

    return 0;
}
