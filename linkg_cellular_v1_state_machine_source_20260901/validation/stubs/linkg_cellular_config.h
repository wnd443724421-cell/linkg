#ifndef LINKG_CELLULAR_CONFIG_H
#define LINKG_CELLULAR_CONFIG_H
#include <stdbool.h>
#define LINKG_CELLULAR_APN_MAX 100U
#define LINKG_CELLULAR_PIN_MIN 4U
#define LINKG_CELLULAR_PIN_MAX 8U
typedef enum
{
    LINKG_CELLULAR_NETWORK_MODE_UNKNOWN = 0,
    LINKG_CELLULAR_NETWORK_MODE_AUTO,
    LINKG_CELLULAR_NETWORK_MODE_4G,
    LINKG_CELLULAR_NETWORK_MODE_5G
} linkg_cellular_network_mode_t;
typedef struct
{
    bool                          enabled;
    linkg_cellular_network_mode_t network_mode;
    char                          apn[LINKG_CELLULAR_APN_MAX + 1U];
    char                          pin[LINKG_CELLULAR_PIN_MAX + 1U];
} linkg_cellular_config_t;
int linkg_cellular_config_validate(const linkg_cellular_config_t *config);
#endif
