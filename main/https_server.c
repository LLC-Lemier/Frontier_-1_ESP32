#include "https_server.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "web_api.h"
#include "lwip/ip4_addr.h"
#include <lwip/sockets.h>
#include "authentication.h"
#include "network_config.h"
#include "spiffs_driver.h"

static const char *TAG = "HTTPS";
static httpd_handle_t s_server;
static httpd_handle_t s_ssl_server;

static char* server_cert_pem = NULL; 
static char* server_key_pem = NULL;
static int server_cert_len = 0;
static int server_key_len = 0;

typedef struct {
    bool is_ca_present;
    bool is_client_cert_present;
    bool is_client_key_present; 

    uint16_t ca_cert_len;
    uint16_t client_cert_len;
    uint16_t client_key_len;

    char ca_cert_pem[15 * 1024]; // 15 KB для CA сертификата
    char client_cert_pem[5 * 1024]; // 5 KB для клиентского сертификата
    char client_key_pem[5 * 1024]; // 5 KB для клиентского ключа
} certs_loader_ctx_t;

static certs_loader_ctx_t* s_certs_ctx;
static eap_tls_config_t* s_eap_tls_config; 
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

// Добавьте эту функцию в ваш ESP32 код
esp_err_t add_cors_headers(httpd_req_t *req) {
    // Для разработки - разрешаем конкретный origin
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", 
                       "https://localhost:5173/"); // Ваш фронтенд порт
    
    // Обязательно для credentials (cookies)
    httpd_resp_set_hdr(req, "Access-Control-Allow-Credentials", "true");
    
    // Разрешаем нужные методы
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", 
                       "GET, POST, PUT, DELETE, OPTIONS");
    
    // Разрешаем нужные заголовки
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", 
                       "Content-Type, Authorization, X-Requested-With");
    
    return ESP_OK;
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

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    const char *content_type = guess_content_type(path);
    bool is_gzipped = false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found plain: %s", path);
        strlcat(path, ".gz", sizeof(path)); // пробуем сжатый вариант
        f = fopen(path, "rb");
        if (f) {
             ESP_LOGI(TAG, "Found gzipped version of file: %s", path);
             is_gzipped = true;
             httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
    } 
    if (!f) {
        f = fopen("/spiffs/web/index.html", "rb");
        content_type = "text/html";
    }
    if (f) {        
        ESP_LOGI(TAG, "Serving file: %s with content type: %s, gzipped: %s", path, content_type, is_gzipped ? "yes" : "no");
        httpd_resp_set_type(req, content_type);
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
    return ESP_FAIL;
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char path[256] = "/spiffs/web";
    const char *uri = req->uri;

    ESP_LOGI(TAG, "Static file request: %s", uri);
    strlcat(path, uri, sizeof(path));    

    const char *content_type = guess_content_type(path);
    bool is_gzipped = false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found plain: %s", path);
        strlcat(path, ".gz", sizeof(path)); // пробуем сжатый вариант
        f = fopen(path, "rb");
        if (f) {
             ESP_LOGI(TAG, "Found gzipped version of file: %s", path);
             is_gzipped = true;
             httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        }
    } 
    if (!f) {
        f = fopen("/spiffs/web/index.html", "rb");
        content_type = "text/html";
    }
    if (f) {        
        ESP_LOGI(TAG, "Serving file: %s with content type: %s, gzipped: %s", path, content_type, is_gzipped ? "yes" : "no");
        httpd_resp_set_type(req, content_type);
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
    return ESP_FAIL;
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
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);
 
    if (session) {
        web_api_request_t request = {
            .type = WEB_API_CMD_GET_NETWORK_CONFIG, 
            .request_id = (uint32_t)esp_timer_get_time()
        };
        esp_err_t result = do_api_call(req, &request);
        
        return result;
    }
 
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    //httpd_resp_send_json(req, 401, ");
    return ESP_OK;
}

