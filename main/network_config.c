#include "network_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "ethernet_init.h"
#include "lwip/ip4_addr.h"
#include "nvs_config.h"

static const char *TAG = "NET_CFG";
static network_config_t s_saved_config;

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
    ESP_RETURN_ON_ERROR(nvs_config_init(), TAG, "nvs init failed");
    return nvs_config_load_network(&s_saved_config);
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
    return nvs_config_save_network(config);
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
            ret = ESP_OK;
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to start DHCP");
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
    return ESP_OK;
}

esp_err_t network_config_apply_saved(void). //конфиг из NVS
{
    return network_config_apply(&s_saved_config);
}

esp_err_t network_config_get_runtime(network_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    esp_netif_t *netif = ethernet_get_netif();
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
    if (esp_netif_dhcpc_get_status(netif, &dhcp_status) == ESP_OK) {
        config->dhcp_enabled = (dhcp_status != ESP_NETIF_DHCP_STOPPED);
    }

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

    return ESP_OK;
}
