/*
 * network_manager — TCP/UDP мост к uart_manager (ТЗ: network_manager).
 *
 * Версия модуля (ПО): 0.2.0
 */

#include "network_manager.h"

#include "sdkconfig.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"    
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NM_TAG "network_mgr"

#define NM_NVS_NAMESPACE "net_mgr"
#define NM_NVS_KEY_CFG "cfg_v1"
#define NM_NVS_MAGIC 0x4e4d4731u
#define NM_NVS_STRUCT_VERSION 1

#define NM_MAX_TCP_CLIENTS 5
#define NM_TCP_TX_BACKLOG 512
#define NM_NET2UART_BACKLOG 4096

typedef struct {
    uint8_t data[NM_TCP_TX_BACKLOG];
    size_t len;
} nm_tx_pending_t;

typedef struct __attribute__((packed)) {
    uint8_t mode;
    uint16_t local_port;
    uint16_t remote_port;
    char remote_ip[16];
    uint32_t keepalive_sec;
    uint8_t nodelay;
} nm_stored_net_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t struct_version;
    uint8_t physical_ports;
    uint8_t reserved;
    nm_stored_net_t ports[16];
} nm_nvs_blob_t;

typedef struct {
    int listen_fd;
    int client_fds[NM_MAX_TCP_CLIENTS];
    int udp_fd;
    int tcp_client_fd;
    nm_port_net_config_t cfg;
} nm_port_runtime_t;

static QueueHandle_t s_cmd_q;
static SemaphoreHandle_t s_stats_mtx;
static TaskHandle_t s_nm_task;
static SemaphoreHandle_t s_nm_task_exit_sem;
static volatile bool s_nm_task_run;
static nm_port_runtime_t s_ports[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS];
static nm_port_stats_t s_stats[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS];
static TickType_t s_tcp_cli_last_try[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS];
/** Отложенная отправка UART→TCP на сокет (non-blocking send); по одному буферу на каждого TCP-клиента сервера. */
static nm_tx_pending_t s_tcp_srv_tx[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS][NM_MAX_TCP_CLIENTS];
static nm_tx_pending_t s_tcp_cli_tx[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS];
/** TCP/UDP → UART: если TX-кольцо полное, байты здесь; иначе nm_task зависает в while (uart_manager_send). */
static uint8_t s_tcp_to_uart_pending[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS][NM_NET2UART_BACKLOG];
static size_t s_tcp_to_uart_pend_len[CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS];
/** Для nm_tcp_queue_and_send(..., src, 0): валидный src при нулевой длине (GCC -Wmaybe-uninitialized). */
static const uint8_t s_nm_tcp_zero_len_placeholder[1] = { 0 };
static bool s_inited;
static bool s_shut_down;

static void nm_close_port_sockets_unlocked(uart_port_id_t port_id);
static esp_err_t nm_open_port_sockets_unlocked(uart_port_id_t port_id);
static void nm_apply_command(const nm_command_t *cmd);
static void nm_task(void *arg);
static esp_err_t nm_load_nvs(void);
static esp_err_t nm_save_nvs(void);
static void nm_defaults_all(void);
static void nm_reapply_all_from_ram(void);
static void nm_ip_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);

#if CONFIG_NETWORK_MGR_VERBOSE_LOG
#define NM_LOGD(fmt, ...) ESP_LOGD(NM_TAG, fmt, ##__VA_ARGS__)
#define NM_LOGI_V(fmt, ...) ESP_LOGI(NM_TAG, fmt, ##__VA_ARGS__)
#else
#define NM_LOGD(fmt, ...) ((void)0)
#define NM_LOGI_V(fmt, ...) ((void)0)
#endif

uint8_t network_manager_get_physical_com_count(void)
{
    return (uint8_t)CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS;
}

static bool nm_port_id_valid(uart_port_id_t port_id)
{
    return port_id < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS;
}

static void nm_stats_reset_port(uart_port_id_t p)
{
    memset(&s_stats[p], 0, sizeof(s_stats[p]));
}

static void nm_init_runtime(void)
{
    memset(s_ports, 0, sizeof(s_ports));
    memset(s_stats, 0, sizeof(s_stats));
    memset(s_tcp_srv_tx, 0, sizeof(s_tcp_srv_tx));
    memset(s_tcp_cli_tx, 0, sizeof(s_tcp_cli_tx));
    memset(s_tcp_to_uart_pending, 0, sizeof(s_tcp_to_uart_pending));
    memset(s_tcp_to_uart_pend_len, 0, sizeof(s_tcp_to_uart_pend_len));
    memset(s_tcp_cli_last_try, 0, sizeof(s_tcp_cli_last_try));
    for (int i = 0; i < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; i++) {
        s_ports[i].listen_fd = -1;
        s_ports[i].udp_fd = -1;
        s_ports[i].tcp_client_fd = -1;
        for (int j = 0; j < NM_MAX_TCP_CLIENTS; j++) {
            s_ports[i].client_fds[j] = -1;
        }
        s_ports[i].cfg.mode = NM_MODE_OFF;
        s_ports[i].cfg.local_tcp_udp_port = 0;
        s_ports[i].cfg.remote_port = 0;
        s_ports[i].cfg.remote_ip[0] = '\0';
        s_ports[i].cfg.tcp_keepalive_idle_sec = 60;
        s_ports[i].cfg.tcp_nodelay = true;
    }
}

static void nm_tcp_apply_sockopts(int fd, const nm_port_net_config_t *cfg)
{
    int yes = 1;
    if (cfg->tcp_nodelay) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    }
    if (cfg->tcp_keepalive_idle_sec > 0) {
        int idle = (int)cfg->tcp_keepalive_idle_sec;
#if defined(TCP_KEEPIDLE)
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
    }
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
}