static esp_err_t network_config_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);
 
    if (session) {
        // Парсим JSON из тела запроса
        char body[1024];
        int ret, remaining = req->content_len;

        if (remaining <= 0 || remaining >= (int)sizeof(body)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
            return ESP_ERR_INVALID_SIZE;
        }

        int whole_body_received = 0;

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
            remaining -= ret;
            whole_body_received += ret;
        }

        cJSON *root = cJSON_Parse(body);
        bool dhcp = cJSON_GetObjectItemCaseSensitive(root, "dhcp") ? cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dhcp")) : false;

        network_config_t runtime_config;
        network_config_get_runtime(&runtime_config);

        if (dhcp) {
            network_config_t network_config = { 
                .dhcp_enabled = true,
                .eap_tls_config = runtime_config.eap_tls_config,
                .ntp_config = runtime_config.ntp_config
            };
            network_config_save(&network_config);
            network_config_apply_saved();
        } else {
            uint32_t ip, netmask, gateway, dns1, dns2;
            if (!parse_ipv4_string(root, "ip", &ip, true) ||
                !parse_ipv4_string(root, "netmask", &netmask, true) ||
                !parse_ipv4_string(root, "gateway", &gateway, true) ||
                !parse_ipv4_string(root, "dns1", &dns1, false) ||
                !parse_ipv4_string(root, "dns2", &dns2, false)) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IPv4 fields");
                return ESP_ERR_INVALID_ARG;
            }
            network_config_t network_config = {
                .dhcp_enabled = false,
                .ip = ip,
                .netmask = netmask,
                .gateway = gateway,
                .dns1 = dns1,
                .dns2 = dns2,
                .eap_tls_config = runtime_config.eap_tls_config,
                .ntp_config = runtime_config.ntp_config
            };
            network_config_save(&network_config);
            network_config_apply_saved();
        }

        cJSON_Delete(root);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        
        return ESP_OK;
    }
 
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    //httpd_resp_send_json(req, 401, ");
    return ESP_OK;
}

static esp_err_t radius_config_get_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    
    if (session) {
        // Обработка GET-запроса для конфигурации RADIUS
        // ...
    }

 
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    //httpd_resp_send_json(req, 401, ");
    return ESP_OK;
}


