#include "eap_tls_supplicant.h"

#include "ethernet_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ssl_cookie.h"

static const char *TAG = "EAP_TLS_SUP";
static const char *password = "whatever";

static eap_tls_config_t s_config;
static eap_tls_state_t s_state = EAP_TLS_IDLE;
static uint8_t s_eap_identifier = 0;
static bool s_authenticated = false;

// TLS контексты
static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_ssl_conf;
static mbedtls_x509_crt s_ca_cert;
static mbedtls_x509_crt s_client_cert;
static mbedtls_pk_context s_client_key;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;

// Буферы для EAP-TLS сообщений
uint8_t frame[1518];
static uint8_t s_eap_out[1500];
static uint8_t s_tls_in[4096];
static uint8_t s_tls_out[4096];
static size_t s_tls_in_len = 0;
static size_t s_tls_out_len = 0;
static size_t s_eap_out_len = 0;

static eap_tls_bio_t s_bio;

static esp_err_t send_eapol_packet(void);

// Структура для передачи контекста в BIO callback
typedef struct {
    size_t *bio_len;
} test_bio_ctx_t;

// Био-функции для callback
static int eap_tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    // Сохраняем данные для отправки
    
    if (s_bio.send_len + len <= sizeof(s_bio.send_buf)) {
        memcpy(s_bio.send_buf + s_bio.send_len, buf, len);
        s_bio.send_len += len;
        return len;
    }
    return ESP_OK;
}

static int eap_tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    // Читаем данные из входного буфера
    
//    ESP_LOG_BUFFER_HEX("RECV_DATA", buf);  // Выведите HEX‑дамп полученных данных для диагностики
    size_t copy_len = len < s_tls_in_len ? len : s_tls_in_len;
    if (copy_len > 0) {
        memcpy(buf, s_tls_in, copy_len);
        memmove(s_tls_in, s_tls_in + copy_len, s_tls_in_len - copy_len);
        s_tls_in_len -= copy_len;
        return copy_len;

    }
    return MBEDTLS_ERR_SSL_WANT_READ;
}
static void my_set_timer(void *data, uint32_t delay_ms, uint32_t timeout_ms) {
    // Сохраните время начала таймера
    
}

static int my_get_timer(void *data) {
    // Верните 1, если таймаут истёк, 0 — если нет
    return 0;  // Замените на реальную логику
}
static void check_certs() {
    // Проверка сертификатов
    ESP_LOGI(TAG, "CA cert len: %d", s_ca_cert.raw.len);
    ESP_LOGI(TAG, "Client cert len: %d", s_client_cert.raw.len);

    if (s_client_cert.raw.len == 0) {
        ESP_LOGE(TAG, "❌ Client certificate is EMPTY!");
    }

    // Проверка, что own_cert установлен в конфигурации
    if (s_ssl_conf.private_key_cert == NULL) {
        ESP_LOGE(TAG, "❌ own_cert is NULL in SSL config!");
    } else {
        ESP_LOGI(TAG, "✅ own_cert configured");
    }
}

static void mbedtls_debug_func(void *ctx, int level, const char *file, int line, const char *str) {
    // Выводим все сообщения отладки mbedTLS
    ESP_LOGI("MBEDTLS", "%s:%d: %s", file, line, str);
}

#include "esp_random.h"