static int nm_sock_set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Отправка UART→TCP без блокировки nm_task: сокет O_NONBLOCK, остаток в *b до следующих итераций.
 * Порядок байт сохраняется (сначала досылается backlog, затем новые данные).
 */
static uint32_t nm_tcp_queue_and_send(uart_port_id_t port_id, int fd, nm_tx_pending_t *b, const uint8_t *src,
                                      size_t src_len)
{
    uint32_t sent_acc = 0;
    const size_t cap = sizeof(b->data);
    if (fd < 0) {
        b->len = 0;
        return 0;
    }
    size_t off = 0;
    while (b->len > 0) {
        ssize_t w = send(fd, b->data, b->len, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                goto append_src_to_backlog;
            }
            b->len = 0;
            return sent_acc;
        }
        if (w == 0) {
            break;
        }
        sent_acc += (uint32_t)w;
        if ((size_t)w < b->len) {
            memmove(b->data, b->data + w, b->len - (size_t)w);
            b->len -= (size_t)w;
            goto append_src_to_backlog;
        }
        b->len = 0;
    }
    while (off < src_len) {
        ssize_t w = send(fd, src + off, src_len - off, 0);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                goto append_src_to_backlog;
            }
            return sent_acc;
        }
        if (w == 0) {
            break;
        }
        sent_acc += (uint32_t)w;
        off += (size_t)w;
    }
    return sent_acc;

append_src_to_backlog:
{
    size_t rem = src_len - off;
    size_t room = (b->len < cap) ? (cap - b->len) : 0;
    if (rem > room) {
        memcpy(b->data + b->len, src + off, room);
        b->len = cap;
    } else {
        memcpy(b->data + b->len, src + off, rem);
        b->len += rem;
    }
    return sent_acc;
}
}

static void nm_flush_tcp_to_uart_pending(uart_port_id_t port_id)
{
    if (!nm_port_id_valid(port_id)) {
        return;
    }
    while (s_tcp_to_uart_pend_len[port_id] > 0) {
        size_t w =
            uart_manager_send(port_id, s_tcp_to_uart_pending[port_id], s_tcp_to_uart_pend_len[port_id], 0);
        if (w == 0) {
            break;
        }
        if (w < s_tcp_to_uart_pend_len[port_id]) {
            memmove(s_tcp_to_uart_pending[port_id], s_tcp_to_uart_pending[port_id] + w,
                    s_tcp_to_uart_pend_len[port_id] - w);
        }
        s_tcp_to_uart_pend_len[port_id] -= w;
    }
}

/** Данные с сокета (TCP recv / UDP) → UART: не блокировать nm_task при полном TX-кольце. */
static void nm_stream_net_to_uart(uart_port_id_t port_id, const uint8_t *data, size_t len)
{
    if (!nm_port_id_valid(port_id) || data == NULL || len == 0) {
        return;
    }

    /* Сначала попытаться слить ранее отложенные байты и сразу отправить новый фрагмент в UART. */
    nm_flush_tcp_to_uart_pending(port_id);
    size_t sent_now = uart_manager_send(port_id, data, len, 0);
    size_t rem = len - sent_now;
    if (rem == 0) {
        return;
    }

    const uint8_t *tail = data + sent_now;
    const size_t cap = sizeof(s_tcp_to_uart_pending[port_id]);
    size_t room = cap - s_tcp_to_uart_pend_len[port_id];
    size_t put = (rem <= room) ? rem : room;
    if (put > 0) {
        memcpy(s_tcp_to_uart_pending[port_id] + s_tcp_to_uart_pend_len[port_id], tail, put);
        s_tcp_to_uart_pend_len[port_id] += put;
    }
    if (put < rem) {
        ESP_LOGW(NM_TAG, "port %u TCP/UDP→UART pending overflow drop %u B", (unsigned)port_id,
                 (unsigned)(rem - put));
    }
    nm_flush_tcp_to_uart_pending(port_id);
}

static void nm_close_port_sockets_unlocked(uart_port_id_t port_id)
{
    memset(s_tcp_srv_tx[port_id], 0, sizeof(s_tcp_srv_tx[port_id]));
    s_tcp_cli_tx[port_id].len = 0;
    s_tcp_to_uart_pend_len[port_id] = 0;
    nm_port_runtime_t *rt = &s_ports[port_id];
    if (rt->listen_fd >= 0) {
        close(rt->listen_fd);
        rt->listen_fd = -1;
    }
    for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
        if (rt->client_fds[i] >= 0) {
            close(rt->client_fds[i]);
            rt->client_fds[i] = -1;
        }
    }
    if (rt->udp_fd >= 0) {
        close(rt->udp_fd);
        rt->udp_fd = -1;
    }
    if (rt->tcp_client_fd >= 0) {
        close(rt->tcp_client_fd);
        rt->tcp_client_fd = -1;
    }
}

static const char *nm_uart_line_mode_str(uart_mgr_line_mode_t m)
{
    switch (m) {
    case UART_MGR_LINE_RS232:
        return "RS232";
    case UART_MGR_LINE_RS485:
        return "RS485";
    case UART_MGR_LINE_RS422:
        return "RS422";
    default:
        return "?";
    }
}

