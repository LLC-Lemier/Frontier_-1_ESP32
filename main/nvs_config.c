#include "nvs_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "NVS_CFG";
static const char *NAMESPACE = "netcfg";

esp_err_t nvs_config_init(void)
{
    esp_err_t ret = nvs_flash_init(); // инициализация
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); // чек ошибки и перезапись
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t nvs_config_load_network(network_config_t *config, eap_tls_config_t* eap_tls_config) // заполняем структуру конфигом из NVS
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config)); // ОЗУ (SRAM)  может хранить мусор
    config->dhcp_enabled = true;
    config->eap_tls_config = NULL; // по умолчанию указатель на EAP-TLS конфиг не указывает ни на что 
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Network config not found in NVS, using DHCP defaults");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_open failed");

    uint8_t is_eap = 0;
    ret = nvs_get_u8(handle, "is_eap", &is_eap);

    if (is_eap != 0) {
        config->eap_tls_config = eap_tls_config;

        (void)nvs_get_u32(handle, "eap_timeout_ms", &eap_tls_config->timeout_ms);
        (void)nvs_get_u8(handle, "eap_max_retries", &eap_tls_config->max_retries);
        
    }

    uint8_t is_static = 1;
    ret = nvs_get_u8(handle, "is_static", &is_static);
    if (ret == ESP_OK) {
        config->dhcp_enabled = (is_static != 1);
    }

    (void)nvs_get_u32(handle, "ip", &config->ip);
    (void)nvs_get_u32(handle, "mask", &config->netmask);
    (void)nvs_get_u32(handle, "gw", &config->gateway);
    (void)nvs_get_u32(handle, "dns1", &config->dns1);
    (void)nvs_get_u32(handle, "dns2", &config->dns2);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_config_save_network(const network_config_t *config, const eap_tls_config_t* eap_tls_config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open failed");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "dhcp", config->dhcp_enabled ? 1 : 0), exit, TAG, "save dhcp failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "ip", config->ip), exit, TAG, "save ip failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "mask", config->netmask), exit, TAG, "save mask failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "gw", config->gateway), exit, TAG, "save gw failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "dns1", config->dns1), exit, TAG, "save dns1 failed");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "dns2", config->dns2), exit, TAG, "save dns2 failed");

    if (eap_tls_config) {
        ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "is_eap", 1), exit, TAG, "save is_eap failed");
        ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "eap_timeout_ms", eap_tls_config->timeout_ms), exit, TAG, "save eap_timeout_ms failed");
        ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "eap_max_retries", eap_tls_config->max_retries), exit, TAG, "save eap_max_retries failed");
    } else {
        ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "is_eap", 0), exit, TAG, "save is_eap failed");
    }
    ESP_GOTO_ON_ERROR(nvs_commit(handle), exit, TAG, "nvs_commit failed");

exit:
    nvs_close(handle);
    return ret;
}
