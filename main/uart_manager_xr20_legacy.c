/*
 * НЕ УЧАСТВУЕТ В СБОРКЕ (не в CMakeLists) — сохранённая реализация XR20/SPI.
 * Активный код: uart_manager.c (заглушки; без XR20).
 */
/*
 * uart_manager: SPI-доступ к XR20M1280, GPIO мультиплексор RS-232/485,
 * кольцевые буферы, задача FreeRTOS, NVS и публичный API для внешних задач.
 * См. UART_MANAGER_API.md — контексты вызова и синхронизация.
 */

#include "uart_manager.h"

#include "board_pins.h"
#include "xr20m1280.h"
#include "xr20m1280_regs.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "uart_mgr";

/* Лимит байт за один poll: иначе при непрерывном DR цикл не отдаёт CPU и срабатывает TWDT на IDLE. */
#define UART_MGR_MAX_RX_PER_POLL 64

#define UART_NVS_NAMESPACE "uart_mgr"
#define UART_NVS_KEY "ports_v1"
#define UART_NVS_MAGIC 0x554d4731u

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t head;
    size_t tail;
} uart_ring_t;

typedef struct {
    uart_ring_t rx;
    uart_ring_t tx;
    uart_manager_port_config_t cfg;
    uart_manager_stats_t stats;
} uart_port_ctx_t;

typedef struct {
    uint32_t magic;
    uart_manager_port_config_t ports[UART_MGR_NUM_PORTS];
} uart_nvs_blob_t;

static spi_device_handle_t s_spi_dev[2];
static bool s_inited;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_spi_lock;
static SemaphoreHandle_t s_ring_mtx[UART_MGR_NUM_PORTS];
static SemaphoreHandle_t s_rx_sem[UART_MGR_NUM_PORTS];
static SemaphoreHandle_t s_tx_space[UART_MGR_NUM_PORTS];
static EventGroupHandle_t s_rx_ready_bits;
static uart_port_ctx_t s_ports[UART_MGR_NUM_PORTS];

static esp_err_t ring_init(uart_ring_t *r, size_t cap)
{
    r->data = (uint8_t *)malloc(cap);
    if (!r->data) {
        return ESP_ERR_NO_MEM;
    }
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
    return ESP_OK;
}

static void ring_free(uart_ring_t *r)
{
    free(r->data);
    r->data = NULL;
    r->cap = 0;
    r->head = r->tail = 0;
}

static bool ring_put(uart_ring_t *r, uint8_t b, uart_mgr_overflow_policy_t pol, uint32_t *overflow_cnt)
{
    size_t next = (r->head + 1) % r->cap;
    if (next == r->tail) {
        if (pol == UART_MGR_OVERFLOW_DISCARD_OLD) {
            r->tail = (r->tail + 1) % r->cap;
            if (overflow_cnt) {
                (*overflow_cnt)++;
            }
        } else {
            if (overflow_cnt) {
                (*overflow_cnt)++;
            }
            return false;
        }
    }
    r->data[r->head] = b;
    r->head = next;
    return true;
}

static bool ring_get(uart_ring_t *r, uint8_t *b)
{
    if (r->tail == r->head) {
        return false;
    }
    *b = r->data[r->tail];
    r->tail = (r->tail + 1) % r->cap;
    return true;
}

static uart_manager_port_config_t default_port_config(void)
{
    uart_manager_port_config_t c = {
        .baud = UART_MGR_DEFAULT_BAUD,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = UART_MGR_PARITY_NONE,
        .line_mode = UART_MGR_LINE_RS232,
        .ring_rx_size = 1024,
        .ring_tx_size = 1024,
        .overflow = UART_MGR_OVERFLOW_DISCARD_OLD,
    };
    return c;
}

static void gpio_pin_out(int pin, int level)
{
    gpio_set_level(pin, level);
}