/** Логирует текущие настройки UART для порта моста (то, что реально в uart_manager). */
static void nm_log_uart_settings_for_port(uart_port_id_t port_id)
{
    uart_manager_port_config_t uc;
    esp_err_t e = uart_manager_get_port_config(port_id, &uc);
    if (e != ESP_OK) {
        ESP_LOGW(NM_TAG, "port %u: не удалось прочитать UART cfg: %s", (unsigned)port_id, esp_err_to_name(e));
        return;
    }
    char pchar = 'N';
    if (uc.parity == UART_MGR_PARITY_ODD) {
        pchar = 'O';
    } else if (uc.parity == UART_MGR_PARITY_EVEN) {
        pchar = 'E';
    }
    ESP_LOGI(NM_TAG,
             "port %u: UART сейчас: %lu baud, %u%c%u, line %s, rings rx=%lu tx=%lu B, overflow_pol=%d",
             (unsigned)port_id, (unsigned long)uc.baud, (unsigned)uc.data_bits, pchar, (unsigned)uc.stop_bits,
             nm_uart_line_mode_str(uc.line_mode), (unsigned long)uc.ring_rx_size, (unsigned long)uc.ring_tx_size,
             (int)uc.overflow);
}

static int nm_tcp_server_accept_slot(nm_port_runtime_t *rt)
{
    for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
        if (rt->client_fds[i] < 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t nm_open_tcp_server(uart_port_id_t port_id)
{
    nm_port_runtime_t *rt = &s_ports[port_id];
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (ls < 0) {
        ESP_LOGE(NM_TAG, "port %u: socket listen errno %d", (unsigned)port_id, errno);
        return ESP_FAIL;
    }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(rt->cfg.local_tcp_udp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(NM_TAG, "port %u: bind %u errno %d", (unsigned)port_id, rt->cfg.local_tcp_udp_port, errno);
        close(ls);
        return ESP_FAIL;
    }
    if (listen(ls, NM_MAX_TCP_CLIENTS) != 0) {
        ESP_LOGE(NM_TAG, "port %u: listen errno %d", (unsigned)port_id, errno);
        close(ls);
        return ESP_FAIL;
    }
    rt->listen_fd = ls;
    ESP_LOGI(NM_TAG, "port %u: TCP server listen :%u", (unsigned)port_id, rt->cfg.local_tcp_udp_port);
    nm_log_uart_settings_for_port(port_id);
    return ESP_OK;
}

static esp_err_t nm_open_tcp_client(uart_port_id_t port_id)
{
    nm_port_runtime_t *rt = &s_ports[port_id];
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (inet_aton(rt->cfg.remote_ip, &a.sin_addr) == 0) {
        ESP_LOGE(NM_TAG, "port %u: bad remote_ip %s", (unsigned)port_id, rt->cfg.remote_ip);
        close(s);
        return ESP_ERR_INVALID_ARG;
    }
    a.sin_port = htons(rt->cfg.remote_port);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        NM_LOGD("port %u: connect %s:%u errno %d", (unsigned)port_id, rt->cfg.remote_ip, rt->cfg.remote_port, errno);
        close(s);
        return ESP_FAIL;
    }
    nm_tcp_apply_sockopts(s, &rt->cfg);
    if (nm_sock_set_nonblocking(s) != 0) {
        ESP_LOGW(NM_TAG, "port %u TCP client fd %d O_NONBLOCK fail errno %d", (unsigned)port_id, s, errno);
    }
    s_tcp_cli_tx[port_id].len = 0;
    rt->tcp_client_fd = s;
    ESP_LOGI(NM_TAG, "port %u: TCP client connected -> %s:%u", (unsigned)port_id, rt->cfg.remote_ip,
             rt->cfg.remote_port);
    return ESP_OK;
}

static esp_err_t nm_open_udp(uart_port_id_t port_id)
{
    nm_port_runtime_t *rt = &s_ports[port_id];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(rt->cfg.local_tcp_udp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(NM_TAG, "port %u: UDP bind errno %d", (unsigned)port_id, errno);
        close(s);
        return ESP_FAIL;
    }
    rt->udp_fd = s;
    ESP_LOGI(NM_TAG, "port %u: UDP bound :%u -> %s:%u", (unsigned)port_id, rt->cfg.local_tcp_udp_port,
             rt->cfg.remote_ip, rt->cfg.remote_port);
    return ESP_OK;
}

static esp_err_t nm_open_port_sockets_unlocked(uart_port_id_t port_id)
{
    nm_close_port_sockets_unlocked(port_id);
    nm_port_runtime_t *rt = &s_ports[port_id];
    switch (rt->cfg.mode) {
    case NM_MODE_OFF:
        return ESP_OK;
    case NM_MODE_TCP_SERVER:
        return nm_open_tcp_server(port_id);
    case NM_MODE_TCP_CLIENT:
        return nm_open_tcp_client(port_id);
    case NM_MODE_UDP:
        return nm_open_udp(port_id);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void nm_stored_to_cfg(const nm_stored_net_t *st, nm_port_net_config_t *out)
{
    out->mode = (nm_socket_mode_t)st->mode;
    out->local_tcp_udp_port = st->local_port;
    out->remote_port = st->remote_port;
    memcpy(out->remote_ip, st->remote_ip, sizeof(out->remote_ip));
    out->remote_ip[sizeof(out->remote_ip) - 1] = '\0';
    out->tcp_keepalive_idle_sec = st->keepalive_sec;
    out->tcp_nodelay = st->nodelay != 0;
}

static void nm_cfg_to_stored(const nm_port_net_config_t *cfg, nm_stored_net_t *st)
{
    memset(st, 0, sizeof(*st));
    st->mode = (uint8_t)cfg->mode;
    st->local_port = cfg->local_tcp_udp_port;
    st->remote_port = cfg->remote_port;
    strncpy(st->remote_ip, cfg->remote_ip, sizeof(st->remote_ip) - 1);
    st->keepalive_sec = cfg->tcp_keepalive_idle_sec;
    st->nodelay = cfg->tcp_nodelay ? 1u : 0u;
}

static void nm_defaults_all(void)
{
    for (int i = 0; i < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; i++) {
        s_ports[i].cfg.mode = NM_MODE_OFF;
        s_ports[i].cfg.local_tcp_udp_port = 0;
        s_ports[i].cfg.remote_port = 0;
        s_ports[i].cfg.remote_ip[0] = '\0';
        s_ports[i].cfg.tcp_keepalive_idle_sec = 60;
        s_ports[i].cfg.tcp_nodelay = true;
    }
}

static esp_err_t nm_load_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NM_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nm_defaults_all();
        ESP_LOGW(NM_TAG, "NVS net_mgr: нет namespace, дефолты");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nm_defaults_all();
        return err;
    }
    nm_nvs_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, NM_NVS_KEY_CFG, &blob, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(blob)) {
        nm_defaults_all();
        ESP_LOGW(NM_TAG, "NVS net_mgr: нет/битый блоб, дефолты");
        return ESP_OK;
    }
    if (blob.magic != NM_NVS_MAGIC || blob.struct_version != NM_NVS_STRUCT_VERSION) {
        nm_defaults_all();
        ESP_LOGW(NM_TAG, "NVS net_mgr: неверная версия, дефолты");
        return ESP_OK;
    }
    if (blob.physical_ports != CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS) {
        ESP_LOGW(NM_TAG, "NVS net_mgr: physical_ports=%u != config=%u, подгонка", (unsigned)blob.physical_ports,
                  (unsigned)CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS);
    }
    int n = (int)blob.physical_ports;
    if (n > CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS) {
        n = CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS;
    }
    for (int i = 0; i < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; i++) {
        if (i < n) {
            nm_stored_to_cfg(&blob.ports[i], &s_ports[i].cfg);
        } else {
            s_ports[i].cfg.mode = NM_MODE_OFF;
        }
    }
    return ESP_OK;
}

