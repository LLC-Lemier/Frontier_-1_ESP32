#include "web_api.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "network_config.h"
#include "ethernet_init.h"
#include "eap_tls_supplicant.h"

typedef struct {
    web_api_request_t request;
    QueueHandle_t reply_queue;
} web_api_message_t;

static const char *TAG = "WEB_API";
static QueueHandle_t s_request_queue;

static void ip4_to_text(uint32_t addr, char *buf, size_t len)
{
    esp_ip4_addr_t ip = {.addr = addr};
    snprintf(buf, len, IPSTR, IP2STR(&ip));
}

static void respond_status(web_api_response_t *response)
{
    network_config_t runtime = {0};
    network_config_t saved = {0};
    network_config_get_runtime(&runtime);
    network_config_load(&saved);

    char ip[16], mask[16], gw[16], dns1[16], dns2[16];
    ip4_to_text(runtime.ip, ip, sizeof(ip));
    ip4_to_text(runtime.netmask, mask, sizeof(mask));
    ip4_to_text(runtime.gateway, gw, sizeof(gw));
    ip4_to_text(runtime.dns1, dns1, sizeof(dns1));
    ip4_to_text(runtime.dns2, dns2, sizeof(dns2));

    response->http_status = 200;
    strlcpy(response->content_type, "application/json", sizeof(response->content_type));
    snprintf(response->body, sizeof(response->body),
             "{\"ok\":true,\"auth\":%s,\"link_up\":%s,\"runtime\":{\"dhcp\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\"},\"saved\":{\"dhcp\":%s}}",
             eap_tls_supplicant_is_authenticated() ? "true" : "false",
             ethernet_is_link_up() ? "true" : "false",
             runtime.dhcp_enabled ? "true" : "false",
             ip, mask, gw, dns1, dns2,
             saved.dhcp_enabled ? "true" : "false");
}

static void respond_saved_config(web_api_response_t *response)
{
    network_config_t saved = {0};
    network_config_load(&saved);

    char ip[16], mask[16], gw[16], dns1[16], dns2[16];
    ip4_to_text(saved.ip, ip, sizeof(ip));
    ip4_to_text(saved.netmask, mask, sizeof(mask));
    ip4_to_text(saved.gateway, gw, sizeof(gw));
    ip4_to_text(saved.dns1, dns1, sizeof(dns1));
    ip4_to_text(saved.dns2, dns2, sizeof(dns2));

    response->http_status = 200;
    strlcpy(response->content_type, "application/json", sizeof(response->content_type));
    snprintf(response->body, sizeof(response->body),
             "{\"ok\":true,\"dhcp\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\"}",
             saved.dhcp_enabled ? "true" : "false", ip, mask, gw, dns1, dns2);
}

static void reboot_delayed_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static void web_api_task(void *arg)
{
    web_api_message_t message;
    while (1) {
        if (xQueueReceive(s_request_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        web_api_response_t response = {
            .status = ESP_OK,
            .request_id = message.request.request_id,
            .http_status = 200,
            .schedule_reboot = false,
        };
        strlcpy(response.content_type, "application/json", sizeof(response.content_type));

        switch (message.request.type) {
            case WEB_API_CMD_GET_STATUS:
                respond_status(&response);
                break;
            case WEB_API_CMD_GET_NETWORK_CONFIG:
                respond_saved_config(&response);
                break;
            case WEB_API_CMD_SET_DHCP: {
                network_config_t cfg = {.dhcp_enabled = true};
                response.status = network_config_save(&cfg);
                response.schedule_reboot = (response.status == ESP_OK);
                snprintf(response.body, sizeof(response.body),
                         response.status == ESP_OK
                            ? "{\"ok\":true,\"message\":\"DHCP config saved, device will reboot\"}"
                            : "{\"ok\":false,\"message\":\"failed to save DHCP config\"}");
                response.http_status = (response.status == ESP_OK) ? 202 : 500;
                break;
            }
            case WEB_API_CMD_SET_STATIC: {
                network_config_t cfg = {
                    .dhcp_enabled = false,
                    .ip = message.request.network.ip,
                    .netmask = message.request.network.netmask,
                    .gateway = message.request.network.gateway,
                    .dns1 = message.request.network.dns1,
                    .dns2 = message.request.network.dns2,
                };
                response.status = network_config_save(&cfg);
                response.schedule_reboot = (response.status == ESP_OK);
                snprintf(response.body, sizeof(response.body),
                         response.status == ESP_OK
                            ? "{\"ok\":true,\"message\":\"Static config saved, device will reboot\"}"
                            : "{\"ok\":false,\"message\":\"failed to save static config\"}");
                response.http_status = (response.status == ESP_OK) ? 202 : 500;
                break;
            }
            case WEB_API_CMD_REBOOT:
                response.schedule_reboot = true;
                response.http_status = 202;
                snprintf(response.body, sizeof(response.body), "{\"ok\":true,\"message\":\"Reboot scheduled\"}");
                break;
            default:
                response.status = ESP_ERR_INVALID_ARG;
                response.http_status = 400;
                snprintf(response.body, sizeof(response.body), "{\"ok\":false,\"message\":\"Unsupported command\"}");
                break;
        }

        if (message.reply_queue) {
            xQueueSend(message.reply_queue, &response, pdMS_TO_TICKS(100));
        }
        if (response.schedule_reboot) {
            xTaskCreate(reboot_delayed_task, "reboot_delayed", 2048, NULL, 4, NULL);
        }
    }
}

esp_err_t web_api_start(void)
{
    if (s_request_queue) {
        return ESP_OK;
    }

    s_request_queue = xQueueCreate(8, sizeof(web_api_message_t));
    if (!s_request_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(web_api_task, "web_api", 6144, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t web_api_call(const web_api_request_t *request, web_api_response_t *response, uint32_t timeout_ms)
{
    if (!s_request_queue || !request || !response) {
        return ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t reply_queue = xQueueCreate(1, sizeof(web_api_response_t));
    if (!reply_queue) {
        return ESP_ERR_NO_MEM;
    }

    web_api_message_t message = {
        .request = *request,
        .reply_queue = reply_queue,
    };

    if (xQueueSend(s_request_queue, &message, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        vQueueDelete(reply_queue);
        return ESP_ERR_TIMEOUT;
    }

    if (xQueueReceive(reply_queue, response, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        vQueueDelete(reply_queue);
        return ESP_ERR_TIMEOUT;
    }

    vQueueDelete(reply_queue);
    return response->status;
}
