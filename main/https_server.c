#include "https_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "web_api.h"
#include "lwip/ip4_addr.h"
#include <lwip/sockets.h>
#include "authentication.h"

static const char *TAG = "HTTPS";
static httpd_handle_t s_server;
static httpd_handle_t s_ssl_server;
static bool is_spiffs_mounted = false; 

static char* server_cert_pem = NULL; 
static char* server_key_pem = NULL;
static int server_cert_len = 0;
static int server_key_len = 0;
//extern const char server_cert_pem_start[] asm("_binary_server_crt_start");  // эмуляция файлов
//extern const char server_cert_pem_end[]   asm("_binary_server_crt_end");
//extern const char server_key_pem_start[]  asm("_binary_server_key_start");
//extern const char server_key_pem_end[]    asm("_binary_server_key_end");

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
    if (is_spiffs_mounted) {
        return ESP_OK; // уже смонтирован
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 16,
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
    char path[256] = "/spiffs/web";
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


static esp_err_t get_client_ip(httpd_req_t *req, char *ip_buffer, size_t buffer_len) {
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return ESP_FAIL;
    }
    
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));
    
    if (getpeername(sockfd, (struct sockaddr *)&client_addr, &addr_len) != 0) {
        ESP_LOGE("HTTP", "getpeername failed: errno %d", errno);
        return ESP_FAIL;
    }
    
    // Конвертируем IP в строку
    if (client_addr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &s->sin_addr, ip_buffer, buffer_len);
    } else if (client_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &s->sin6_addr, ip_buffer, buffer_len);
    } else {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Обработчик логина
esp_err_t login_handler(httpd_req_t *req) {
    
    char client_ip[INET6_ADDRSTRLEN];
    
    if (get_client_ip(req, client_ip, sizeof(client_ip)) == ESP_OK) {
        ESP_LOGI("LOGIN", "Login attempt from IP: %s", client_ip);
    } else {
        ESP_LOGE("LOGIN", "Failed to get client IP");
        strcpy(client_ip, "unknown");
    }
    
    // Парсим JSON из тела запроса
    char body[1024];
    int ret, remaining = req->content_len;

    while (remaining > 0) {
        /* Read the data for the request */
        ret = httpd_req_recv(req, body, MIN(remaining, sizeof(body)));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }
        /* Process your data in 'buf' here */
        remaining -= ret;
    }

    cJSON *root = cJSON_Parse(body);
    const char *username = cJSON_GetObjectItem(root, "username")->valuestring;
    const char *password = cJSON_GetObjectItem(root, "password")->valuestring;
    ESP_LOGI(TAG, "Login attempt: %s", username);


    char user_agent[128];
    httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, sizeof(user_agent));

    ESP_LOGI(TAG, "User-Agent: %s", user_agent);
    // Создаём сессию
    session_t *session = create_session(username, password, client_ip, user_agent);
    
    if (session) {
        // Отправляем session_id в cookie
        char cookie_header[64];
        snprintf(cookie_header, sizeof(cookie_header), "SESSION_ID=%s; HttpOnly", session->session_id);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_header);
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        char response[256];
        snprintf(response, sizeof(response), "{\"authenticated\":true, \"user\":\"%s\"}", session->user_id);
        httpd_resp_sendstr(req, response);
        //free(session);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
        //httpd_resp_send_json(req, 401, ");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// Middleware для проверки аутентификации
session_t* auth_middleware(httpd_req_t *req) {
    // Получаем session_id из cookie
    char session_id[33];
    size_t len = sizeof(session_id);
    esp_err_t err = httpd_req_get_cookie_val(req, "SESSION_ID", session_id, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SESSION_ID cookie read failed: %s (%d), required_len=%u", esp_err_to_name(err), err, (unsigned)len);
        return NULL;
    }
    session_id[sizeof(session_id) - 1] = '\0'; // гарантируем null-терминатор
    ESP_LOGI(TAG, "Auth middleware, session_id: %s", session_id);
    
    char client_ip[INET6_ADDRSTRLEN];
    
    if (get_client_ip(req, client_ip, sizeof(client_ip)) == ESP_OK) {
        ESP_LOGI("LOGIN", "Login attempt from IP: %s", client_ip);
    } else {
        ESP_LOGE("LOGIN", "Failed to get client IP");
        strcpy(client_ip, "unknown");
    }
       
    return validate_session(session_id, client_ip);
}

