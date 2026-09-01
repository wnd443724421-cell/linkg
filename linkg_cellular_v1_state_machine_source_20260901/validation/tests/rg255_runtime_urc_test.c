#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "rg255_runtime_urc.h"
struct at_channel { int dummy; };
static char g_command[64];
int at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int response_size)
{
    assert(channel != NULL);
    assert(command != NULL);
    assert(config != NULL);
    assert(config->timeout_ms > 0);
    assert(response == NULL);
    assert(response_size == 0);
    strcpy(g_command, command);
    return 0;
}
int main(void)
{
    struct at_channel channel;
    assert(rg255_cmd_set_eps_registration_urc(&channel, true) == 0);
    assert(strcmp(g_command, "AT+CEREG=1") == 0);
    assert(rg255_cmd_set_5g_registration_urc(&channel, false) == 0);
    assert(strcmp(g_command, "AT+C5GREG=0") == 0);
    assert(rg255_cmd_set_signal_urc(&channel, true) == 0);
    assert(strcmp(g_command, "AT+QCSQ=1") == 0);
    return 0;
}
