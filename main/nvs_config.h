#pragma once

#include "esp_err.h"
#include "network_config.h"

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_load_network(network_config_t *config);
esp_err_t nvs_config_save_network(const network_config_t *config);
