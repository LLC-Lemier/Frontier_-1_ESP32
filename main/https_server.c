#include "https_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "web_api.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "HTTPS";
static httpd_handle_t s_server;

extern const char server_cert_pem_start[] asm("_binary_server_crt_start");  // эмуляция файлов
extern const char server_cert_pem_end[]   asm("_binary_server_crt_end");
extern const char server_key_pem_start[]  asm("_binary_server_key_start");
extern const char server_key_pem_end[]    asm("_binary_server_key_end");

static const char *guess_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".json") == 0) return "application/json";
    return "application/octet-stream";
}

static esp_err_t mount_spiffs(void) // раздел фронта
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    return esp_vfs_spiffs_register(&conf);
}

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, guess_content_type(path));
    char chunk[1024];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char path[256] = "/spiffs";
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0) {
        strlcat(path, "/index.html", sizeof(path));
    } else {
        strlcat(path, uri, sizeof(path));
    }
    return send_file(req, path);
}

static bool parse_ipv4_string(cJSON *root, const char *field, uint32_t *out_addr, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!item || !cJSON_IsString(item) || item->valuestring == NULL) {
        return !required;
    }

    ip4_addr_t addr;
    if (!ip4addr_aton(item->valuestring, &addr)) {
        return false;
    }
    *out_addr = addr.addr;
    return true;
}