static esp_err_t radius_config_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    char body[1024];

    if (session) {
        if (s_certs_ctx) {
            if (s_certs_ctx->is_ca_present && s_certs_ctx->is_client_cert_present && s_certs_ctx->is_client_key_present) {
                FILE *f_ca = fopen("/spiffs/certs/ca_new.pem", "wb");
                FILE *f_client_cert = fopen("/spiffs/certs/client_new.crt", "wb");
                FILE *f_client_key = fopen("/spiffs/certs/client_new.key", "wb");
                
                if (f_ca && f_client_cert && f_client_key) {
                    fwrite(s_certs_ctx->ca_cert_pem, 1, s_certs_ctx->ca_cert_len, f_ca);
                    fwrite(s_certs_ctx->client_cert_pem, 1, s_certs_ctx->client_cert_len, f_client_cert);
                    fwrite(s_certs_ctx->client_key_pem, 1, s_certs_ctx->client_key_len, f_client_key);
                    fclose(f_ca);
                    fclose(f_client_cert);
                    fclose(f_client_key);

                    httpd_resp_set_status(req, "200 OK");
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_sendstr(req, "{\"ok\":true}");
                    return ESP_OK;
                }
                if (f_ca) fclose(f_ca);
                if (f_client_cert) fclose(f_client_cert);
                if (f_client_key) fclose(f_client_key);
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"error\":\"Failed to save cert files\"}");
                return ESP_OK;
            } 
            httpd_resp_set_status(req, "428 Precondition Required");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"CA cert, client cert and client key must all be uploaded before applying EAP-TLS config\"}");
            return ESP_ERR_INVALID_STATE;
        }
        int ret, remaining = req->content_len;

        if (remaining <= 0 || remaining >= (int)sizeof(body)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
            return ESP_ERR_INVALID_SIZE;
        }

        int whole_body_received = 0;

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
            remaining -= ret;
            whole_body_received += ret;
        }
        cJSON *root = cJSON_Parse(body);
        
        s_eap_tls_config = malloc(sizeof(eap_tls_config_t));
        if (!s_eap_tls_config) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }

        s_certs_ctx = malloc(sizeof(certs_loader_ctx_t));

        if (!s_certs_ctx) {
            free(s_eap_tls_config);
            s_eap_tls_config = NULL;
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }

        s_certs_ctx->ca_cert_len = 0;
        s_certs_ctx->client_cert_len = 0;
        s_certs_ctx->client_key_len = 0;

        const char *identity = cJSON_GetObjectItemCaseSensitive(root, "identity") ? cJSON_GetObjectItemCaseSensitive(root, "identity")->valuestring : "";

        if (strlen(identity) < 256) {
            ESP_LOGI(TAG, "Received EAP-TLS config with identity: %s", identity);
        } else {
            ESP_LOGW(TAG, "Received EAP-TLS config with too long identity, length: %zu", strlen(identity));
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Identity too long");
            free(s_eap_tls_config);
            s_eap_tls_config = NULL;
            free(s_certs_ctx);
            s_certs_ctx = NULL;
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        
        strlcpy(s_eap_tls_config->identity, identity, sizeof(s_eap_tls_config->identity));
        s_eap_tls_config->timeout_ms = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms") ? cJSON_GetObjectItemCaseSensitive(root, "timeout_ms")->valueint : 60000;
        s_eap_tls_config->max_retries = cJSON_GetObjectItemCaseSensitive(root, "max_retries") ? cJSON_GetObjectItemCaseSensitive(root, "max_retries")->valueint : 5;

        cJSON_Delete(root);

        char response[512];
        snprintf(response, sizeof(response), "{\"identity\":\"%s\", \"timeout_ms\":%" PRIu32 ", \"max_retries\":%d}", s_eap_tls_config->identity, s_eap_tls_config->timeout_ms, s_eap_tls_config->max_retries);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t radius_config_ca_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    if (session) {
        if (s_certs_ctx) {
            s_certs_ctx->is_ca_present = true;
            s_certs_ctx->ca_cert_len = req->content_len;

            int ret, remaining = req->content_len;

            if (remaining <= 0 || remaining >= (int)sizeof(s_certs_ctx->ca_cert_pem)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
                return ESP_ERR_INVALID_SIZE;
            }

            while (remaining > 0) {
                /* Read the data for the request */
                ret = httpd_req_recv(req, s_certs_ctx->ca_cert_pem, MIN(remaining, sizeof(s_certs_ctx->ca_cert_pem)));
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        /* Retry if timeout occurred */
                        continue;
                    }
                    return ESP_FAIL;
                }
                remaining -= ret;
            }

            s_certs_ctx->ca_cert_pem[s_certs_ctx->ca_cert_len] = '\0'; // гарантируем null-терминатор

            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":true, \"ca_cert_len\":%d}", s_certs_ctx->ca_cert_len);

            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
        httpd_resp_set_status(req, "428 Precondition Required");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"EAP-TLS config must be set before uploading CA cert\"}");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t radius_config_ccert_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    if (session) {
        if (s_certs_ctx) {
            s_certs_ctx->is_client_cert_present = true;
            s_certs_ctx->client_cert_len = req->content_len;

            int ret, remaining = req->content_len;

            if (remaining <= 0 || remaining >= (int)sizeof(s_certs_ctx->client_cert_pem)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
                return ESP_ERR_INVALID_SIZE;
            }

            while (remaining > 0) {
                /* Read the data for the request */
                ret = httpd_req_recv(req, s_certs_ctx->client_cert_pem, MIN(remaining, sizeof(s_certs_ctx->client_cert_pem)));
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        /* Retry if timeout occurred */
                        continue;
                    }
                    return ESP_FAIL;
                }
                remaining -= ret;
            }

            s_certs_ctx->client_cert_pem[s_certs_ctx->client_cert_len] = '\0'; // гарантируем null-терминатор

            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":true, \"client_cert_len\":%d}", s_certs_ctx->client_cert_len);
            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
        httpd_resp_set_status(req, "428 Precondition Required");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"EAP-TLS config must be set before uploading client cert\"}");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t radius_config_ckey_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    if (session) {
        if (s_certs_ctx) {
            s_certs_ctx->is_client_key_present = true;
            s_certs_ctx->client_key_len = req->content_len;

            int ret, remaining = req->content_len;

            if (remaining <= 0 || remaining >= (int)sizeof(s_certs_ctx->client_key_pem)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
                return ESP_ERR_INVALID_SIZE;
            }

            while (remaining > 0) {
                /* Read the data for the request */
                ret = httpd_req_recv(req, s_certs_ctx->client_key_pem, MIN(remaining, sizeof(s_certs_ctx->client_key_pem)));
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        /* Retry if timeout occurred */
                        continue;
                    }
                    return ESP_FAIL;
                }
                remaining -= ret;
            }

            s_certs_ctx->client_key_pem[s_certs_ctx->client_key_len] = '\0'; // гарантируем null-терминатор

            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":true, \"client_key_len\":%d}", s_certs_ctx->client_key_len);

            httpd_resp_set_status(req, "202 Accepted");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, response);
            return ESP_OK;
        }
        httpd_resp_set_status(req, "428 Precondition Required");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"EAP-TLS config must be set before uploading client key\"}");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t ntp_config_get_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    if (session) {
        char response[2048];
        
        network_config_t saved = {0};
        network_config_load(&saved);
        snprintf(
            response, 
            sizeof(response), 
            "{\"servers\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"]}", 
            saved.ntp_config->server[0],
            saved.ntp_config->server[1],
            saved.ntp_config->server[2],
            saved.ntp_config->server[3],
            saved.ntp_config->server[4]
        );
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);

        return ESP_OK;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t ntp_config_put_handler(httpd_req_t *req)
{
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    session_t *session = auth_middleware(req);

    if (session) {
        // Парсим JSON из тела запроса
        char body[2048];
        int ret, remaining = req->content_len;

        if (remaining <= 0 || remaining >= (int)sizeof(body)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
            return ESP_ERR_INVALID_SIZE;
        }

        int whole_body_received = 0;

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
            remaining -= ret;
            whole_body_received += ret;
        }

        //network_config_t runtime_config;
        network_config_t* saved_config = network_config_get_saved();

        cJSON *root = cJSON_Parse(body);
        const cJSON *servers = cJSON_GetObjectItem(root, "servers");
 
        if (!servers || !cJSON_IsArray(servers)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'servers' array");
            return ESP_ERR_INVALID_ARG;
        }

        int server_count = cJSON_GetArraySize(servers);
        if (server_count < 1 || server_count > 5) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Number of servers must be between 1 and 5");
            return ESP_ERR_INVALID_ARG;
        }

        ip_addr_t temp;

        for (int i = 0; i < server_count; i++) {
            cJSON *server = cJSON_GetArrayItem(servers, i);
            if (!cJSON_IsString(server) || server->valuestring == NULL) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "All servers must be valid strings");
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(server->valuestring) == 0) {
                saved_config->ntp_config->server_type[i] = NTP_SERVER_NONE;
                continue; // пропускаем пустые строки, они будут игнорироваться при сохранении
            }
            // Здесь можно добавить дополнительную валидацию строки сервера, если нужно
            if (ipaddr_aton(server->valuestring, &temp)) {
                saved_config->ntp_config->server_type[i] = NTP_SERVER_IP_ADDRESS;
            } else {
                saved_config->ntp_config->server_type[i] = NTP_SERVER_DOMAIN_NAME;
            }
            if (saved_config->ntp_config->server[i]) {
                free(saved_config->ntp_config->server[i]); // освобождаем старую строку, если она была
            } 
            if (strlen(server->valuestring) < 256) {
                saved_config->ntp_config->server[i] = strdup(server->valuestring); // сохраняем новую строку
            } else {
                saved_config->ntp_config->server[i] = NULL; // если строка слишком длинная, не сохраняем её
            }
        }

        network_config_save(saved_config);
        network_config_apply_saved();

        cJSON_Delete(root);
        
        return ESP_OK;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"Invalid credentials\"}");
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    web_api_request_t request = {.type = WEB_API_CMD_REBOOT, .request_id = (uint32_t)esp_timer_get_time()};
    return do_api_call(req, &request);
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
    add_cors_headers(req); // Добавляем CORS заголовки к ответу
    if (session) {
        // Отправляем session_id в cookie
        char cookie_header[100];
        snprintf(cookie_header, sizeof(cookie_header), "SESSION_ID=%s; HttpOnly; Path=/api/", session->session_id);
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
    const httpd_uri_t net_cfg_uri_get = {
        .uri = "/api/network/",
        .method = HTTP_GET,
        .handler = network_config_get_handler,
    };
    const httpd_uri_t net_cfg_uri_put = {
        .uri = "/api/network/",
        .method = HTTP_PUT,
        .handler = network_config_put_handler,
    };
    const httpd_uri_t radius_cfg_uri_get = {
        .uri = "/api/radius/",
        .method = HTTP_GET,
        .handler = radius_config_get_handler,
    };
    const httpd_uri_t radius_cfg_uri_put = {
        .uri = "/api/radius/",
        .method = HTTP_PUT,
        .handler = radius_config_put_handler,
    };
    const httpd_uri_t radius_cfg_ca_uri_put = {
        .uri = "/api/radius/ca/",
            .method = HTTP_PUT,
            .handler = radius_config_ca_put_handler,
        };
    const httpd_uri_t radius_cfg_ccert_uri_put = {
        .uri = "/api/radius/ccert/",
        .method = HTTP_PUT,
        .handler = radius_config_ccert_put_handler,
    };
    const httpd_uri_t radius_cfg_ckey_uri_put = {
        .uri = "/api/radius/ckey/",
        .method = HTTP_PUT,
        .handler = radius_config_ckey_put_handler,
    };
    const httpd_uri_t ntp_cfg_uri_get = {
        .uri = "/api/ntp/",
        .method = HTTP_GET,
        .handler = ntp_config_get_handler,
    };  
    const httpd_uri_t ntp_cfg_uri_put = {
        .uri = "/api/ntp/",
        .method = HTTP_PUT,
        .handler = ntp_config_put_handler,
    };

    /*const httpd_uri_t dhcp_uri = {
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
    };*/
    const httpd_uri_t auth_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = login_handler,
    };
    const httpd_uri_t auth_check_uri = {
        .uri = "/api/auth/check",
        .method = HTTP_GET,
        .handler = auth_check_handler,
    };

    httpd_register_uri_handler(server, &status_uri);
    
    httpd_register_uri_handler(server, &net_cfg_uri_get);
    httpd_register_uri_handler(server, &net_cfg_uri_put);

    httpd_register_uri_handler(server, &radius_cfg_uri_get);
    httpd_register_uri_handler(server, &radius_cfg_uri_put);
    httpd_register_uri_handler(server, &radius_cfg_ca_uri_put);
    httpd_register_uri_handler(server, &radius_cfg_ccert_uri_put);
    httpd_register_uri_handler(server, &radius_cfg_ckey_uri_put);

    httpd_register_uri_handler(server, &ntp_cfg_uri_get);
    httpd_register_uri_handler(server, &ntp_cfg_uri_put);

    /*httpd_register_uri_handler(server, &dhcp_uri);
    httpd_register_uri_handler(server, &static_uri);
    httpd_register_uri_handler(server, &reboot_uri);*/
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
