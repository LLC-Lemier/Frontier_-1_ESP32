idf_component_register(
    SRCS
        "authentication.c"
        "eap_tls_supplicant.c"
        "ethernet_init.c"
        "https_server.c"
        "main.c"
        "network_config.c"
        "ntp_adapter.c"
        "nvs_config.c"
        "spiffs_driver.c"
        "web_api.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        esp_https_server
        esp_timer
        esp_driver_gpio
        esp_netif
        esp_eth
        nvs_flash
        spiffs
        json
)

#set(CERT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/certs)

#target_add_binary_data(${COMPONENT_TARGET} "${CERT_DIR}/ca.pem" TEXT)
#target_add_binary_data(${COMPONENT_TARGET} "${CERT_DIR}/client.crt" TEXT)
#target_add_binary_data(${COMPONENT_TARGET} "${CERT_DIR}/client.key" TEXT)
#target_add_binary_data(${COMPONENT_TARGET} "${CERT_DIR}/server.crt" TEXT)
#target_add_binary_data(${COMPONENT_TARGET} "${CERT_DIR}/server.key" TEXT)

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
