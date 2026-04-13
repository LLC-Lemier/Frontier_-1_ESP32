#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WEB_API_CMD_GET_STATUS = 1,
    WEB_API_CMD_GET_NETWORK_CONFIG,
    WEB_API_CMD_SET_DHCP,
    WEB_API_CMD_SET_STATIC,
    WEB_API_CMD_REBOOT,
    WEB_API_CMD_AUTH_LOGIN,
    WEB_API_CMD_AUTH_LOGOUT,
    WEB_API_CMD_AUTH_CHANGE_PASSWORD,
    WEB_API_CMD_GET_SETTINGS_NETWORK,
    WEB_API_CMD_GET_SETTINGS_RS232,
    WEB_API_CMD_GET_SETTINGS_GATEWAY,
    WEB_API_CMD_GET_DASHBOARD,
    WEB_API_CMD_GET_DASHBOARD_LOGS,
    WEB_API_CMD_GET_USER,
} web_api_cmd_type_t;

typedef struct {
    bool dhcp_enabled;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns1;
    uint32_t dns2;
} web_api_network_payload_t;

#include "auth_service.h"


typedef struct {
    char username[32];
    char password[96];
    bool remember_me;
} web_api_auth_login_payload_t;

typedef struct {
    char token[AUTH_TOKEN_MAX_LEN + 1];
} web_api_auth_logout_payload_t;

typedef struct {
    char token[AUTH_TOKEN_MAX_LEN + 1];
    char current_password[96];
    char new_password[96];
} web_api_auth_change_password_payload_t;

typedef struct {
    web_api_cmd_type_t type;
    uint32_t request_id;
    union {
        web_api_network_payload_t network;
        web_api_auth_login_payload_t auth_login;
        web_api_auth_logout_payload_t auth_logout;
        web_api_auth_change_password_payload_t auth_change_password;
    };
} web_api_request_t;

typedef struct {
    esp_err_t status;
    uint32_t request_id;
    int http_status;
    char content_type[32];
    char body[1024];
    bool schedule_reboot;
} web_api_response_t;

esp_err_t web_api_start(void);
esp_err_t web_api_call(const web_api_request_t *request, web_api_response_t *response, uint32_t timeout_ms);
