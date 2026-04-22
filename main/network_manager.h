/*
 * network_manager — TCP/UDP мост к uart_manager (ТЗ: network_manager).
 *
 * Версия модуля (ПО): 0.2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "uart_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_MANAGER_MODULE_VERSION "0.2.0"

/** Число физических COM, с которыми работает сборка (Kconfig). */
#define NETWORK_MGR_PHYSICAL_COM_PORTS CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS

typedef enum {
    NM_MODE_OFF = 0,
    NM_MODE_TCP_SERVER = 1,
    NM_MODE_TCP_CLIENT = 2,
    NM_MODE_UDP = 3,
} nm_socket_mode_t;

typedef struct {
    nm_socket_mode_t mode;
    uint16_t local_tcp_udp_port;
    uint16_t remote_port;
    char remote_ip[16];
    uint32_t tcp_keepalive_idle_sec;
    bool tcp_nodelay;
} nm_port_net_config_t;

typedef enum {
    NM_CMD_APPLY_CONFIG = 0,
    NM_CMD_STOP_PORT,
    NM_CMD_SHUTDOWN,
    NM_CMD_LOAD_AND_APPLY_ALL,
    /** Закрыть/открыть сокеты по текущему RAM-конфигу (после ETH_GOT_IP, без чтения NVS). */
    NM_CMD_REAPPLY_RUNNING,
    /** Только закрыть сокеты (ETH_LOST_IP), конфиг и NVS не менять. */
    NM_CMD_LINK_DOWN,

    NM_CMD_LOAD_CONFIG_FROM_NVS,
} nm_cmd_type_t;

typedef struct {
    nm_cmd_type_t type;
    uart_port_id_t port_id;
    bool apply_uart_line;
    uart_manager_port_config_t uart_cfg;
    nm_port_net_config_t net_cfg;
} nm_command_t;

typedef struct {
    uint32_t socket_rx_bytes;
    uint32_t socket_tx_bytes;
    uint32_t uart_rx_to_socket_bytes;
    uint8_t tcp_client_count;
} nm_port_stats_t;

uint8_t network_manager_get_physical_com_count(void);

esp_err_t network_manager_init(void);
void network_manager_deinit(void);

/**
 * Загрузить сетевую конфигурацию из NVS в RAM и применить (очередь → задача).
 * Вызывать после готовности IP (например после EAP и network_config_apply_saved).
 */
esp_err_t network_manager_start(void);

bool network_manager_post_command(const nm_command_t *cmd, TickType_t ticks_to_wait);

esp_err_t network_manager_get_stats(uart_port_id_t port_id, nm_port_stats_t *out_stats);



#if defined(CONFIG_NETWORK_MGR_ENABLE_TEST_TASK) && CONFIG_NETWORK_MGR_ENABLE_TEST_TASK
void network_mgr_test_task(void *arg);
#endif

#ifdef __cplusplus
}
#endif
