#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ethernet_init.h"
#include "eap_tls_supplicant.h"
#include "tcp_server.h"
#include "https_server.h"
#include "network_config.h"
#include "web_api.h"

static const char *TAG = "MAIN";

extern const char ca_cert_pem_start[] asm("_binary_ca_pem_start");
extern const char ca_cert_pem_end[] asm("_binary_ca_pem_end");
extern const char client_cert_pem_start[] asm("_binary_client_crt_start");
extern const char client_cert_pem_end[] asm("_binary_client_crt_end");
extern const char client_key_pem_start[] asm("_binary_client_key_start");
extern const char client_key_pem_end[] asm("_binary_client_key_end");

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4 EAP-TLS Supplicant starting");

    ESP_ERROR_CHECK(network_config_init());
    ESP_ERROR_CHECK(ethernet_init());

    eap_tls_config_t config = {
        .identity = "user@levonik.ru\n",
        .ca_cert_pem = ca_cert_pem_start,
        .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,
        .client_cert_pem = client_cert_pem_start,
        .client_cert_len = client_cert_pem_end - client_cert_pem_start,
        .client_key_pem = client_key_pem_start,
        .client_key_len = client_key_pem_end - client_key_pem_start,
        .timeout_ms = 10000,
        .max_retries = 3,
    };

    ESP_ERROR_CHECK(eap_tls_supplicant_init(&config));
    ESP_ERROR_CHECK(eap_tls_supplicant_start());
    ESP_ERROR_CHECK(web_api_start()); // url handler

    bool services_started = false;
    while (1) {
        if (eap_tls_supplicant_is_authenticated()) {
            ESP_LOGI(TAG, "Authentication successful! Network access granted");
            if (!services_started) {
                ESP_ERROR_CHECK(network_config_apply_saved());
                xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
                ESP_ERROR_CHECK(start_https_server_task()); //
                services_started = true;
            }
        } else {
            ESP_LOGW(TAG, "Waiting for authentication...");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