static esp_err_t nm_save_nvs(void)
{
    nm_nvs_blob_t blob = {
        .magic = NM_NVS_MAGIC,
        .struct_version = NM_NVS_STRUCT_VERSION,
        .physical_ports = (uint8_t)CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS,
        .reserved = 0,
    };
    for (int i = 0; i < 16; i++) {
        if (i < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS) {
            nm_cfg_to_stored(&s_ports[i].cfg, &blob.ports[i]);
        } else {
            memset(&blob.ports[i], 0, sizeof(blob.ports[i]));
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NM_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NM_NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void nm_reapply_all_from_ram(void)
{
    for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
        nm_close_port_sockets_unlocked(p);
        if (s_ports[p].cfg.mode != NM_MODE_OFF) {
            if (nm_open_port_sockets_unlocked(p) != ESP_OK) {
                NM_LOGD("reapply port %u open fail", (unsigned)p);
            }
        }
        nm_stats_reset_port(p);
    }
}

static esp_err_t nm_apply_uart_if_requested(uart_port_id_t port_id, bool apply_uart, const uart_manager_port_config_t *ucfg)
{
    if (!apply_uart) {
        return ESP_OK;
    }
    if (port_id != UART_MGR_F1_PORT_ID) {
        ESP_LOGW(NM_TAG, "apply_uart: порт %u не F1 — пропуск UART", (unsigned)port_id);
        return ESP_OK;
    }
    esp_err_t e = uart_manager_configure_port(port_id, ucfg);
    if (e != ESP_OK) {
        ESP_LOGE(NM_TAG, "uart_manager_configure_port fail: %s", esp_err_to_name(e));
        return e;
    }
    e = uart_manager_save_config_to_nvs();
    if (e != ESP_OK) {
        ESP_LOGE(NM_TAG, "uart_manager_save_config_to_nvs fail: %s", esp_err_to_name(e));
    }
    return e;
}

static void nm_handle_apply_config(const nm_command_t *cmd)
{
    if (!nm_port_id_valid(cmd->port_id)) {
        ESP_LOGE(NM_TAG, "APPLY: неверный port_id %u", (unsigned)cmd->port_id);
        return;
    }
    esp_err_t ue = nm_apply_uart_if_requested(cmd->port_id, cmd->apply_uart_line, &cmd->uart_cfg);
    if (ue != ESP_OK) {
        ESP_LOGW(NM_TAG, "APPLY: UART не применён, сокеты не трогаем");
        return;
    }
    nm_close_port_sockets_unlocked(cmd->port_id);
    s_ports[cmd->port_id].cfg = cmd->net_cfg;
    s_tcp_cli_last_try[cmd->port_id] = 0;
    esp_err_t oe = nm_open_port_sockets_unlocked(cmd->port_id);
    if (oe != ESP_OK) {
        ESP_LOGW(NM_TAG, "APPLY: открытие сокетов port %u err %s", (unsigned)cmd->port_id, esp_err_to_name(oe));
    }
    nm_stats_reset_port(cmd->port_id);
    if (nm_save_nvs() != ESP_OK) {
        ESP_LOGW(NM_TAG, "nm_save_nvs после APPLY не удалось");
    }
}

static void nm_forward_uart_to_net(uart_port_id_t port_id)
{
    nm_port_runtime_t *rt = &s_ports[port_id];
    uint8_t buf[NM_TCP_TX_BACKLOG];

    /* Пока UART молчит, всё равно досылаем backlog в TCP (иначе окно на ПК открылось — а байты висят в s_tcp_*_tx). */
    if (rt->cfg.mode == NM_MODE_TCP_SERVER) {
        for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
            int fd = rt->client_fds[i];
            if (fd >= 0) {
                uint32_t w =
                    nm_tcp_queue_and_send(port_id, fd, &s_tcp_srv_tx[port_id][i], s_nm_tcp_zero_len_placeholder, 0);
                if (w > 0 && xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_stats[port_id].socket_tx_bytes += w;
                    xSemaphoreGive(s_stats_mtx);
                }
            }
        }
    } else if (rt->cfg.mode == NM_MODE_TCP_CLIENT && rt->tcp_client_fd >= 0) {
        uint32_t w = nm_tcp_queue_and_send(port_id, rt->tcp_client_fd, &s_tcp_cli_tx[port_id],
                                           s_nm_tcp_zero_len_placeholder, 0);
        if (w > 0 && xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats[port_id].socket_tx_bytes += w;
            xSemaphoreGive(s_stats_mtx);
        }
    }

    size_t n = uart_manager_receive(port_id, buf, sizeof(buf), 0);
    if (n == 0) {
        return;
    }
    if (xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_stats[port_id].uart_rx_to_socket_bytes += (uint32_t)n;
        xSemaphoreGive(s_stats_mtx);
    }
    switch (rt->cfg.mode) {
    case NM_MODE_TCP_SERVER:
        for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
            int fd = rt->client_fds[i];
            if (fd >= 0) {
                uint32_t w = nm_tcp_queue_and_send(port_id, fd, &s_tcp_srv_tx[port_id][i], buf, n);
                if (w > 0 && xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_stats[port_id].socket_tx_bytes += w;
                    xSemaphoreGive(s_stats_mtx);
                }
            }
        }
        break;
    case NM_MODE_TCP_CLIENT:
        if (rt->tcp_client_fd >= 0) {
            uint32_t w =
                nm_tcp_queue_and_send(port_id, rt->tcp_client_fd, &s_tcp_cli_tx[port_id], buf, n);
            if (w > 0 && xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_stats[port_id].socket_tx_bytes += w;
                xSemaphoreGive(s_stats_mtx);
            }
        }
        break;
    case NM_MODE_UDP:
        if (rt->udp_fd >= 0 && rt->cfg.remote_ip[0] != '\0') {
            struct sockaddr_in a = { .sin_family = AF_INET };
            if (inet_aton(rt->cfg.remote_ip, &a.sin_addr) != 0) {
                a.sin_port = htons(rt->cfg.remote_port);
                ssize_t w = sendto(rt->udp_fd, buf, n, 0, (struct sockaddr *)&a, sizeof(a));
                if (w > 0 && xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_stats[port_id].socket_tx_bytes += (uint32_t)w;
                    xSemaphoreGive(s_stats_mtx);
                }
            }
        }
        break;
    default:
        break;
    }
}