// Своя функция RNG
static int esp_rng(void *ctx, unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t random = esp_random();
        size_t copy = (len - i) < 4 ? (len - i) : 4;
        memcpy(buf + i, &random, copy);
    }
    return 0;  // Успех
}
// Инициализация TLS
static esp_err_t tls_init(void) {
    int ret;

    ESP_LOGI(TAG, "Initializing TLS with PEM certificates");
    ESP_LOGI(TAG, "CA cert size: %zu bytes", s_config.ca_cert_len);
    ESP_LOGI(TAG, "Client cert size: %zu bytes", s_config.client_cert_len);
    ESP_LOGI(TAG, "Client key size: %zu bytes", s_config.client_key_len);
      
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_ssl_conf);
    mbedtls_x509_crt_init(&s_ca_cert);
    mbedtls_x509_crt_init(&s_client_cert);
    mbedtls_pk_init(&s_client_key);
    //mbedtls_entropy_init(&s_entropy);
    //mbedtls_ctr_drbg_init(&s_ctr_drbg);
    //MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED
    ret = mbedtls_ssl_config_defaults(&s_ssl_conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,  // ВАЖНО: STREAM, не DATAGRAM
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    
    // Явно задайте набор шифров (если нужно)

    const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
        0
    };
    //mbedtls_ssl_conf_ciphersuites(&s_ssl_conf, ciphersuites);
    mbedtls_ssl_conf_min_tls_version(&s_ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&s_ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    ESP_LOGI(TAG, "✅ TLS 1.2 forced");
                                     
    if (ret != 0) {
        ESP_LOGE(TAG, "SSL config defaults failed: -0x%04x", -ret);
        return ESP_FAIL;
    }
    // Инициализация RNG
    mbedtls_ssl_conf_rng(&s_ssl_conf, esp_rng, NULL);

    ESP_LOGI(TAG, "✅ Using custom RNG (esp_random)");
    
    
    // Загрузка CA сертификата (PEM)
    ret = mbedtls_x509_crt_parse(
        &s_ca_cert,
        (const unsigned char*)s_config.ca_cert_pem,
        s_config.ca_cert_len 
    );
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to parse CA certificate: -0x%04x", -ret);
        ESP_LOG_BUFFER_CHAR(TAG, s_config.ca_cert_pem, s_config.ca_cert_len);
        // Дополнительная диагностика
        if (ret == MBEDTLS_ERR_X509_INVALID_FORMAT) {
            ESP_LOGE(TAG, "Invalid certificate format. Expected PEM (-----BEGIN CERTIFICATE-----)");
        } else if (ret == MBEDTLS_ERR_X509_INVALID_VERSION) {
            ESP_LOGE(TAG, "Invalid certificate version");
        } else if (ret == MBEDTLS_ERR_X509_INVALID_SERIAL) {
            ESP_LOGE(TAG, "Invalid serial number");
        } else if (ret == MBEDTLS_ERR_X509_INVALID_NAME) {
            ESP_LOGE(TAG, "Invalid name");
        } else if (ret == MBEDTLS_ERR_X509_INVALID_DATE) {
            ESP_LOGE(TAG, "Invalid date");
        }
        
        return ESP_FAIL;
    }
    
    // Загрузка клиентского сертификата
    ret = mbedtls_x509_crt_parse(
        &s_client_cert,
        (const unsigned char*)s_config.client_cert_pem,
        s_config.client_cert_len
    );
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to parse client certificate: %d", ret);
        return ESP_FAIL;
    }
    
    // Загрузка приватного ключа (PEM)
    ret = mbedtls_pk_parse_key(
        &s_client_key,
        (const unsigned char*)s_config.client_key_pem,
        s_config.client_key_len,
        (const unsigned char*)password,
        strlen(password),
        NULL,
        NULL
    );

    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to parse client key: %d", ret);
        ESP_LOG_BUFFER_CHAR(TAG, s_config.client_key_pem, s_config.client_key_len);
        if (ret == MBEDTLS_ERR_PK_PASSWORD_REQUIRED) {
            ESP_LOGE(TAG, "Key requires password but none provided");
            ESP_LOGI(TAG, "Either remove password from key:");
            ESP_LOGI(TAG, "  openssl rsa -in client.key -out client_unencrypted.key");
            ESP_LOGI(TAG, "Or provide password in the code");
        } else if (ret == MBEDTLS_ERR_PK_KEY_INVALID_FORMAT) {
            ESP_LOGE(TAG, "Invalid key format");
            ESP_LOGI(TAG, "Convert key to PKCS#1 format:");
            ESP_LOGI(TAG, "  openssl rsa -in client.key -out client_rsa.key");
        }
        return ESP_FAIL;
    }   
    
    
    // ========== ПРОВЕРКА СООТВЕТСТВИЯ КЛЮЧА И СЕРТИФИКАТА ==========
    ret = mbedtls_pk_check_pair(&s_client_cert.pk, &s_client_key, 
                            mbedtls_ctr_drbg_random, &s_ctr_drbg);
    ESP_LOGI(TAG, "Key-cert match check: %d", ret);
    if (ret != 0) {
        ESP_LOGE(TAG, "Key does not match certificate!");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Key matches certificate");
    // Настройка SSL конфигурации
    // После загрузки сертификата и ключа
    ESP_LOGI(TAG, "=== CERTIFICATE/KEY CHECK ===");

    // Проверка типа ключа
    mbedtls_pk_type_t key_type = mbedtls_pk_get_type(&s_client_key);
    ESP_LOGI(TAG, "Key type: %d", key_type);
    if (key_type == MBEDTLS_PK_RSA) {
        ESP_LOGI(TAG, "RSA key");
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(s_client_key);
        ESP_LOGI(TAG, "RSA key size: %d bits", mbedtls_rsa_get_bitlen(rsa));
    } else if (key_type == MBEDTLS_PK_ECKEY) {
        ESP_LOGI(TAG, "EC key");
    } else {
        ESP_LOGE(TAG, "Unknown key type: %d", key_type);
    }    
    mbedtls_ssl_conf_authmode(&s_ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&s_ssl_conf, &s_ca_cert, NULL);
    mbedtls_ssl_conf_own_cert(&s_ssl_conf, &s_client_cert, &s_client_key);
  //  mbedtls_ssl_conf_rng(&s_ssl_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
     //mbedtls_ssl_conf_rng(&s_ssl_conf, fake_rng, NULL);
    ESP_LOGI(TAG, "CA cert len: %d", s_ca_cert.raw.len);
    ESP_LOGI(TAG, "Client cert len: %d", s_client_cert.raw.len);
    ESP_LOGI(TAG, "Key type: %d", mbedtls_pk_get_type(&s_client_key));
    ESP_LOGI(TAG, "Key type: %d", mbedtls_pk_get_type(&s_client_cert.pk));

    if (s_client_cert.raw.len == 0) {
        ESP_LOGE(TAG, "❌ Client certificate is EMPTY!");
    }
        
    mbedtls_ssl_set_timer_cb(&s_ssl, NULL, my_set_timer, my_get_timer);
    // Установка SSL
    ret = mbedtls_ssl_setup(&s_ssl, &s_ssl_conf);
    //test_rng(); // Тестируем RNG после настройки SSL, так как некоторые операции могли его повлиять
    ESP_LOGI(TAG, "mbedtls_ssl_setup returned: %d", ret);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "SSL setup FAILED: -0x%04x", -ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Setting BIO callbacks");
    
    s_bio.send_len = 0;
    s_bio.recv_len = 0;
    mbedtls_ssl_set_bio(&s_ssl, &s_bio, eap_tls_bio_send, eap_tls_bio_recv, NULL);
    
    // Проверка, что BIO установлены
    if (s_ssl.private_f_send == NULL) {
        ESP_LOGE(TAG, "BIO send callback is NULL!");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BIO callbacks set successfully");
    return ESP_OK;
}

// Формирование EAP-Response пакета
static size_t build_eap_packet(
    uint8_t code, 
    uint8_t id, 
    uint8_t type,
    const uint8_t *data,
    size_t data_len,
    uint8_t *output
) {
    size_t offset = 0;
    
    // EAP заголовок
    output[offset++] = code;
    output[offset++] = id;
    
    // Длина (EAP заголовок + данные)
    uint16_t length = 4 + data_len;
    output[offset++] = (length >> 8) & 0xFF;
    output[offset++] = length & 0xFF;
    
    // EAP тип (если есть данные)
    if (data_len > 0 && code == EAP_CODE_RESPONSE) {
        output[offset++] = type;
    }
    
    // Данные
    if (data && data_len > 0) {
        memcpy(output + offset, data, data_len);
        offset += data_len;
    }
    
    return offset;
}

// Обработка EAP-Request/Identity
static esp_err_t handle_eap_identity(const uint8_t *data, size_t len) {
    ESP_LOGI(TAG, "Received EAP-Request/Identity");
    
    // Отправляем Identity
    s_eap_out_len = build_eap_packet(
        EAP_CODE_RESPONSE, 
        s_eap_identifier,
        EAP_TYPE_IDENTITY,
        (const uint8_t*)s_config.identity,
        strlen(s_config.identity),
        s_eap_out
    );
    
    ESP_LOGI(TAG, "Responding with Identity: %s", s_config.identity);
    ESP_LOG_BUFFER_HEX(TAG, s_eap_out, s_eap_out_len);
    s_state = EAP_TLS_IDENTITY_SENT;
    return ESP_OK;
}

static esp_err_t handle_eap_tls(const uint8_t *data, size_t len) {
    if (len < 1) return ESP_FAIL;
    int ret = 0;        
    uint8_t flags = data[0];
    size_t tls_len = len - 5;
    const uint8_t *tls_data = data + 5;
    
    ESP_LOGI(TAG, "EAP-TLS flags: 0x%02X, len: %zu", flags, tls_len);
    
    bool is_start = (flags & 0x20) != 0;
    bool is_fragment = (flags & 0x40) != 0;
    bool has_length = (flags & 0x80) != 0;
    
    // Стартовый пакет
    if (is_start) {
        ESP_LOGI(TAG, "START received, generating ClientHello");
        s_bio.send_len = 0;
        
        mbedtls_ssl_session_reset(&s_ssl);
        
        s_state = EAP_TLS_TLS_START;       
        while (ret == 0)
        {
            ESP_LOGW(TAG, "call handshake");
            ret = mbedtls_ssl_handshake(&s_ssl);
            ESP_LOGI(TAG, "handshake: %d", ret);
            ESP_LOGI(TAG, "BIO send_len: %zu", s_bio.send_len);
            if (ret == 0) {
                if (mbedtls_ssl_is_handshake_over(&s_ssl)) {
                    ESP_LOGI(TAG, "TLS handshake completed successfully");
                    break;  // Выходим из цикла — можно отправлять данные
                }
                // Продолжаем цикл: рукопожатие в процессе
            }
            if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
                if ( s_bio.send_len > 0) {
                    ESP_LOGI(TAG, "✅ ClientHello: %zu bytes", s_bio.send_len);
                    
                    uint8_t *eap_tls_data = malloc(6 + s_bio.send_len);
                    if (eap_tls_data) {
                        eap_tls_data[0] = 0x80;
                        int len = s_bio.send_len;
                        eap_tls_data[1] = (len >> 24) & 0xFF;
                        eap_tls_data[2] = (len >> 16) & 0xFF;
                        eap_tls_data[3] = (len >> 8) & 0xFF;
                        eap_tls_data[4] = (len >> 0) & 0xFF;
                        memcpy(eap_tls_data + 5, s_bio.send_buf, s_bio.send_len);
                        
                        s_eap_out_len = build_eap_packet(
                            EAP_CODE_RESPONSE, 
                            s_eap_identifier,
                            EAP_TYPE_TLS,
                            eap_tls_data, 6 + s_bio.send_len,
                            s_eap_out);
                        free(eap_tls_data);
                        send_eapol_packet();
                    }
                    s_bio.send_len = 0;
                    s_state = EAP_TLS_KEY_EXCHANGE;
                    return ESP_OK;
                }
                return ESP_OK;
            }
        }
    }

    if (tls_len > 0) {
        // Получаем данные TLS от сервера
        if (tls_len + s_tls_in_len <= sizeof(s_tls_in)) {
            memcpy(s_tls_in + s_tls_in_len, tls_data, tls_len);
            s_tls_in_len += tls_len;
            ESP_LOGI(TAG, "Received TLS data: %zu bytes, total in buffer: %zu bytes", tls_len, s_tls_in_len);
        } else {
            ESP_LOGE(TAG, "TLS input buffer overflow! Received: %zu bytes, available space: %zu bytes", tls_len, sizeof(s_tls_in) - s_tls_in_len);
            s_tls_in_len = 0;  // Сброс буфера при переполнении для предотвращения некорректной обработки
            s_state = EAP_TLS_FAILED;
            eap_tls_supplicant_start();  // Перезапуск процесса аутентификации
            return ESP_FAIL;
        }
        
        if (s_state == EAP_TLS_KEY_EXCHANGE) {
            if (is_fragment) {
                ESP_LOGI(TAG, "Received TLS fragment, waiting for more data...");
                uint8_t flags = 0x00;  // Fragment flag
                s_eap_out_len = build_eap_packet(
                    EAP_CODE_RESPONSE, 
                    s_eap_identifier,
                    EAP_TYPE_TLS,
                    &flags, 1,
                    s_eap_out);
                send_eapol_packet();
                return ESP_OK;
            }
            ESP_LOGI(TAG, "Received complete TLS message, processing...");       
            ESP_LOG_BUFFER_HEX(TAG, s_tls_in, s_tls_in_len); 
            ESP_LOGW(TAG, "call handshake");
            ret = 0;
            while (ret == 0) {
                ret = mbedtls_ssl_handshake_step(&s_ssl);
                ESP_LOGI(TAG, "handshake: %d", ret);
                ESP_LOGI(TAG, "BIO send_len: %zu", s_bio.send_len);
                if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
                    if ( s_bio.send_len > 0) {
                        ESP_LOGI(TAG, "✅ ClientHello: %zu bytes", s_bio.send_len);
                     
                        uint8_t *eap_tls_data = malloc(6 + s_bio.send_len);
                        if (eap_tls_data) {
                            eap_tls_data[0] = 0x80;
                            int len = s_bio.send_len;
                            eap_tls_data[1] = (len >> 24) & 0xFF;
                            eap_tls_data[2] = (len >> 16) & 0xFF;
                            eap_tls_data[3] = (len >> 8) & 0xFF;
                            eap_tls_data[4] = (len >> 0) & 0xFF;
                            memcpy(eap_tls_data + 5, s_bio.send_buf, s_bio.send_len);
                            
                            s_eap_out_len = build_eap_packet(
                                EAP_CODE_RESPONSE, 
                                s_eap_identifier,
                                EAP_TYPE_TLS,
                                eap_tls_data, 6 + s_bio.send_len,
                                s_eap_out);
                            send_eapol_packet();
                            free(eap_tls_data);
                        }
                        s_bio.send_len = 0;
                        s_tls_in_len = 0;  // Очистить входной буфер после обработки
                        s_state = EAP_TLS_CHANGE_CIPHER_SPEC; 
                        return ESP_OK;
                    }
                }
            }
        }
        if (s_state == EAP_TLS_CHANGE_CIPHER_SPEC) {
            ESP_LOGI(TAG, "Processing post-handshake TLS messages...");
            // Здесь можно обрабатывать сообщения после завершения рукопожатия
            ret = 0;
            while (ret == 0) {
                ret = mbedtls_ssl_handshake_step(&s_ssl);
                ESP_LOGI(TAG, "handshake: %d", ret);
                ESP_LOGI(TAG, "BIO send_len: %zu", s_bio.send_len);
                     if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
                    if ( s_bio.send_len > 0) {
                        ESP_LOGI(TAG, "✅ ClientHello: %zu bytes", s_bio.send_len);
                     
                        uint8_t *eap_tls_data = malloc(6 + s_bio.send_len);
                        if (eap_tls_data) {
                            eap_tls_data[0] = 0x80;
                            int len = s_bio.send_len;
                            eap_tls_data[1] = (len >> 24) & 0xFF;
                            eap_tls_data[2] = (len >> 16) & 0xFF;
                            eap_tls_data[3] = (len >> 8) & 0xFF;
                            eap_tls_data[4] = (len >> 0) & 0xFF;
                            memcpy(eap_tls_data + 5, s_bio.send_buf, s_bio.send_len);
                            
                            s_eap_out_len = build_eap_packet(
                                EAP_CODE_RESPONSE, 
                                s_eap_identifier,
                                EAP_TYPE_TLS,
                                eap_tls_data, 6 + s_bio.send_len,
                                s_eap_out);
                            send_eapol_packet();
                            free(eap_tls_data);
                        }
                        s_bio.send_len = 0;
                        s_tls_in_len = 0;  // Очистить входной буфер после обработки
                        s_state = EAP_TLS_CHANGE_CIPHER_SPEC; 
                        return ESP_OK;
                    }
                }
            }
            ESP_LOGI(TAG, "TLS handshake completed successfully");
            s_state = EAP_TLS_AUTHENTICATED;
            return ESP_OK;
        }
        if (s_state == EAP_TLS_AUTHENTICATED) {
            ESP_LOGI(TAG, "Device is authenticated! Ready to send/receive application data.");
             // Здесь можно начать обмен данными по защищённому каналу
            uint8_t flags = 0x00;  // No flags for application data
            s_eap_out_len = build_eap_packet(
                EAP_CODE_RESPONSE, 
                s_eap_identifier,
                EAP_TYPE_TLS,
                &flags, 1,
                s_eap_out);
            send_eapol_packet(); 
        }
    }    
    return ESP_OK;
}

