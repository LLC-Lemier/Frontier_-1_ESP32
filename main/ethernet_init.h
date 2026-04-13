#ifndef ETHERNET_INIT_H
#define ETHERNET_INIT_H

#include <stdbool.h>
#include "esp_eth.h"
#include "esp_err.h"
#include "esp_netif.h"

extern uint8_t custom_mac[6];

// Инициализация Ethernet интерфейса
esp_err_t ethernet_init(void);

// Получение статуса Ethernet линка
bool ethernet_is_link_up(void);

// Получение MAC адреса
void ethernet_get_mac(uint8_t *mac);

// Получение Ethernet handle драйвера
esp_eth_handle_t ethernet_get_handle(void);

// Получение Ethernet netif интерфейса
esp_netif_t* ethernet_get_netif(void);

void start_dhcp_client(esp_netif_t *eth_netif);
/* Проверка статуса DHCP клиента */
void check_dhcp_status(esp_netif_t *eth_netif);

#endif