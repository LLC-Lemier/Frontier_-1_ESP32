#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WEB_API_CMD_GET_STATUS = 1,
    WEB_API_CMD_GET_NETWORK_CONFIG,
    WEB_API_CMD_PUT_NETWORK_CONFIG,
    WEB_API_CMD_REBOOT,
} web_api_cmd_type_t;

typedef struct {
    bool dhcp_enabled;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns1;
    uint32_t dns2;
} web_api_network_payload_t;

typedef struct {
    web_api_cmd_type_t type;
    uint32_t request_id;
    web_api_network_payload_t network;
} web_api_request_t;

typedef struct {
    esp_err_t status;
    uint32_t request_id;
    int http_status;
    char content_type[32];
    char body[512];
    bool schedule_reboot;
} web_api_response_t;

esp_err_t web_api_start(void);
esp_err_t web_api_call(const web_api_request_t *request, web_api_response_t *response, uint32_t timeout_ms);
