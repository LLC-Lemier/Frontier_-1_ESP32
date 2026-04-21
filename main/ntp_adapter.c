#include "ntp_adapter.h"
#include "lwip/ip4_addr.h"

bool s_is_ntp_synced = false;

void init_ntp(ntp_config_t *config) {
    if (config == NULL) {
        ESP_LOGW("SNTP", "NTP config is NULL, skip initialization");
        return;
    }

    ESP_LOGI("SNTP", "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    for (int i = 0; i < 5; i++) {
        if (config->server_type[i] == NTP_SERVER_NONE) {
            continue;
        } else if (config->server_type[i] == NTP_SERVER_DOMAIN_NAME) {
            ESP_LOGI("SNTP", "Setting NTP server %d to domain name: %s", i + 1, config->server[i]);
            esp_sntp_setservername(i, config->server[i]);
        } else if (config->server_type[i] == NTP_SERVER_IP_ADDRESS) {
            ip4_addr_t ip;
            if (ip4addr_aton(config->server[i], &ip)) {
                ESP_LOGI("SNTP", "Setting NTP server %d to IP address: %s", i + 1, config->server[i]);
                esp_sntp_setservername(i, config->server[i]);
            } else {
                ESP_LOGW("SNTP", "Invalid IP address for NTP server %d: %s", i + 1, config->server[i]);
            }
        }
    }
}

void time_sync_ntp_task(void *pvParameters) {
    s_is_ntp_synced = false;    
    esp_sntp_init();
    int retry = 0;
    const int retry_count = 50;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        //ESP_LOGI("SNTP", "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    time_t now = 0;
    struct tm timeinfo = { 0 };

    time(&now);
    localtime_r(&now, &timeinfo);
    
    
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI("SNTP", "Current date/time: %s", strftime_buf);

    s_is_ntp_synced = true;

    vTaskDelete(NULL);
}

void start_ntp_sync_task() {
    xTaskCreate(time_sync_ntp_task, "ntp_sync", 2048, NULL, 5, NULL);
}

bool is_ntp_server_synced() {
    return s_is_ntp_synced;
}   