// Отправка broadcast Ethernet-кадра с кастомным EtherType
void send_raw_ethernet_frame(
    const uint8_t* payload, 
    size_t size
) {
    esp_eth_handle_t eth_handle = ethernet_get_handle();
    if (eth_handle == NULL) {
        ESP_LOGE(TAG, "Ethernet handle is not initialized");
        return;
    }

    // Буфер для Ethernet-кадра (без преамбулы и CRC - их добавит драйвер)
    for (int i = 0; i < 1600; i++) {
        frame[i] = 0;
    }
    // Заполняем MAC-адреса
    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    //uint8_t src_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}; // Замените на реальный MAC вашего устройства
    //esp_efuse_mac_get_default(src_mac);  // получить MAC ESP32
    
    memcpy(frame, broadcast_mac, 6);      // Destination MAC
    memcpy(frame + 6, custom_mac, 6);        // Source MAC
    
    uint16_t ethertype = htons(0x888E);

    memcpy(frame + 12, &ethertype, 2);
    
    // Данные (payload)
    size_t payload_len = size;
    memcpy(frame + 14, payload, payload_len);
    
    // Общая длина кадра: MAC(12) + EtherType(2) + payload
    size_t frame_len = 14 + payload_len;
    
    ESP_LOGI(TAG, "Prepared Ethernet frame: %zu bytes", frame_len);
    ESP_LOG_BUFFER_HEX(TAG, frame, frame_len);
    // Отправка через Ethernet драйвер
    esp_err_t err = esp_eth_transmit(eth_handle, frame, frame_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet frame sent successfully");
    } else {
        ESP_LOGI(TAG, "Failed to send frame: 0x%04x", err);
    }
}

