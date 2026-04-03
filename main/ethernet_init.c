#include "ethernet_init.h"
#include "esp_compiler.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "eap_tls_supplicant.h"


static const char *TAG = "ETHERNET";
static esp_eth_handle_t eth_handle = NULL;
static bool s_link_up = false;
static esp_netif_t *s_eth_netif = NULL;



esp_err_t esp_netif_receive_eapol(esp_eth_handle_t hdl, uint8_t *buffer, uint32_t length, void *priv)
{
    if (length > 14) {
        uint16_t ethertype = (buffer[12] << 8) | buffer[13];
        
        if (ethertype == 0x888E) {
            // Обрабатываем EAPoL
            //eapol_input_callback(buffer, length);
            ESP_LOGI(TAG, "Received EAPoL frame, length: %u", length);
            ESP_LOG_BUFFER_HEX(TAG, buffer, length);
            eap_frame_handler(buffer, length, priv);
            free(buffer);
            return ESP_OK;  // Кадр обработан, не передаем дальше
        }
    }
    ESP_LOGI(TAG, "Received frame, length: %u", length);
    ESP_LOG_BUFFER_HEX(TAG, buffer, length);
            
    // Передаем в TCP/IP стек через esp_netif_receive
    return esp_netif_receive(s_eth_netif, buffer, length, NULL);
}

// Обработчик событий Ethernet
static void ethernet_event_handler(
    void *arg, 
    esp_event_base_t event_base,
    int32_t event_id, 
    void *event_data
) {

    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                ESP_LOGI(TAG, "Ethernet connected");
                s_link_up = true;
                break;
            case ETHERNET_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "Ethernet disconnected");
                s_link_up = false;
                break;
            case ETHERNET_EVENT_START:
                ESP_LOGI(TAG, "Ethernet started");
                break;
            case ETHERNET_EVENT_STOP:
                ESP_LOGI(TAG, "Ethernet stopped");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_ETH_GOT_IP:
                s_link_up = true;
                ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
                break;
            case IP_EVENT_ETH_LOST_IP:
                s_link_up = false;
                ESP_LOGI(TAG, "Lost IP");
                break;
            default:
                break;
        }
    }
}

esp_err_t ethernet_set_mac(esp_eth_handle_t eth_handle, uint8_t *mac) {
    return esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac);
}

esp_err_t ethernet_init(void) {
    ESP_LOGI(TAG, "Initializing Ethernet");
    
    // Инициализация сетевого стека
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Создание Ethernet интерфейса
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);

    // 2. Создаём конфигурацию для ESP32 EMAC

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /*esp32_emac_config.smi_mdc_gpio_num = GPIO_NUM_31;
    esp32_emac_config.smi_mdio_gpio_num = GPIO_NUM_52;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = GPIO_NUM_50;*/

    // Конфигурация PHY (LAN8720 для примера)
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;                          // KEY FIX: address 1, not 0
    //phy_config.reset_gpio_num = GPIO_NUM_51;          // Power control
    phy_config.autonego_timeout_ms = 1000;            // Allow time for negotiation
    // Конфигурация MAC для ESP32-P4 RMII
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096; // настройка размера стека    

    // Установка MAC адреса из efuse
    //uint8_t mac[6];
    //esp_efuse_mac_get_default(mac);
    //mac_config.mac = mac;
    
    // Создание драйвера Ethernet
    esp_eth_mac_t *mac_driver = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy_driver = esp_eth_phy_new_ip101(&phy_config);
    
    // Initialize Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac_driver, phy_driver);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    esp_err_t ret = ethernet_set_mac(eth_handle, custom_mac);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MAC address set successfully");
    }
    // Подключение к сетевому интерфейсу
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle)));
    
    ret = esp_eth_update_input_path(eth_handle, esp_netif_receive_eapol, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update input path: %d, EAPoL may not work", ret);
    } else {
        ESP_LOGI(TAG, "Custom input path installed successfully");
    }
    
    // Регистрация обработчиков событий
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                              &ethernet_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                              &ethernet_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                              &ethernet_event_handler, NULL));
    
    // Запуск Ethernet
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    return ESP_OK;
}

bool ethernet_is_link_up(void) {
    return s_link_up;
}

void ethernet_get_mac(uint8_t *mac) {
    if (s_eth_netif) {
        esp_netif_get_mac(s_eth_netif, mac);
    }
}

esp_eth_handle_t ethernet_get_handle(void) {
    return eth_handle;
}
esp_netif_t* ethernet_get_netif(void) {
    return s_eth_netif;
}
void start_dhcp_client(esp_netif_t *eth_netif)
{
    esp_err_t ret = esp_netif_dhcpc_start(eth_netif);
    if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGI(TAG, "DHCP клиент уже запущен");
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DHCP клиент успешно запущен");
    } else {
        ESP_LOGE(TAG, "Ошибка запуска DHCP клиента: %s", esp_err_to_name(ret));
    }
}

/* Проверка статуса DHCP клиента */
void check_dhcp_status(esp_netif_t *eth_netif)
{
    esp_netif_dhcp_status_t status;
    esp_err_t ret = esp_netif_dhcpc_get_status(eth_netif, &status);
    if (ret == ESP_OK) {
        switch (status) {
            case ESP_NETIF_DHCP_STOPPED:
                ESP_LOGI(TAG, "DHCP клиент остановлен");
                break;
            case ESP_NETIF_DHCP_STARTED:
                ESP_LOGI(TAG, "DHCP клиент запущен и выполняет обнаружение");
                break;
            default:
                break;
        }
    }
}