static esp_err_t uart_hw_apply_line_mode(uart_mgr_line_mode_t mode)
{
    switch (mode) {
        case UART_MGR_LINE_RS232:
            gpio_pin_out(BOARD_GPIO_IF_SEL_A, 0);
            gpio_pin_out(BOARD_GPIO_IF_SEL_B, 0);
            break;
        case UART_MGR_LINE_RS485:
            gpio_pin_out(BOARD_GPIO_IF_SEL_A, 0);
            gpio_pin_out(BOARD_GPIO_IF_SEL_B, 1);
            break;
        case UART_MGR_LINE_RS422:
            gpio_pin_out(BOARD_GPIO_IF_SEL_A, 1);
            gpio_pin_out(BOARD_GPIO_IF_SEL_B, 0);
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void uart_hw_select_group(uint8_t group_index)
{
    const int gp[4] = {
        BOARD_GPIO_CS_GROUP1,
        BOARD_GPIO_CS_GROUP2,
        BOARD_GPIO_CS_GROUP3,
        BOARD_GPIO_CS_GROUP4,
    };
    for (int i = 0; i < 4; i++) {
        gpio_pin_out(gp[i], (i == (int)group_index) ? 1 : 0);
    }
}

static spi_device_handle_t uart_hw_spi_dev_for_port(uart_port_id_t port)
{
    return (port < 8) ? s_spi_dev[0] : s_spi_dev[1];
}

static esp_err_t uart_hw_select_port(uart_port_id_t port)
{
    if (port >= UART_MGR_NUM_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_mgr_line_mode_t mode = s_ports[port].cfg.line_mode;
    ESP_RETURN_ON_ERROR(uart_hw_apply_line_mode(mode), TAG, "line mode");
    uint8_t group = (uint8_t)(port / 4);
    uart_hw_select_group(group);
    return ESP_OK;
}

static esp_err_t uart_ports_load_nvs_cfg(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(UART_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open uart");

    uart_nvs_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, UART_NVS_KEY, &blob, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_get_blob uart");
    if (sz != sizeof(blob) || blob.magic != UART_NVS_MAGIC) {
        ESP_LOGW(TAG, "uart NVS blob invalid, defaults");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        s_ports[i].cfg = blob.ports[i];
    }
    return ESP_OK;
}

static esp_err_t uart_apply_xr_config(uart_port_id_t port)
{
    spi_device_handle_t dev = uart_hw_spi_dev_for_port(port);
    ESP_RETURN_ON_ERROR(uart_hw_select_port(port), TAG, "select port");
    ESP_RETURN_ON_ERROR(xr20m1280_fifo_enable(dev), TAG, "fifo");
    ESP_RETURN_ON_ERROR(xr20m1280_set_line_config(dev, BOARD_XR20_UART_CLOCK_HZ, s_ports[port].cfg.baud,
                                                  s_ports[port].cfg.data_bits, s_ports[port].cfg.stop_bits,
                                                  (uint8_t)s_ports[port].cfg.parity),
                        TAG, "line cfg");
    return ESP_OK;
}

static esp_err_t uart_manager_poll_rx(uart_port_id_t port)
{
    spi_device_handle_t dev = uart_hw_spi_dev_for_port(port);
    ESP_RETURN_ON_ERROR(uart_hw_select_port(port), TAG, "select");

    uint8_t lsr = 0;
    for (unsigned n = 0; n < UART_MGR_MAX_RX_PER_POLL; n++) {
        ESP_RETURN_ON_ERROR(xr20m1280_read_lsr(dev, &lsr), TAG, "lsr");
        if ((lsr & XR20_LSR_DR) == 0) {
            break;
        }
        uint8_t b = 0;
        ESP_RETURN_ON_ERROR(xr20m1280_read_rhr(dev, &b), TAG, "rhr");
        if (lsr & (XR20_LSR_OE | XR20_LSR_PE | XR20_LSR_FE | XR20_LSR_BI)) {
            s_ports[port].stats.framing_errors++;
        }
        xSemaphoreTake(s_ring_mtx[port], portMAX_DELAY);
        bool ok = ring_put(&s_ports[port].rx, b, s_ports[port].cfg.overflow, &s_ports[port].stats.rx_overflows);
        xSemaphoreGive(s_ring_mtx[port]);
        if (ok) {
            s_ports[port].stats.rx_bytes++;
            xSemaphoreGive(s_rx_sem[port]);
            xEventGroupSetBits(s_rx_ready_bits, (EventBits_t)(1u << port));
        }
        if ((n & 7u) == 7u) {
            taskYIELD();
        }
    }
    return ESP_OK;
}

static esp_err_t uart_manager_poll_tx(uart_port_id_t port)
{
    spi_device_handle_t dev = uart_hw_spi_dev_for_port(port);
    ESP_RETURN_ON_ERROR(uart_hw_select_port(port), TAG, "select");

    uint8_t lsr = 0;
    ESP_RETURN_ON_ERROR(xr20m1280_read_lsr(dev, &lsr), TAG, "lsr tx");
    if ((lsr & XR20_LSR_THRE) == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ring_mtx[port], portMAX_DELAY);
    uint8_t b = 0;
    bool has = ring_get(&s_ports[port].tx, &b);
    xSemaphoreGive(s_ring_mtx[port]);
    if (!has) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(uart_hw_select_port(port), TAG, "select2");
    ESP_RETURN_ON_ERROR(xr20m1280_write_thr(dev, b), TAG, "thr");
    s_ports[port].stats.tx_bytes++;
    xSemaphoreGive(s_tx_space[port]);
    return ESP_OK;
}

static void uart_manager_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_spi_lock, portMAX_DELAY);
        for (int p = 0; p < UART_MGR_NUM_PORTS; p++) {
            if (uart_manager_poll_rx((uart_port_id_t)p) != ESP_OK) {
                ESP_LOGW(TAG, "poll_rx fail port %d", p);
            }
            if (uart_manager_poll_tx((uart_port_id_t)p) != ESP_OK) {
                ESP_LOGW(TAG, "poll_tx fail port %d", p);
            }
        }
        xSemaphoreGive(s_spi_lock);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t uart_manager_save_config_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(UART_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uart_nvs_blob_t blob = {.magic = UART_NVS_MAGIC};
    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        blob.ports[i] = s_ports[i].cfg;
    }
    err = nvs_set_blob(h, UART_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t uart_manager_load_config_from_nvs(void)
{
    esp_err_t err = uart_ports_load_nvs_cfg();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_inited) {
        return ESP_OK;
    }
    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        ESP_RETURN_ON_ERROR(uart_manager_configure_port((uart_port_id_t)i, &s_ports[i].cfg), TAG, "reconfigure");
    }
    return ESP_OK;
}

esp_err_t uart_manager_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        s_ports[i].cfg = default_port_config();
    }
    esp_err_t ncfg = uart_ports_load_nvs_cfg();
    if (ncfg != ESP_OK && ncfg != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "NVS cfg: %s", esp_err_to_name(ncfg));
    }

    s_spi_lock = xSemaphoreCreateMutex();
    if (!s_spi_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_rx_ready_bits = xEventGroupCreate();
    if (!s_rx_ready_bits) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        s_ring_mtx[i] = xSemaphoreCreateMutex();
        size_t rx_slots = (s_ports[i].cfg.ring_rx_size > 1) ? s_ports[i].cfg.ring_rx_size - 1 : 1;
        size_t tx_slots = (s_ports[i].cfg.ring_tx_size > 1) ? s_ports[i].cfg.ring_tx_size - 1 : 1;
        s_rx_sem[i] = xSemaphoreCreateCounting((UBaseType_t)rx_slots, 0);
        s_tx_space[i] = xSemaphoreCreateCounting((UBaseType_t)tx_slots, (UBaseType_t)tx_slots);
        if (!s_ring_mtx[i] || !s_rx_sem[i] || !s_tx_space[i]) {
            return ESP_ERR_NO_MEM;
        }
        ESP_RETURN_ON_ERROR(ring_init(&s_ports[i].rx, s_ports[i].cfg.ring_rx_size), TAG, "rx ring");
        ESP_RETURN_ON_ERROR(ring_init(&s_ports[i].tx, s_ports[i].cfg.ring_tx_size), TAG, "tx ring");
        memset(&s_ports[i].stats, 0, sizeof(s_ports[i].stats));
    }

    const int mux_pins[] = {
        BOARD_GPIO_IF_SEL_A,
        BOARD_GPIO_IF_SEL_B,
        BOARD_GPIO_CS_GROUP1,
        BOARD_GPIO_CS_GROUP2,
        BOARD_GPIO_CS_GROUP3,
        BOARD_GPIO_CS_GROUP4,
    };
    for (size_t i = 0; i < sizeof(mux_pins) / sizeof(mux_pins[0]); i++) {
        gpio_reset_pin(mux_pins[i]);
        gpio_set_direction(mux_pins[i], GPIO_MODE_OUTPUT);
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_SPI_MOSI_GPIO,
        .miso_io_num = BOARD_SPI_MISO_GPIO,
        .sclk_io_num = BOARD_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    spi_host_device_t host = (spi_host_device_t)BOARD_SPI_HOST;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi_bus_init");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (int)BOARD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
    };

    for (int c = 0; c < 2; c++) {
        devcfg.spics_io_num = (c == 0) ? BOARD_SPI_CS0_GPIO : BOARD_SPI_CS1_GPIO;
        ESP_RETURN_ON_ERROR(spi_bus_add_device(host, &devcfg, &s_spi_dev[c]), TAG, "spi_dev_add");
    }

    if (ncfg == ESP_ERR_INVALID_STATE) {
        for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
            s_ports[i].cfg = default_port_config();
        }
    }
    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        ESP_RETURN_ON_ERROR(uart_apply_xr_config((uart_port_id_t)i), TAG, "apply cfg");
    }

    BaseType_t ok = xTaskCreate(uart_manager_task, "uart_mgr", 8192, NULL, 5, &s_task);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