// Отправка EAPoL пакета
static esp_err_t send_eapol_packet(void) {
     
    // Формируем EAPoL заголовок (EtherType 0x888E)
    uint8_t eapol_frame[1600];
    size_t offset = 0;
    
    // EAPoL версия и тип
    eapol_frame[offset++] = 0x01; // Version
    eapol_frame[offset++] = 0x00; // Type: EAP Packet
    
    // Длина EAP пакета
    uint16_t eap_len = s_eap_out_len;
    eapol_frame[offset++] = (eap_len >> 8) & 0xFF;
    eapol_frame[offset++] = eap_len & 0xFF;
    
    // Копируем EAP пакет
    memcpy(eapol_frame + offset, s_eap_out, eap_len);
    offset += eap_len;
    
    ESP_LOGI(TAG, "Sending EAPoL packet, length: %zu", offset);
    send_raw_ethernet_frame(eapol_frame, offset);
    int sent = offset;
    if (sent != offset) {
        ESP_LOGE(TAG, "Failed to send EAPoL packet: %d", errno);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Sent EAPoL packet, len: %zu", offset);
    return ESP_OK;
}

/**
 * @brief Callback-функция для приема кадров через L2TAP интерфейс.
 * @param buffer Указатель на данные принятого кадра (начиная с заголовка Ethernet).
 * @param len Длина кадра.
 * @param ctx Пользовательский контекст (можно передать NULL).
 */
void eap_frame_handler(void *buffer, size_t len, void *ctx)
{
    uint8_t *frame = (uint8_t *)buffer;

    // Базовая проверка: в кадре должен быть хотя бы заголовок (14 байт)
    if (len < 14) {
        ESP_LOGW(TAG, "Received frame is too short (%zu bytes)", len);
        return;
    }

    // Извлекаем EtherType из кадра (байты 12 и 13)
    uint16_t ethertype = (frame[12] << 8) | frame[13];
    ESP_LOGI(TAG, "Received frame with EtherType: 0x%04X, length: %zu", ethertype, len);

    // Проверяем, что это EAP
    if (ethertype == 0x888E) {
        // Данные начинаются с 14-го байта
        uint8_t *payload = frame + 14;
        size_t payload_len = len - 14;

        ESP_LOGI(TAG, "Received our custom frame! Payload length: %zu", payload_len);
        
        if (payload_len > 0) {
            // Проверяем минимальную длину EAPoL пакета
            if (payload_len < 4) {
                return;
            }
            
            uint8_t version = payload[0];
            uint8_t type = payload[1];
            uint16_t eap_len = (payload[2] << 8) | payload[3];
            
            if (type != 0x00) {
                return; // Не EAP пакет
            }
            
            if (len < 4 + eap_len) {
                return;
            }
            
            // Обрабатываем EAP пакет
            uint8_t *eap_packet = payload + 4;
            uint8_t eap_code = eap_packet[0];
            uint8_t eap_id = eap_packet[1];
    
            ESP_LOGI(TAG, "Received EAP packet: code=%d, id=%d, length=%d", eap_code, eap_id, eap_len);
            
            // Сохраняем идентификатор для ответа
            s_eap_identifier = eap_id;
            
            if (eap_code == EAP_CODE_REQUEST) {
                ESP_LOGI(TAG, "Received EAP-Request");
                uint8_t eap_type = eap_packet[4];
                
                switch (eap_type) {
                    case EAP_TYPE_IDENTITY:
                        handle_eap_identity(eap_packet + 5, eap_len - 5);
                        send_eapol_packet();
                        break;
                        
                    case EAP_TYPE_TLS:
                        handle_eap_tls(eap_packet + 5, eap_len - 5);
                        break;
                        
                    default:
                        ESP_LOGW(TAG, "Unsupported EAP type: %d", eap_type);
                        break;
                }
            } else if (eap_code == EAP_CODE_SUCCESS) {
                ESP_LOGI(TAG, "EAP-Success received!");
                s_authenticated = true;
                s_state = EAP_TLS_AUTHENTICATED;
            } else if (eap_code == EAP_CODE_FAILURE) {
                ESP_LOGE(TAG, "EAP-Failure received!");
                s_authenticated = false;
                s_state = EAP_TLS_FAILED;
            }
        }
    }

}

// Публичные функции
esp_err_t eap_tls_supplicant_init(const eap_tls_config_t *config) {
    if (!config || !config->ca_cert_pem || !config->client_cert_pem || !config->client_key_pem) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&s_config, config, sizeof(eap_tls_config_t));
  
    // Инициализация TLS
    ESP_ERROR_CHECK(tls_init());
        
    return ESP_OK;
}

esp_err_t eap_tls_supplicant_start(void) {
    if (s_state != EAP_TLS_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting EAP-TLS authentication");
    // Ждём поднятия Ethernet линка, но не спамим логом каждые 100 мс.
    int wait_log_divider = 0;
    while (!ethernet_is_link_up()) {
        if (wait_log_divider == 0) {
            ESP_LOGI(TAG, "Waiting for Ethernet link...");
        }
        wait_log_divider = (wait_log_divider + 1) % 50; // лог примерно раз в 5 секунд
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Отправляем EAPoL-Start
    uint8_t eapol_start[4] = {0x01, 0x01, 0x00, 0x00};
    send_raw_ethernet_frame(eapol_start, 4);
    s_state = EAP_TLS_START;
    
    return ESP_OK;
}

void eap_tls_supplicant_stop(void) {
    s_state = EAP_TLS_IDLE;
    s_authenticated = false;
}

bool eap_tls_supplicant_is_authenticated(void) {
    return s_authenticated;
}