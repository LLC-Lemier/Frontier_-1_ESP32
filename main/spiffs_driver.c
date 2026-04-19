#include "spiffs_driver.h"
#include "esp_spiffs.h"

static bool is_spiffs_mounted = false; 

esp_err_t mount_spiffs(void) // раздел фронта
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
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        is_spiffs_mounted = true;
    }
    return ret;
}