static void nm_socket_to_uart(uart_port_id_t port_id, int fd)
{
    uint8_t buf[512];
    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    if (r > 0) {
        nm_stream_net_to_uart(port_id, buf, (size_t)r);
        if (xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats[port_id].socket_rx_bytes += (uint32_t)r;
            xSemaphoreGive(s_stats_mtx);
        }
    } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        NM_LOGD("port %u fd %d closing (r=%zd errno=%d)", (unsigned)port_id, fd, r, errno);
        close(fd);
        nm_port_runtime_t *rt = &s_ports[port_id];
        if (rt->tcp_client_fd == fd) {
            s_tcp_cli_tx[port_id].len = 0;
            rt->tcp_client_fd = -1;
        } else {
            for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
                if (rt->client_fds[i] == fd) {
                    s_tcp_srv_tx[port_id][i].len = 0;
                    rt->client_fds[i] = -1;
                    break;
                }
            }
        }
    }
}

static void nm_udp_to_uart(uart_port_id_t port_id)
{
    /* UDP дейтаграмма должна читаться целиком; иначе хвост пакета теряется. */
    uint8_t buf[1536];
    struct sockaddr_in from = { 0 };
    socklen_t flen = sizeof(from);
    ssize_t r = recvfrom(s_ports[port_id].udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
    if (r > 0) {
        nm_stream_net_to_uart(port_id, buf, (size_t)r);
        if (xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stats[port_id].socket_rx_bytes += (uint32_t)r;
            xSemaphoreGive(s_stats_mtx);
        }
    }
}

static void nm_try_tcp_client_reconnect(TickType_t now)
{
    for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
        nm_port_runtime_t *rt = &s_ports[p];
        if (rt->cfg.mode != NM_MODE_TCP_CLIENT) {
            continue;
        }
        if (rt->tcp_client_fd >= 0) {
            continue;
        }
        TickType_t period = pdMS_TO_TICKS(CONFIG_NETWORK_MGR_TCP_RECONNECT_MS);
        if (s_tcp_cli_last_try[p] != 0 && (now - s_tcp_cli_last_try[p]) < period) {
            continue;
        }
        s_tcp_cli_last_try[p] = now;
        if (nm_open_tcp_client(p) == ESP_OK) {
            nm_stats_reset_port(p);
        }
    }
}

static void nm_build_fd_set(fd_set *rfds, int *pmax)
{
    FD_ZERO(rfds);
    *pmax = -1;
    for (uart_port_id_t port_id = 0; port_id < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; port_id++) {
        nm_port_runtime_t *rt = &s_ports[port_id];
        if (rt->cfg.mode == NM_MODE_TCP_SERVER) {
            if (rt->listen_fd >= 0) {
                FD_SET(rt->listen_fd, rfds);
                *pmax = MAX(*pmax, rt->listen_fd);
            }
            for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
                if (rt->client_fds[i] >= 0) {
                    FD_SET(rt->client_fds[i], rfds);
                    *pmax = MAX(*pmax, rt->client_fds[i]);
                }
            }
        } else if (rt->cfg.mode == NM_MODE_TCP_CLIENT && rt->tcp_client_fd >= 0) {
            FD_SET(rt->tcp_client_fd, rfds);
            *pmax = MAX(*pmax, rt->tcp_client_fd);
        } else if (rt->cfg.mode == NM_MODE_UDP && rt->udp_fd >= 0) {
            FD_SET(rt->udp_fd, rfds);
            *pmax = MAX(*pmax, rt->udp_fd);
        }
    }
}