static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len)
{
    int total = req->content_len;
    if (total < 0 || (size_t)total >= buffer_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return ESP_ERR_INVALID_SIZE;
    }
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buffer + received, total - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
            return ESP_FAIL;
        }
        received += ret;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static bool get_authorization_bearer(httpd_req_t *req, char *token, size_t token_len)
{
    char header[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    const char *prefix = "Bearer ";
    size_t prefix_len = strlen(prefix);
    if (strncmp(header, prefix, prefix_len) != 0) {
        return false;
    }
    strlcpy(token, header + prefix_len, token_len);
    return token[0] != '\0';
}

static bool ensure_authorized(httpd_req_t *req)
{
    char token[AUTH_TOKEN_MAX_LEN + 1] = {0};
    if (!get_authorization_bearer(req, token, sizeof(token)) || !auth_service_verify_token(token)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
        return false;
    }
    return true;
}

static esp_err_t send_json_stub(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t do_api_call(httpd_req_t *req, web_api_request_t *request)
{
    web_api_response_t response = {0};
    esp_err_t ret = web_api_call(request, &response, 5000); //  мост между https и webcall
    if (ret != ESP_OK && response.http_status == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "API call failed");
        return ret;
    }

    httpd_resp_set_status(req, response.http_status == 202 ? "202 Accepted" :
                               response.http_status == 400 ? "400 Bad Request" :
                               response.http_status == 401 ? "401 Unauthorized" :
                               response.http_status == 404 ? "404 Not Found" :
                               response.http_status == 500 ? "500 Internal Server Error" : "200 OK");
    httpd_resp_set_type(req, response.content_type[0] ? response.content_type : "application/json");
    httpd_resp_sendstr(req, response.body[0] ? response.body : "{\"ok\":true}");
    return ret;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    web_api_request_t request = {.type = WEB_API_CMD_GET_STATUS, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t network_config_get_handler(httpd_req_t *req)
{
    web_api_request_t request = {.type = WEB_API_CMD_GET_NETWORK_CONFIG, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t network_dhcp_post_handler(httpd_req_t *req)
{
    web_api_request_t request = {
        .type = WEB_API_CMD_SET_DHCP,
        .request_id = (uint32_t)esp_timer_get_time(),
        .network = {.dhcp_enabled = true},
    };
    return do_api_call(req, &request);
}

static esp_err_t network_static_post_handler(httpd_req_t *req)
{
    char buffer[512];
    if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    web_api_request_t request = {.type = WEB_API_CMD_SET_STATIC, .request_id = (uint32_t)esp_timer_get_time()};
    bool ok = parse_ipv4_string(root, "ip", &request.network.ip, true) &&
              parse_ipv4_string(root, "netmask", &request.network.netmask, true) &&
              parse_ipv4_string(root, "gateway", &request.network.gateway, true) &&
              parse_ipv4_string(root, "dns1", &request.network.dns1, false) &&
              parse_ipv4_string(root, "dns2", &request.network.dns2, false);
    cJSON_Delete(root);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IPv4 fields");
        return ESP_ERR_INVALID_ARG;
    }
    return do_api_call(req, &request);
}

static esp_err_t settings_network_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_SETTINGS_NETWORK, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t settings_rs232_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_SETTINGS_RS232, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t settings_gateway_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_SETTINGS_GATEWAY, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_DASHBOARD, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t dashboard_logs_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_DASHBOARD_LOGS, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t user_get_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    web_api_request_t request = {.type = WEB_API_CMD_GET_USER, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}

static esp_err_t settings_network_post_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char buffer[768];
    if (req->content_len > 0) {
        if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return send_json_stub(req, "200 OK", "{\"status\":\"ok\"}");
}

static esp_err_t settings_network_set_post_handler(httpd_req_t *req)
{
    return settings_network_post_handler(req);
}

static esp_err_t settings_rs232_post_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char buffer[768];
    if (req->content_len > 0) {
        if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return send_json_stub(req, "200 OK", "{\"Change_RS_status\":\"OK\"}");
}

static esp_err_t settings_gateway_post_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char buffer[768] = {0};
    if (req->content_len > 0) {
        if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return send_json_stub(req, "200 OK", "{\"status\":\"ok\"}");
}

static esp_err_t settings_gateway_set_post_handler(httpd_req_t *req)
{
    if (!ensure_authorized(req)) {
        return ESP_ERR_INVALID_STATE;
    }
    char buffer[768] = {0};
    if (req->content_len > 0) {
        if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return send_json_stub(req, "200 OK", "{\"mode\":\"tcp_server\",\"local_port\":8080,\"status\":\"stopped\",\"timeout\":30,\"keepalive\":false,\"port\":8080,\"enabled\":false}");
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    web_api_request_t request = {.type = WEB_API_CMD_REBOOT, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
}


static esp_err_t auth_login_post_handler(httpd_req_t *req)
{
    char buffer[512];
    if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *username = cJSON_GetObjectItemCaseSensitive(root, "username");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *remember_me = cJSON_GetObjectItemCaseSensitive(root, "rememberMe");

    if (!cJSON_IsString(username) || !cJSON_IsString(password)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_ERR_INVALID_ARG;
    }

    web_api_request_t request = {
        .type = WEB_API_CMD_AUTH_LOGIN,
        .request_id = (uint32_t)esp_timer_get_time(),
    };
    strlcpy(request.auth_login.username, username->valuestring, sizeof(request.auth_login.username));
    strlcpy(request.auth_login.password, password->valuestring, sizeof(request.auth_login.password));
    request.auth_login.remember_me = cJSON_IsBool(remember_me) ? cJSON_IsTrue(remember_me) : false;
    cJSON_Delete(root);
    return do_api_call(req, &request);
}

static esp_err_t auth_logout_post_handler(httpd_req_t *req)
{
    web_api_request_t request = {
        .type = WEB_API_CMD_AUTH_LOGOUT,
        .request_id = (uint32_t)esp_timer_get_time(),
    };
    get_authorization_bearer(req, request.auth_logout.token, sizeof(request.auth_logout.token));
    return do_api_call(req, &request);
}

static esp_err_t system_password_post_handler(httpd_req_t *req)
{
    char token[AUTH_TOKEN_MAX_LEN + 1] = {0};
    if (!get_authorization_bearer(req, token, sizeof(token))) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
        return ESP_ERR_INVALID_STATE;
    }

    char buffer[512];
    if (read_request_body(req, buffer, sizeof(buffer)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *current_password = cJSON_GetObjectItemCaseSensitive(root, "current_password");
    cJSON *new_password = cJSON_GetObjectItemCaseSensitive(root, "new_password");
    if (!cJSON_IsString(current_password) || !cJSON_IsString(new_password)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_ERR_INVALID_ARG;
    }

    web_api_request_t request = {
        .type = WEB_API_CMD_AUTH_CHANGE_PASSWORD,
        .request_id = (uint32_t)esp_timer_get_time(),
    };
    strlcpy(request.auth_change_password.token, token, sizeof(request.auth_change_password.token));
    strlcpy(request.auth_change_password.current_password, current_password->valuestring, sizeof(request.auth_change_password.current_password));
    strlcpy(request.auth_change_password.new_password, new_password->valuestring, sizeof(request.auth_change_password.new_password));
    cJSON_Delete(root);
    return do_api_call(req, &request);
}

static void register_uri_handlers(httpd_handle_t server) // handler
{
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = static_get_handler,
    };
    const httpd_uri_t assets_uri = {
        .uri = "/assets/*",
        .method = HTTP_GET,
        .handler = static_get_handler,
    };
    const httpd_uri_t wildcard_static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t auth_login_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = auth_login_post_handler,
    };
    const httpd_uri_t auth_logout_uri = {
        .uri = "/api/auth/logout",
        .method = HTTP_POST,
        .handler = auth_logout_post_handler,
    };
    const httpd_uri_t system_password_uri = {
        .uri = "/api/system/password",
        .method = HTTP_POST,
        .handler = system_password_post_handler,
    };
    const httpd_uri_t user_uri = {
        .uri = "/api/user",
        .method = HTTP_GET,
        .handler = user_get_handler,
    };
    const httpd_uri_t net_cfg_uri = {
        .uri = "/api/network/config",
        .method = HTTP_GET,
        .handler = network_config_get_handler,
    };
    const httpd_uri_t settings_network_uri = {
        .uri = "/api/settings/network",
        .method = HTTP_GET,
        .handler = settings_network_get_handler,
    };
    const httpd_uri_t settings_network_post_uri = {
        .uri = "/api/settings/network",
        .method = HTTP_POST,
        .handler = settings_network_post_handler,
    };
    const httpd_uri_t settings_network_set_uri = {
        .uri = "/api/settings/network/set",
        .method = HTTP_POST,
        .handler = settings_network_set_post_handler,
    };
    const httpd_uri_t settings_rs232_uri = {
        .uri = "/api/settings/rs232",
        .method = HTTP_GET,
        .handler = settings_rs232_get_handler,
    };
    const httpd_uri_t settings_rs232_post_uri = {
        .uri = "/api/settings/rs232",
        .method = HTTP_POST,
        .handler = settings_rs232_post_handler,
    };
    const httpd_uri_t settings_gateway_uri = {
        .uri = "/api/settings/gateway",
        .method = HTTP_GET,
        .handler = settings_gateway_get_handler,
    };
    const httpd_uri_t settings_gateway_post_uri = {
        .uri = "/api/settings/gateway",
        .method = HTTP_POST,
        .handler = settings_gateway_post_handler,
    };
    const httpd_uri_t settings_gateway_set_uri = {
        .uri = "/api/settings/gateway/set",
        .method = HTTP_POST,
        .handler = settings_gateway_set_post_handler,
    };
    const httpd_uri_t dashboard_uri = {
        .uri = "/api/dashboard",
        .method = HTTP_GET,
        .handler = dashboard_get_handler,
    };
    const httpd_uri_t dashboard_logs_uri = {
        .uri = "/api/dashboard/logs",
        .method = HTTP_GET,
        .handler = dashboard_logs_get_handler,
    };
    const httpd_uri_t dhcp_uri = {
        .uri = "/api/network/dhcp",
        .method = HTTP_POST,
        .handler = network_dhcp_post_handler,
    };
    const httpd_uri_t static_uri = {
        .uri = "/api/network/static",
        .method = HTTP_POST,
        .handler = network_static_post_handler,
    };
    const httpd_uri_t reboot_uri = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
    };

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &auth_login_uri);
    httpd_register_uri_handler(server, &auth_logout_uri);
    httpd_register_uri_handler(server, &system_password_uri);
    httpd_register_uri_handler(server, &user_uri);
    httpd_register_uri_handler(server, &net_cfg_uri);
    httpd_register_uri_handler(server, &settings_network_uri);
    httpd_register_uri_handler(server, &settings_network_post_uri);
    httpd_register_uri_handler(server, &settings_network_set_uri);
    httpd_register_uri_handler(server, &settings_rs232_uri);
    httpd_register_uri_handler(server, &settings_rs232_post_uri);
    httpd_register_uri_handler(server, &settings_gateway_uri);
    httpd_register_uri_handler(server, &settings_gateway_post_uri);
    httpd_register_uri_handler(server, &settings_gateway_set_uri);
    httpd_register_uri_handler(server, &dashboard_uri);
    httpd_register_uri_handler(server, &dashboard_logs_uri);
    httpd_register_uri_handler(server, &dhcp_uri);
    httpd_register_uri_handler(server, &static_uri);
    httpd_register_uri_handler(server, &reboot_uri);
    httpd_register_uri_handler(server, &assets_uri);
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &wildcard_static_uri);
}

static void https_server_task(void *arg)
{
    if (mount_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition 'storage'");
        vTaskDelete(NULL);
        return;
    }

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.port_secure = 443;
    conf.httpd.server_port = 443;
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
    conf.httpd.stack_size = 12288;
    conf.httpd.max_uri_handlers = 24;
    conf.httpd.max_open_sockets = 4;
    conf.servercert = (const uint8_t *)server_cert_pem_start;
    conf.servercert_len = server_cert_pem_end - server_cert_pem_start;
    conf.prvtkey_pem = (const uint8_t *)server_key_pem_start;
    conf.prvtkey_len = server_key_pem_end - server_key_pem_start;

    esp_err_t ret = httpd_ssl_start(&s_server, &conf); // старт
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    register_uri_handlers(s_server);
    ESP_LOGI(TAG, "HTTPS server started on port 443");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t start_https_server_task(void)
{
    if (s_server) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(https_server_task, "https_server", 12288, NULL, 5, NULL, tskNO_AFFINITY);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void stop_https_server(void)
{
    if (s_server) {
        httpd_ssl_stop(s_server);
        s_server = NULL;
    }
}
