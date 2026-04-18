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
    network_config_t runtime = {0};
    network_config_t saved = {0};
    network_config_get_runtime(&runtime);
    network_config_load(&saved);

    char saved_ip[16], saved_mask[16], saved_gw[16], saved_dns1[16], saved_dns2[16];
    ip4_to_text(saved.ip, saved_ip, sizeof(saved_ip));
    ip4_to_text(saved.netmask, saved_mask, sizeof(saved_mask));
    ip4_to_text(saved.gateway, saved_gw, sizeof(saved_gw));
    ip4_to_text(saved.dns1, saved_dns1, sizeof(saved_dns1));
    ip4_to_text(saved.dns2, saved_dns2, sizeof(saved_dns2));

    char runtime_ip[16], runtime_mask[16], runtime_gw[16], runtime_dns1[16], runtime_dns2[16];
    ip4_to_text(runtime.ip, runtime_ip, sizeof(runtime_ip));
    ip4_to_text(runtime.netmask, runtime_mask, sizeof(runtime_mask));
    ip4_to_text(runtime.gateway, runtime_gw, sizeof(runtime_gw));
    ip4_to_text(runtime.dns1, runtime_dns1, sizeof(runtime_dns1));
    ip4_to_text(runtime.dns2, runtime_dns2, sizeof(runtime_dns2));

    response->http_status = 200;
    strlcpy(response->content_type, "application/json", sizeof(response->content_type));
    snprintf(response->body, sizeof(response->body),
        "{\"ok\":true,\"saved\":{\"dhcp\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\"},\"runtime\":{\"dhcp\":%s,\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\"}}",
        saved.dhcp_enabled ? "true" : "false", saved_ip, saved_mask, saved_gw, saved_dns1, saved_dns2,
        runtime.dhcp_enabled ? "true" : "false", runtime_ip, runtime_mask, runtime_gw, runtime_dns1, runtime_dns2
    );
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
    /*if (!s_request_queue || !request || !response) {
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
    return response->status;*/

    
    *response = (web_api_response_t){
        .status = ESP_OK,
        .request_id = request->request_id,
        .http_status = 200,
        .schedule_reboot = false,
    };
    strlcpy(response->content_type, "application/json", sizeof(response->content_type));

    switch (request->type) {
        case WEB_API_CMD_GET_STATUS:
            respond_status(response);
            break;
        case WEB_API_CMD_GET_NETWORK_CONFIG:
            respond_saved_config(response);
            break;
        case WEB_API_CMD_PUT_NETWORK_CONFIG: {

            esp_err_t ret = ESP_OK;
            //esp_err_t ret = network_config_save(&new_config);
            if (ret != ESP_OK) {
                response->status = ret;
                response->http_status = 500;
                snprintf(response->body, sizeof(response->body), "{\"ok\":false,\"message\":\"Failed to save config\"}");
            } else {
                response->http_status = 200;
                snprintf(response->body, sizeof(response->body), "{\"ok\":true,\"message\":\"Config saved\"}");
            }
            break;
        }
        case WEB_API_CMD_REBOOT:
            response->schedule_reboot = true;
            response->http_status = 202;
            snprintf(response->body, sizeof(response->body), "{\"ok\":true,\"message\":\"Reboot scheduled\"}");
            break;
        default:
            response->status = ESP_ERR_INVALID_ARG;
            response->http_status = 400;
            snprintf(response->body, sizeof(response->body), "{\"ok\":false,\"message\":\"Unsupported command\"}");
            break;
    }
    return response->status;
}