static void nm_process_select(fd_set *rfds)
{
    for (uart_port_id_t port_id = 0; port_id < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; port_id++) {
        nm_port_runtime_t *rt = &s_ports[port_id];
        if (rt->cfg.mode == NM_MODE_TCP_SERVER) {
            if (rt->listen_fd >= 0 && FD_ISSET(rt->listen_fd, rfds)) {
                struct sockaddr_in c = { 0 };
                socklen_t cl = sizeof(c);
                int ns = accept(rt->listen_fd, (struct sockaddr *)&c, &cl);
                if (ns >= 0) {
                    int slot = nm_tcp_server_accept_slot(rt);
                    if (slot < 0) {
                        ESP_LOGW(NM_TAG, "port %u: max TCP clients, reject", (unsigned)port_id);
                        close(ns);
                    } else {
                        nm_tcp_apply_sockopts(ns, &rt->cfg);
                        if (nm_sock_set_nonblocking(ns) != 0) {
                            ESP_LOGW(NM_TAG, "port %u accept fd %d O_NONBLOCK fail errno %d", (unsigned)port_id, ns,
                                      errno);
                        }
                        s_tcp_srv_tx[port_id][slot].len = 0;
                        rt->client_fds[slot] = ns;
                        NM_LOGI_V("port %u: accept slot %d fd %d", (unsigned)port_id, slot, ns);
                    }
                }
            }
            for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
                int fd = rt->client_fds[i];
                if (fd >= 0 && FD_ISSET(fd, rfds)) {
                    nm_socket_to_uart(port_id, fd);
                }
            }
        } else if (rt->cfg.mode == NM_MODE_TCP_CLIENT && rt->tcp_client_fd >= 0 &&
                   FD_ISSET(rt->tcp_client_fd, rfds)) {
            nm_socket_to_uart(port_id, rt->tcp_client_fd);
        } else if (rt->cfg.mode == NM_MODE_UDP && rt->udp_fd >= 0 && FD_ISSET(rt->udp_fd, rfds)) {
            nm_udp_to_uart(port_id);
        }
    }
}

static void nm_update_tcp_client_counts(void)
{
    if (xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
        nm_port_runtime_t *rt = &s_ports[p];
        uint8_t c = 0;
        if (rt->cfg.mode == NM_MODE_TCP_SERVER) {
            for (int i = 0; i < NM_MAX_TCP_CLIENTS; i++) {
                if (rt->client_fds[i] >= 0) {
                    c++;
                }
            }
        } else if (rt->cfg.mode == NM_MODE_TCP_CLIENT && rt->tcp_client_fd >= 0) {
            c = 1;
        }
        s_stats[p].tcp_client_count = c;
    }
    xSemaphoreGive(s_stats_mtx);
}

static void nm_apply_command(const nm_command_t *cmd)
{
    switch (cmd->type) {
    case NM_CMD_APPLY_CONFIG:
        nm_handle_apply_config(cmd);
        break;
    case NM_CMD_STOP_PORT:
        if (nm_port_id_valid(cmd->port_id)) {
            nm_close_port_sockets_unlocked(cmd->port_id);
            s_ports[cmd->port_id].cfg.mode = NM_MODE_OFF;
            nm_save_nvs();
        }
        break;
    case NM_CMD_SHUTDOWN:
        s_shut_down = true;
        for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
            nm_close_port_sockets_unlocked(p);
            s_ports[p].cfg.mode = NM_MODE_OFF;
        }
        nm_save_nvs();
        break;
    case NM_CMD_LOAD_AND_APPLY_ALL:
        nm_load_nvs();
        nm_reapply_all_from_ram();
        break;
    case NM_CMD_REAPPLY_RUNNING:
        nm_reapply_all_from_ram();
        break;
    case NM_CMD_LINK_DOWN:
        for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
            nm_close_port_sockets_unlocked(p);
        }
        break;
    default:
        break;
    }
}

static void nm_task(void *arg)
{
    (void)arg;
    nm_command_t cmd;
    const TickType_t qt = pdMS_TO_TICKS(CONFIG_NETWORK_MGR_SELECT_TIMEOUT_MS);
    while (s_nm_task_run) {
        if (s_shut_down) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        nm_command_t cmd_now;
        while (xQueueReceive(s_cmd_q, &cmd_now, 0) == pdTRUE) {
            nm_apply_command(&cmd_now);
        }
        for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
            if (s_ports[p].cfg.mode != NM_MODE_OFF) {
                nm_flush_tcp_to_uart_pending(p);
            }
        }
        TickType_t now = xTaskGetTickCount();
        nm_try_tcp_client_reconnect(now);

        fd_set rfds;
        int maxfd = -1;
        nm_build_fd_set(&rfds, &maxfd);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
        if (maxfd >= 0) {
            int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
            if (rc > 0) {
                nm_process_select(&rfds);
            } else if (rc < 0 && errno != EINTR) {
                ESP_LOGW(NM_TAG, "select failed errno=%d", errno);
            }
        }

        for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
            if (s_ports[p].cfg.mode != NM_MODE_OFF) {
                nm_forward_uart_to_net(p);
            }
        }
        nm_update_tcp_client_counts();

        if (xQueueReceive(s_cmd_q, &cmd, qt) == pdTRUE) {
            nm_apply_command(&cmd);
        }
    }
    if (s_nm_task_exit_sem) {
        xSemaphoreGive(s_nm_task_exit_sem);
    }
    vTaskDelete(NULL);
}


