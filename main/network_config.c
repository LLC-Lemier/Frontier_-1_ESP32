#include "network_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "ethernet_init.h"
#include "lwip/ip4_addr.h"
#include "nvs_config.h"
#include "spiffs_driver.h"

static const char *TAG = "NET_CFG";
static network_config_t s_saved_config;
static eap_tls_config_t s_saved_eap_tls_config; 
static ntp_config_t s_saved_ntp_config;
static char* ca_cert_pem = NULL;
static char* client_cert_pem = NULL;
static char* client_key_pem = NULL; 

static esp_err_t load_eap_tls_config_from_spiffs(eap_tls_config_t *config)
{
    FILE* f_ca = fopen("/spiffs/certs/ca.pem", "rb");
    FILE* f_client_cert = fopen("/spiffs/certs/client.crt", "rb");
    FILE* f_client_key = fopen("/spiffs/certs/client.key", "rb");

    if (!f_ca || !f_client_cert || !f_client_key) {
        ESP_LOGW(TAG, "One or more EAP-TLS cert files not found in SPIFFS");
        if (f_ca) fclose(f_ca);
        if (f_client_cert) fclose(f_client_cert);
        if (f_client_key) fclose(f_client_key);
        return ESP_OK; // Не критично, просто продолжим без EAP-TLS конфига
    }

    fseek(f_ca, 0, SEEK_END);
    size_t ca_len = ftell(f_ca);
    fseek(f_ca, 0, SEEK_SET);

    fseek(f_client_cert, 0, SEEK_END);
    size_t client_cert_len = ftell(f_client_cert);
    fseek(f_client_cert, 0, SEEK_SET);

    fseek(f_client_key, 0, SEEK_END);
    size_t client_key_len = ftell(f_client_key);
    fseek(f_client_key, 0, SEEK_SET);

    ca_cert_pem = malloc(ca_len + 1);
    client_cert_pem = malloc(client_cert_len + 1);
    client_key_pem = malloc(client_key_len + 1);
    config->ca_cert_pem = ca_cert_pem;
    config->client_cert_pem = client_cert_pem;
    config->client_key_pem = client_key_pem;
    config->ca_cert_len = ca_len;
    config->client_cert_len = client_cert_len;
    config->client_key_len = client_key_len;

    if (config->ca_cert_pem && config->client_cert_pem && config->client_key_pem) {
        fread(ca_cert_pem, 1, ca_len, f_ca);
        fread(client_cert_pem, 1, client_cert_len, f_client_cert);
        fread(client_key_pem, 1, client_key_len, f_client_key);

        ca_cert_pem[ca_len] = '\0';
        client_cert_pem[client_cert_len] = '\0';
        client_key_pem[client_key_len] = '\0';

        return ESP_OK;
    } 
    ESP_LOGE(TAG, "Failed to allocate memory for EAP-TLS certs");
    if (ca_cert_pem) free(ca_cert_pem);
    if (client_cert_pem) free(client_cert_pem);
    if (client_key_pem) free(client_key_pem);
    return ESP_ERR_NO_MEM;
}

static esp_err_t set_dns_if_present(esp_netif_t *netif, esp_netif_dns_type_t type, uint32_t dns_addr)
{
    if (dns_addr == 0) {
        return ESP_OK;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = dns_addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    return esp_netif_set_dns_info(netif, type, &dns);
}

esp_err_t network_config_init(void)
{
    ESP_RETURN_ON_ERROR(mount_spiffs(), TAG, "failed to mount spiffs");
    ESP_RETURN_ON_ERROR(nvs_config_init(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(
        nvs_config_load_network(&s_saved_config, &s_saved_eap_tls_config, &s_saved_ntp_config),
        TAG, 
        "failed to load network config from NVS"
    );

    if (s_saved_config.eap_tls_config == NULL) {
        ESP_LOGI(TAG, "No EAP-TLS config found in NVS");
    } else {
        ESP_LOGI(
            TAG,
            "EAP-TLS config loaded from NVS with timeout %d ms and max retries %d",
            s_saved_eap_tls_config.timeout_ms, 
            s_saved_eap_tls_config.max_retries
        );
        esp_err_t ret = load_eap_tls_config_from_spiffs(&s_saved_eap_tls_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load EAP-TLS certs from SPIFFS: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t network_config_load(network_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_saved_config;
    return ESP_OK;
}

esp_err_t network_config_save(const network_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    } 
    s_saved_config = *config;
    return nvs_config_save_network(config, config->eap_tls_config, config->ntp_config);
}

esp_err_t network_config_apply(const network_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = ethernet_get_netif();
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;
    if (config->dhcp_enabled) {
        ret = esp_netif_dhcpc_start(netif);
        if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGI(TAG, "DHCP клиент уже запущен");
        } else if (ret == ESP_OK) {
            ESP_LOGI(TAG, "DHCP клиент успешно запущен");
        } else {
            ESP_LOGE(TAG, "Ошибка запуска DHCP клиента: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "Applied DHCP mode");
        return ESP_OK;
    }

    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
    ret = esp_netif_dhcpc_get_status(netif, &dhcp_status);
    if (ret == ESP_OK && dhcp_status != ESP_NETIF_DHCP_STOPPED) {
        ret = esp_netif_dhcpc_stop(netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_RETURN_ON_ERROR(ret, TAG, "failed to stop DHCP");
        }
    }

    esp_netif_ip_info_t ip_info = {
        .ip.addr = config->ip,
        .netmask.addr = config->netmask,
        .gw.addr = config->gateway,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip_info), TAG, "set ip info failed");
    ESP_RETURN_ON_ERROR(set_dns_if_present(netif, ESP_NETIF_DNS_MAIN, config->dns1), TAG, "set dns1 failed");
    ESP_RETURN_ON_ERROR(set_dns_if_present(netif, ESP_NETIF_DNS_BACKUP, config->dns2), TAG, "set dns2 failed");

    ESP_LOGI(TAG, "Applied static IP config");

    init_ntp(config->ntp_config);

    start_ntp_sync_task();
    
    return ESP_OK;
}

esp_err_t network_config_apply_saved(void) //конфиг из NVS
{
    return network_config_apply(&s_saved_config);
}

esp_err_t network_config_get_runtime(network_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));  // чистка памяти под структуру
    esp_netif_t *netif = ethernet_get_netif();
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
    if (esp_netif_dhcpc_get_status(netif, &dhcp_status) == ESP_OK) {
        config->dhcp_enabled = (dhcp_status != ESP_NETIF_DHCP_STOPPED);
    }

    //config->dhcp_enabled = s_saved_config->dhcp_enabled; 

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        config->ip = info.ip.addr;
        config->netmask = info.netmask.addr;
        config->gateway = info.gw.addr;
    }

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        config->dns1 = dns.ip.u_addr.ip4.addr;
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        config->dns2 = dns.ip.u_addr.ip4.addr;
    }

    
    if (s_saved_config.eap_tls_config) {
        config->eap_tls_config = &s_saved_eap_tls_config;
    }

    if (s_saved_config.ntp_config) {
        config->ntp_config = &s_saved_ntp_config;
    }

    return ESP_OK;
}

network_config_t* network_config_get_saved(void)
{
    return &s_saved_config;
}