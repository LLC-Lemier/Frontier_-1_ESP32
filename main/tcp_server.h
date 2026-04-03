#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#define PORT 8080

static const char *TCP_TAG = "tcp_server";

/* Задача TCP-сервера */
static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    if (listen_sock < 0) {
        ESP_LOGE(TCP_TAG, "Ошибка создания сокета: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TCP_TAG, "Ошибка привязки сокета: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TCP_TAG, "Ошибка прослушивания: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TCP_TAG, "TCP сервер запущен на порту %d", PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        
        if (sock < 0) {
            ESP_LOGE(TCP_TAG, "Ошибка принятия соединения: errno %d", errno);
            break;
        }

        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TCP_TAG, "Новое подключение от %s, сокет fd: %d", addr_str, sock);

        char rx_buffer[128];
        int len;
        
        while ((len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0)) > 0) {
            rx_buffer[len] = 0;
            ESP_LOGI(TCP_TAG, "Получено %d байт: %s", len, rx_buffer);
            
            const char *response = "Привет от ESP32-P4 TCP сервера!\r\n";
            send(sock, response, strlen(response), 0);
        }

        if (len < 0) {
            ESP_LOGE(TCP_TAG, "Ошибка приема: errno %d", errno);
        }
        
        shutdown(sock, 0);
        close(sock);
    }

    close(listen_sock);
    vTaskDelete(NULL);
}
