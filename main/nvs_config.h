#pragma once

#include "eap_tls_supplicant.h"
#include "esp_err.h"
#include "network_config.h"

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_load_network(
    network_config_t *config,
    eap_tls_config_t *eap_tls_config,
    ntp_config_t *ntp_config
);
esp_err_t nvs_config_save_network(
    const network_config_t *config,
    const eap_tls_config_t *eap_tls_config,
    const ntp_config_t *ntp_config
);