#if defined(CONFIG_UART_MGR_ENABLE_TEST_TASK) && CONFIG_UART_MGR_ENABLE_TEST_TASK
    xTaskCreate(uart_mgr_consumer_test_task, "uart_tst", 4096, NULL, 5, NULL);
#endif

    s_inited = true;
    ESP_LOGI(TAG, "uart_manager initialized");
    return ESP_OK;
}

void uart_manager_deinit(void)
{
    if (!s_inited) {
        return;
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    for (int c = 0; c < 2; c++) {
        if (s_spi_dev[c]) {
            spi_bus_remove_device(s_spi_dev[c]);
            s_spi_dev[c] = NULL;
        }
    }
    spi_host_device_t host = (spi_host_device_t)BOARD_SPI_HOST;
    spi_bus_free(host);

    for (int i = 0; i < UART_MGR_NUM_PORTS; i++) {
        ring_free(&s_ports[i].rx);
        ring_free(&s_ports[i].tx);
        if (s_ring_mtx[i]) {
            vSemaphoreDelete(s_ring_mtx[i]);
            s_ring_mtx[i] = NULL;
        }
        if (s_rx_sem[i]) {
            vSemaphoreDelete(s_rx_sem[i]);
            s_rx_sem[i] = NULL;
        }
        if (s_tx_space[i]) {
            vSemaphoreDelete(s_tx_space[i]);
            s_tx_space[i] = NULL;
        }
    }
    if (s_rx_ready_bits) {
        vEventGroupDelete(s_rx_ready_bits);
        s_rx_ready_bits = NULL;
    }
    if (s_spi_lock) {
        vSemaphoreDelete(s_spi_lock);
        s_spi_lock = NULL;
    }
    s_inited = false;
}

esp_err_t uart_manager_configure_port(uart_port_id_t port, const uart_manager_port_config_t *cfg)
{
    if (!cfg || port >= UART_MGR_NUM_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->ring_rx_size < 2 || cfg->ring_tx_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    s_ports[port].cfg = *cfg;

    if (s_ports[port].rx.cap != cfg->ring_rx_size) {
        ring_free(&s_ports[port].rx);
        esp_err_t e = ring_init(&s_ports[port].rx, cfg->ring_rx_size);
        if (e != ESP_OK) {
            xSemaphoreGive(s_spi_lock);
            return e;
        }
        vSemaphoreDelete(s_rx_sem[port]);
        size_t rx_slots = (cfg->ring_rx_size > 1) ? cfg->ring_rx_size - 1 : 1;
        s_rx_sem[port] = xSemaphoreCreateCounting((UBaseType_t)rx_slots, 0);
        if (!s_rx_sem[port]) {
            xSemaphoreGive(s_spi_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ports[port].tx.cap != cfg->ring_tx_size) {
        ring_free(&s_ports[port].tx);
        esp_err_t e = ring_init(&s_ports[port].tx, cfg->ring_tx_size);
        if (e != ESP_OK) {
            xSemaphoreGive(s_spi_lock);
            return e;
        }
        vSemaphoreDelete(s_tx_space[port]);
        size_t tx_slots = (cfg->ring_tx_size > 1) ? cfg->ring_tx_size - 1 : 1;
        s_tx_space[port] = xSemaphoreCreateCounting((UBaseType_t)tx_slots, (UBaseType_t)tx_slots);
        if (!s_tx_space[port]) {
            xSemaphoreGive(s_spi_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = uart_apply_xr_config(port);
    xSemaphoreGive(s_spi_lock);
    return err;
}

esp_err_t uart_manager_set_line_mode(uart_port_id_t port, uart_mgr_line_mode_t mode)
{
    if (port >= UART_MGR_NUM_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_spi_lock, portMAX_DELAY);
    s_ports[port].cfg.line_mode = mode;
    esp_err_t err = uart_apply_xr_config(port);
    xSemaphoreGive(s_spi_lock);
    return err;
}

size_t uart_manager_send(uart_port_id_t port, const void *data, size_t len, TickType_t ticks_to_wait)
{
    if (port >= UART_MGR_NUM_PORTS || !data) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    TickType_t deadline = xTaskGetTickCount() + ticks_to_wait;
    while (sent < len) {
        TickType_t left = deadline - xTaskGetTickCount();
        if (left <= 0) {
            break;
        }
        if (xSemaphoreTake(s_tx_space[port], left) != pdTRUE) {
            break;
        }
        xSemaphoreTake(s_ring_mtx[port], portMAX_DELAY);
        bool ok = ring_put(&s_ports[port].tx, p[sent], UART_MGR_OVERFLOW_DISCARD_OLD, NULL);
        xSemaphoreGive(s_ring_mtx[port]);
        if (!ok) {
            xSemaphoreGive(s_tx_space[port]);
            break;
        }
        sent++;
    }
    return sent;
}

size_t uart_manager_receive(uart_port_id_t port, void *buf, size_t len, TickType_t ticks_to_wait)
{
    if (port >= UART_MGR_NUM_PORTS || !buf) {
        return 0;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;
    TickType_t deadline = xTaskGetTickCount() + ticks_to_wait;
    while (n < len) {
        TickType_t left = deadline - xTaskGetTickCount();
        if (left <= 0 && n > 0) {
            break;
        }
        if (xSemaphoreTake(s_rx_sem[port], (n == 0) ? left : 0) != pdTRUE) {
            break;
        }
        xSemaphoreTake(s_ring_mtx[port], portMAX_DELAY);
        uint8_t b = 0;
        if (!ring_get(&s_ports[port].rx, &b)) {
            xSemaphoreGive(s_ring_mtx[port]);
            continue;
        }
        xSemaphoreGive(s_ring_mtx[port]);
        out[n++] = b;
    }
    return n;
}

EventBits_t uart_manager_wait_event(uart_port_id_t port, EventBits_t bits, bool clear_on_exit,
                                    TickType_t ticks_to_wait)
{
    if (port >= UART_MGR_NUM_PORTS) {
        return 0;
    }
    EventBits_t wait = 0;
    if (bits & UART_MGR_EVT_RX_READY) {
        wait |= (EventBits_t)(1u << port);
    }
    if (wait == 0) {
        return 0;
    }
    return xEventGroupWaitBits(s_rx_ready_bits, wait, clear_on_exit ? pdTRUE : pdFALSE, pdFALSE, ticks_to_wait);
}

esp_err_t uart_manager_get_stats(uart_port_id_t port, uart_manager_stats_t *out_stats)
{
    if (port >= UART_MGR_NUM_PORTS || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stats = s_ports[port].stats;
    return ESP_OK;
}

esp_err_t uart_manager_reset_stats(uart_port_id_t port)
{
    if (port >= UART_MGR_NUM_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_ports[port].stats, 0, sizeof(s_ports[port].stats));
    return ESP_OK;
}

#if defined(CONFIG_UART_MGR_ENABLE_TEST_TASK) && CONFIG_UART_MGR_ENABLE_TEST_TASK
void uart_mgr_consumer_test_task(void *arg)
{
    (void)arg;
    uint8_t buf[32];
    for (;;) {
        EventBits_t b = uart_manager_wait_event(0, UART_MGR_EVT_RX_READY, pdTRUE, pdMS_TO_TICKS(5000));
        if (b) {
            size_t n = uart_manager_receive(0, buf, sizeof(buf), pdMS_TO_TICKS(100));
            if (n > 0) {
                ESP_LOGI(TAG, "test rx port0: %u bytes", (unsigned)n);
            }
        }
    }
}
#endif