static void nm_ip_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;
    if (event_id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(NM_TAG, "ETH_LOST_IP: закрытие сокетов (очередь)");
        nm_command_t c = { .type = NM_CMD_LINK_DOWN };
        (void)network_manager_post_command(&c, 0);
    } else if (event_id == IP_EVENT_ETH_GOT_IP) {
        ESP_LOGI(NM_TAG, "ETH_GOT_IP: переоткрытие сокетов");
        nm_command_t c = { .type = NM_CMD_REAPPLY_RUNNING };
        (void)network_manager_post_command(&c, 0);
    }
}

esp_err_t network_manager_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    nm_init_runtime();
    s_cmd_q = xQueueCreate(CONFIG_NETWORK_MGR_CMD_QUEUE_LEN, sizeof(nm_command_t));
    if (s_cmd_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stats_mtx = xSemaphoreCreateMutex();
    if (s_stats_mtx == NULL) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_nm_task_exit_sem = xSemaphoreCreateBinary();
    if (s_nm_task_exit_sem == NULL) {
        vSemaphoreDelete(s_stats_mtx);
        vQueueDelete(s_cmd_q);
        s_stats_mtx = NULL;
        s_cmd_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, nm_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, nm_ip_event, NULL));
    s_nm_task_run = true;
    BaseType_t ok = xTaskCreatePinnedToCore(nm_task, "network_mgr", CONFIG_NETWORK_MGR_TASK_STACK, NULL,
                                            CONFIG_NETWORK_MGR_TASK_PRIORITY, &s_nm_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, nm_ip_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, nm_ip_event);
        vSemaphoreDelete(s_nm_task_exit_sem);
        vSemaphoreDelete(s_stats_mtx);
        vQueueDelete(s_cmd_q);
        s_nm_task_exit_sem = NULL;
        s_stats_mtx = NULL;
        s_cmd_q = NULL;
        s_nm_task_run = false;
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(NM_TAG, "init OK, physical COM ports = %u", (unsigned)network_manager_get_physical_com_count());
    return ESP_OK;
}

void network_manager_deinit(void)
{
    if (!s_inited) {
        return;
    }

    s_inited = false;
    s_shut_down = false;
    s_nm_task_run = false;
    nm_command_t wake = { .type = NM_CMD_REAPPLY_RUNNING };
    (void)xQueueSend(s_cmd_q, &wake, 0);

    if (s_nm_task_exit_sem) {
        (void)xSemaphoreTake(s_nm_task_exit_sem, pdMS_TO_TICKS(2000));
        vSemaphoreDelete(s_nm_task_exit_sem);
        s_nm_task_exit_sem = NULL;
    }
    s_nm_task = NULL;

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, nm_ip_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, nm_ip_event);

    for (uart_port_id_t p = 0; p < CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS; p++) {
        nm_close_port_sockets_unlocked(p);
    }

    if (s_stats_mtx) {
        vSemaphoreDelete(s_stats_mtx);
        s_stats_mtx = NULL;
    }
    if (s_cmd_q) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }
    nm_init_runtime();
}

