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

    esp_ip4_addr_t addr;
    if (!ip4addr_aton(item->valuestring, &addr)) {
        return false;
    }
    *out_addr = addr.addr;
    return true;
}

static esp_err_t do_api_call(httpd_req_t *req, web_api_request_t *request)
{
    web_api_response_t response = {0};
    esp_err_t ret = web_api_call(request, &response, 5000); // строка ответа
    if (ret != ESP_OK && response.http_status == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "API call failed");
        return ret;
    }

    httpd_resp_set_status(req, response.http_status == 202 ? "202 Accepted" :
                               response.http_status == 400 ? "400 Bad Request" :
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
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buffer)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
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

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    web_api_request_t request = {.type = WEB_API_CMD_REBOOT, .request_id = (uint32_t)esp_timer_get_time()};
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
    const httpd_uri_t net_cfg_uri = {
        .uri = "/api/network/config",
        .method = HTTP_GET,
        .handler = network_config_get_handler,
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
    httpd_register_uri_handler(server, &net_cfg_uri);
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
    conf.httpd.max_uri_handlers = 16;
    conf.httpd.max_open_sockets = 4;
    conf.servercert = (const uint8_t *)server_cert_pem_start;
    conf.servercert_len = server_cert_pem_end - server_cert_pem_start;
    conf.prvtkey_pem = (const uint8_t *)server_key_pem_start;
    conf.prvtkey_len = server_key_pem_end - server_key_pem_start;

    esp_err_t ret = httpd_ssl_start(&s_server, &conf);
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
