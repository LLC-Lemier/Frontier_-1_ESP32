#ifndef EAP_TLS_SUPPLICANT_H
#define EAP_TLS_SUPPLICANT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_eth_driver.h"


// EAP коды
#define EAP_CODE_REQUEST      1
#define EAP_CODE_RESPONSE     2
#define EAP_CODE_SUCCESS      3
#define EAP_CODE_FAILURE      4

// EAP типы
#define EAP_TYPE_IDENTITY     1
#define EAP_TYPE_TLS          13

#define MBEDTLS_DEBUG_C 
// Состояния EAP-TLS
typedef enum {
    EAP_TLS_IDLE,
    EAP_TLS_START,
    EAP_TLS_IDENTITY_SENT,
    EAP_TLS_TLS_START,
    EAP_TLS_KEY_EXCHANGE,
    EAP_TLS_CHANGE_CIPHER_SPEC,
    EAP_TLS_WAITING_ACCESS_CHALLENGE,
    EAP_TLS_AUTHENTICATED,
    EAP_TLS_FAILED
} eap_tls_state_t;

// eap_tls_supplicant.h
typedef struct {
    uint8_t send_buf[4096];
    size_t send_len;
    uint8_t recv_buf[4096];
    size_t recv_len;
} eap_tls_bio_t;

// Конфигурация Supplicant
typedef struct {
    char identity[64];           // Идентификатор клиента
    const char *ca_cert_pem;     // CA сертификат
    size_t ca_cert_len;          // Длина CA сертификата
    const char *client_cert_pem; // Клиентский сертификат
    size_t client_cert_len;      // Длина клиентского сертификата
    const char *client_key_pem;  // Приватный ключ клиента
    size_t client_key_len;       // Длина приватного ключа
    uint32_t timeout_ms;         // Таймаут EAP обмена
    uint8_t max_retries;         // Максимальное число попыток
} eap_tls_config_t;

// Инициализация EAP-TLS supplicant
esp_err_t eap_tls_supplicant_init(const eap_tls_config_t *config);

// Запуск аутентификации
esp_err_t eap_tls_supplicant_start(void);

// Остановка аутентификации
void eap_tls_supplicant_stop(void);

// Получение статуса аутентификации
bool eap_tls_supplicant_is_authenticated(void);

// Обработка входящего Ethernet/EAPoL кадра
extern void eap_frame_handler(void *buffer, size_t len, void *ctx);

#endif