esp_err_t network_manager_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    nm_command_t c = { .type = NM_CMD_LOAD_AND_APPLY_ALL };
    if (!network_manager_post_command(&c, pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool network_manager_post_command(const nm_command_t *cmd, TickType_t ticks_to_wait)
{
    if (!s_inited || s_cmd_q == NULL || cmd == NULL) {
        return false;
    }
    return xQueueSend(s_cmd_q, cmd, ticks_to_wait) == pdTRUE;
}

esp_err_t network_manager_get_stats(uart_port_id_t port_id, nm_port_stats_t *out_stats)
{
    if (out_stats == NULL || !nm_port_id_valid(port_id) || !s_inited || s_stats_mtx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_stats_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_stats = s_stats[port_id];
    xSemaphoreGive
    (s_stats_mtx);
    return ESP_OK;
}

esp_err_t network_manager_get_port_config(uart_port_id_t port_id, nm_port_net_config_t *out_cfg)
{
    if (out_cfg == NULL || !nm_port_id_valid(port_id) || !s_inited) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out_cfg, &s_ports[port_id].cfg, sizeof(nm_port_net_config_t));
    return ESP_OK;
}

#if defined(CONFIG_NETWORK_MGR_ENABLE_TEST_TASK) && CONFIG_NETWORK_MGR_ENABLE_TEST_TASK
#include "network_manager_test_scenarios.h"

static void nm_test_copy_remote_ip(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0) {
        return;
    }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

void network_mgr_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(NM_TEST_START_DELAY_MS));

#if defined(NM_TEST_SCENARIO_TCP_SERVER) || defined(NM_TEST_SCENARIO_TCP_SERVER_5CLIENTS_MANUAL) || defined(NM_TEST_SCENARIO_TCP_THEN_STOP) || defined(NM_TEST_SCENARIO_TCP_THEN_UDP)
    nm_command_t cmd = {
        .type = NM_CMD_APPLY_CONFIG,
        .port_id = UART_MGR_F1_PORT_ID,
        .apply_uart_line = false,
        .net_cfg =
            {
                .mode = NM_MODE_TCP_SERVER,
                .local_tcp_udp_port = NM_TEST_LOCAL_PORT,
                .remote_port = 0,
                .remote_ip = "",
                .tcp_keepalive_idle_sec = NM_TEST_TCP_KEEPALIVE_SEC,
                .tcp_nodelay = true,
            },
    };
#if defined(NM_TEST_SCENARIO_TCP_SERVER)
    ESP_LOGW(NM_TAG, "test: S1 TCP server :%u → nc/ПК на IP устройства", (unsigned)NM_TEST_LOCAL_PORT);
#endif
#if defined(NM_TEST_SCENARIO_TCP_SERVER_5CLIENTS_MANUAL)
    ESP_LOGW(NM_TAG,
             "test: S2 TCP server :%u — откройте до 5 сессий с ПК; UART без конкурирующего uart_echo",
             (unsigned)NM_TEST_LOCAL_PORT);
#endif
#if defined(NM_TEST_SCENARIO_TCP_THEN_STOP)
    ESP_LOGW(NM_TAG, "test: S5 TCP server :%u затем STOP через %ums", (unsigned)NM_TEST_LOCAL_PORT,
             (unsigned)NM_TEST_THEN_STOP_DELAY_MS);
#endif
#if defined(NM_TEST_SCENARIO_TCP_THEN_UDP)
    ESP_LOGW(NM_TAG, "test: S5->S4 TCP server :%u затем STOP через %ums и APPLY UDP через %ums",
             (unsigned)NM_TEST_LOCAL_PORT, (unsigned)NM_TEST_THEN_STOP_DELAY_MS, (unsigned)NM_TEST_S5_TO_S4_DELAY_MS);
#endif
    if (!network_manager_post_command(&cmd, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(NM_TAG, "test: очередь команд полна (APPLY)");
        vTaskDelete(NULL);
        return;
    }
#if defined(NM_TEST_SCENARIO_TCP_THEN_STOP) || defined(NM_TEST_SCENARIO_TCP_THEN_UDP)
    vTaskDelay(pdMS_TO_TICKS(NM_TEST_THEN_STOP_DELAY_MS));
    nm_command_t stop = { .type = NM_CMD_STOP_PORT, .port_id = UART_MGR_F1_PORT_ID };
    ESP_LOGW(NM_TAG, "test: S5 NM_CMD_STOP_PORT");
    if (!network_manager_post_command(&stop, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(NM_TAG, "test: очередь полна (STOP)");
    }
#endif
#if defined(NM_TEST_SCENARIO_TCP_THEN_UDP)
    vTaskDelay(pdMS_TO_TICKS(NM_TEST_S5_TO_S4_DELAY_MS));
    nm_command_t udp_cmd = {
        .type = NM_CMD_APPLY_CONFIG,
        .port_id = UART_MGR_F1_PORT_ID,
        .apply_uart_line = false,
        .net_cfg =
            {
                .mode = NM_MODE_UDP,
                .local_tcp_udp_port = NM_TEST_LOCAL_PORT,
                .remote_port = NM_TEST_REMOTE_PORT,
                .tcp_keepalive_idle_sec = 0,
                .tcp_nodelay = true,
            },
    };
    nm_test_copy_remote_ip(udp_cmd.net_cfg.remote_ip, sizeof(udp_cmd.net_cfg.remote_ip), NM_TEST_REMOTE_IP);
    ESP_LOGW(NM_TAG, "test: S4 APPLY UDP bind :%u ↔ %s:%u", (unsigned)NM_TEST_LOCAL_PORT, NM_TEST_REMOTE_IP,
             (unsigned)NM_TEST_REMOTE_PORT);
    if (!network_manager_post_command(&udp_cmd, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(NM_TAG, "test: очередь полна (APPLY UDP)");
    }
#endif
#endif

#if defined(NM_TEST_SCENARIO_TCP_CLIENT)
    nm_command_t cmd = {
        .type = NM_CMD_APPLY_CONFIG,
        .port_id = UART_MGR_F1_PORT_ID,
        .apply_uart_line = false,
        .net_cfg =
            {
                .mode = NM_MODE_TCP_CLIENT,
                .local_tcp_udp_port = 0,
                .remote_port = NM_TEST_REMOTE_PORT,
                .tcp_keepalive_idle_sec = NM_TEST_TCP_KEEPALIVE_SEC,
                .tcp_nodelay = true,
            },
    };
    nm_test_copy_remote_ip(cmd.net_cfg.remote_ip, sizeof(cmd.net_cfg.remote_ip), NM_TEST_REMOTE_IP);
    ESP_LOGW(NM_TAG, "test: S3 TCP client → %s:%u (на ПК: nc -l -p %u)", NM_TEST_REMOTE_IP,
             (unsigned)NM_TEST_REMOTE_PORT, (unsigned)NM_TEST_REMOTE_PORT);
    if (!network_manager_post_command(&cmd, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(NM_TAG, "test: очередь команд полна");
    }
#endif

#if defined(NM_TEST_SCENARIO_UDP)
    nm_command_t cmd = {
        .type = NM_CMD_APPLY_CONFIG,
        .port_id = UART_MGR_F1_PORT_ID,
        .apply_uart_line = false,
        .net_cfg =
            {
                .mode = NM_MODE_UDP,
                .local_tcp_udp_port = NM_TEST_LOCAL_PORT,
                .remote_port = NM_TEST_REMOTE_PORT,
                .tcp_keepalive_idle_sec = 0,
                .tcp_nodelay = true,
            },
    };
    nm_test_copy_remote_ip(cmd.net_cfg.remote_ip, sizeof(cmd.net_cfg.remote_ip), NM_TEST_REMOTE_IP);
    ESP_LOGW(NM_TAG, "test: S4 UDP bind :%u ↔ %s:%u", (unsigned)NM_TEST_LOCAL_PORT, NM_TEST_REMOTE_IP,
             (unsigned)NM_TEST_REMOTE_PORT);
    if (!network_manager_post_command(&cmd, pdMS_TO_TICKS(2000))) {
        ESP_LOGE(NM_TAG, "test: очередь команд полна");
    }
#endif

    vTaskDelete(NULL);
}
#endif
