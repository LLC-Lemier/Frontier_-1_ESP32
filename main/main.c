#include <stdio.h>                                    
#include "freertos/FreeRTOS.h"                        // Подключаем базовые типы и макросы FreeRTOS;  tick-макросы.
#include "freertos/task.h"                            // API задач FreeRTOS; таски и dealys
#include "esp_log.h"                                  
#include "ethernet_init.h"                            //
#include "https_server.h"                             
#include "network_config.h"                           // модуль сетевой конфигурации; он читает/сохраняет DHCP/static-настройки и применяет их к netif.
#include "web_api.h"                                  // внутренний web API dispatcher
#include "eap_tls_supplicant.h"

static const char *TAG = "MAIN";                     

extern const char ca_cert_pem_start[] asm("_binary_ca_pem_start");             
extern const char ca_cert_pem_end[] asm("_binary_ca_pem_end");                 
extern const char client_cert_pem_start[] asm("_binary_client_crt_start");     
extern const char client_cert_pem_end[] asm("_binary_client_crt_end");         
extern const char client_key_pem_start[] asm("_binary_client_key_start");      
extern const char client_key_pem_end[] asm("_binary_client_key_end");          

void app_main(void)                                                            
{
    ESP_LOGI(TAG, "ESP32-P4-Nano start: Ethernet + Web UI + background EAP-TLS"); 

    ESP_ERROR_CHECK(network_config_init());                                     // модуль сетевой конфигурации; сохранённые значения из NVS
                                                                                 

    ESP_ERROR_CHECK(ethernet_init());                                           // Ethernet-стек, netif, драйвер MAC/PHY и запуск интерфейса. инициализация сетевых параметров

    ESP_ERROR_CHECK(network_config_apply_saved());                              // применяем сетевые параметры
                                                                                // устройство получает режим DHCP/static независимо от наличия RADIUS.


    ESP_ERROR_CHECK(web_api_start());                                           //


    ESP_ERROR_CHECK(start_https_server_task());                                 //

    eap_tls_config_t config = {                                                 
        .identity = "user@levonik.ru\n",                                        
                                                                                 
        .ca_cert_pem = ca_cert_pem_start,                                       
        .ca_cert_len = ca_cert_pem_end - ca_cert_pem_start,              //                 
        .client_cert_pem = client_cert_pem_start,                               
        .client_cert_len = client_cert_pem_end - client_cert_pem_start,         
        .client_key_pem = client_key_pem_start,                                 
        .client_key_len = client_key_pem_end - client_key_pem_start,            
        .timeout_ms = 10000,                                                    
                                                                                 
        .max_retries = 3,                                                       
                                                                                 
    };

    ESP_ERROR_CHECK(eap_tls_supplicant_init(&config));                          


    ESP_ERROR_CHECK(eap_tls_supplicant_start());                                


    while (1) {                                                                 
        if (ethernet_is_link_up()) {                                            //проверяем физическое состояние 
            if (eap_tls_supplicant_is_authenticated()) {                        // chek eap-tls
                ESP_LOGI(TAG, "Status: Ethernet UP, HTTPS running, EAP-TLS authenticated"); // состояние "все работает"

            } else {                                                            
                ESP_LOGW(TAG, "Status: Ethernet UP, HTTPS running, RADIUS no detected"); // сценарий "если линк есть, но аутентификация пока не подтверждена"
            }

        } else {                                                                
            ESP_LOGW(TAG, "Status: Ethernet DOWN, HTTPS server started but peer access depends on link/IP");   // нет линка
        }

        vTaskDelay(pdMS_TO_TICKS(5000));                                       
    }
}