esp_err_t auth_check_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Auth check for URI: %s", req->uri);
    session_t *session = auth_middleware(req);
    if (session) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        char response[256];
        snprintf(response, sizeof(response), "{\"authenticated\":true, \"user\":\"%s\"}", session->user_id);
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"authenticated\":false}");
    }
    return ESP_OK;
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
    const httpd_uri_t auth_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = login_handler, // TODO: implement auth handler
    };
    const httpd_uri_t auth_check_uri = {
        .uri = "/api/auth/check",
        .method = HTTP_GET,
        .handler = auth_check_handler, // TODO: implement auth check handler
    };

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &net_cfg_uri);
    httpd_register_uri_handler(server, &dhcp_uri);
    httpd_register_uri_handler(server, &static_uri);
    httpd_register_uri_handler(server, &reboot_uri);
    httpd_register_uri_handler(server, &assets_uri);
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &auth_uri);
    httpd_register_uri_handler(server, &auth_check_uri);
    httpd_register_uri_handler(server, &wildcard_static_uri);
    
}

static void http_server_start()
{
/*    if (mount_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition 'storage'");
        vTaskDelete(NULL);
        return;
    }
*/
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.stack_size = 12288;
    conf.max_uri_handlers = 16;
    conf.max_open_sockets = 4;
    conf.uri_match_fn = httpd_uri_match_wildcard;
    
    esp_err_t ret = httpd_start(&s_server, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        //vTaskDelete(NULL);
        return;
    }

    register_uri_handlers(s_server);
    ESP_LOGI(TAG, "HTTP server started on port 80");

    /*while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }*/
}

static esp_err_t read_cert_and_key(const char *cert_path, const char *key_path)
{
    FILE *f = fopen(cert_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", cert_path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    server_cert_len = ftell(f);   
    fseek(f, 0, SEEK_SET); 

    ESP_LOGI(TAG, "Server certificate size: %d bytes", server_cert_len);
    server_cert_pem = malloc(server_cert_len + 1);  // +1 for null terminator
    if (!server_cert_pem) {
        ESP_LOGE(TAG, "Failed to allocate memory for server certificate");
        fclose(f);
        return ESP_FAIL;
    }

    if (fread(server_cert_pem, 1, server_cert_len, f) != server_cert_len) {
        ESP_LOGE(TAG, "Failed to read server certificate");
        free(server_cert_pem);
        fclose(f);
        return ESP_FAIL;
    }
    // Null-terminate for mbedtls PEM parser
    server_cert_pem[server_cert_len] = '\0';
    server_cert_len++;  // Include null terminator in length
    ESP_LOGI(TAG, "Server certificate read successfully");
    ESP_LOGI(TAG, "Certificate content:\n%.*s", server_cert_len, server_cert_pem);
    fclose(f);

    f = fopen(key_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", key_path);
        free(server_cert_pem);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    server_key_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    server_key_pem = malloc(server_key_len + 1);  // +1 for null terminator
    if (!server_key_pem) {
        ESP_LOGE(TAG, "Failed to allocate memory for server key");
        free(server_cert_pem);
        fclose(f);
        return ESP_FAIL;
    }
    if (fread(server_key_pem, 1, server_key_len, f) != server_key_len) {
        ESP_LOGE(TAG, "Failed to read server key");
        free(server_cert_pem);
        free(server_key_pem);
        fclose(f);
        return ESP_FAIL;
    }
    // Null-terminate for mbedtls PEM parser
    server_key_pem[server_key_len] = '\0';
    server_key_len++;  // Include null terminator in length
    ESP_LOGI(TAG, "Server key read successfully");
    ESP_LOGI(TAG, "Key content:\n%.*s", server_key_len, server_key_pem);
    fclose(f);
    return ESP_OK;
}

static void https_server_task(void *arg)
{
    if (mount_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition 'storage'");
        vTaskDelete(NULL);
        return;
    }

    if (read_cert_and_key("/spiffs/certs/server.crt", "/spiffs/certs/server.key") != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read server certificate and key");
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

 
    conf.servercert = (const uint8_t *)server_cert_pem;
    conf.servercert_len = server_cert_len;
    conf.prvtkey_pem = (const uint8_t *)server_key_pem;
    conf.prvtkey_len = server_key_len;

    esp_err_t ret = httpd_ssl_start(&s_ssl_server, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    register_uri_handlers(s_ssl_server);
    ESP_LOGI(TAG, "HTTPS server started on port 443");

    http_server_start();
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
    if (s_ssl_server) {
        httpd_ssl_stop(s_ssl_server);
        s_ssl_server = NULL;
    }

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
