#ifndef AT_CHANNEL_H
#define AT_CHANNEL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct at_channel at_channel_t;
typedef void (*at_urc_callback_t)(const char *line, void *context);
typedef bool (*at_response_continuation_match_t)(const char *line);
struct at_channel
{
    at_urc_callback_t callback;
    void             *context;
};
typedef struct
{
    int                              timeout_ms;
    const char                      *expect_prefix;
    bool                             accept_plain_text;
    at_response_continuation_match_t continuation_match;
    uint8_t                          continuation_max_lines;
} at_command_config_t;
int at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context);
#endif
