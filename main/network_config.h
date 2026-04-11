#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "eap_tls_supplicant.h"

typedef struct {
    bool dhcp_enabled;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns1;
    uint32_t dns2;
    eap_tls_config_t* eap_tls_config;
} network_config_t;


esp_err_t network_config_init(void);
esp_err_t network_config_load(network_config_t *config);
esp_err_t network_config_save(const network_config_t *config);
esp_err_t network_config_apply_saved(void);
esp_err_t network_config_apply(const network_config_t *config);
esp_err_t network_config_get_runtime(network_config_t *config);
network_config_t* network_config_get_saved(void);
