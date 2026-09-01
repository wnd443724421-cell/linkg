#ifndef AT_CHANNEL_H
#define AT_CHANNEL_H
#include <stdbool.h>
#include <stdint.h>
#include "linkg_uart.h"
typedef struct at_channel at_channel_t;
typedef void (*at_urc_callback_t)(const char *line, void *context);
typedef bool (*at_response_continuation_match_t)(const char *line);
typedef struct
{
    int                              timeout_ms;
    const char                      *expect_prefix;
    bool                             accept_plain_text;
    at_response_continuation_match_t continuation_match;
    uint8_t                          continuation_max_lines;
} at_command_config_t;
at_channel_t *at_channel_create(const char *device, const uart_config_t *config);
int           at_channel_start(at_channel_t *channel);
int           at_channel_stop(at_channel_t *channel);
void          at_channel_destroy(at_channel_t *channel);
int           at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int response_size);
int           at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context);
#endif
