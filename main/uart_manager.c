/*
 * uart_manager — Frontier_1: UART, кольца RX/TX, NVS, задача uart_mgr.
 * Версия модуля: UART_MANAGER_MODULE_VERSION (см. uart_manager.h).
 */

#include "uart_manager.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_UART_MGR_RX_EVENT_WAIT_MS
#define CONFIG_UART_MGR_RX_EVENT_WAIT_MS 3
#endif

static const char *TAG = "uart_mgr";

#if defined(CONFIG_UART_MGR_ENABLE_TEST_TASK) && CONFIG_UART_MGR_ENABLE_TEST_TASK
static const char *TAG_ECHO = "uart_echo";
#endif

/** Периодический отладочный снимок (раз в ~5 с). */
static TickType_t s_uart_mgr_hb_tick;
/** Периодический ESP_LOGI: статистика и заполнение колец (~10 с). */
static TickType_t s_uart_status_log_tick;
/** Ограничение частоты W по кадровым ошибкам. */
static TickType_t s_uart_last_line_err_log_tick;

#define UART_F1_ACTIVE_PORT ((uart_port_id_t)0)

#define UART_NVS_NAMESPACE "uart_mgr"
#define UART_NVS_KEY_F1 "f1_cfg_v1"

#define UART_NVS_MAGIC 0x554d4631u
/** Запись в NVS без поля CRC (совместимость со старыми прошивками). */
#define UART_NVS_VERSION_V1 1u
/** Текущий формат: полезная нагрузка + CRC32 по полезной нагрузке. */
#define UART_NVS_VERSION 2u

#define UART_DRIVER_RX_BUF 256
#define UART_DRIVER_TX_BUF 256
#define UART_QUEUE_LEN 20

#define SCRATCH_READ 128

/** Полезная нагрузка NVS (28 байт packed); CRC считается по всему этому блоку. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t baud;
    uint8_t data_bits;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t line_mode;
    uint32_t ring_rx_size;
    uint32_t ring_tx_size;
    uint8_t overflow;
    uint8_t pad[3];
} uart_nvs_f1_payload_t;

typedef struct __attribute__((packed)) {
    uart_nvs_f1_payload_t pl;
    uint32_t crc;
} uart_nvs_f1_blob_t;

_Static_assert(sizeof(uart_nvs_f1_payload_t) == 28, "uart NVS payload packed size");
_Static_assert(sizeof(uart_nvs_f1_blob_t) == 32, "uart NVS blob with CRC");

typedef struct {
    uint8_t *pool;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
} uart_ring_t;

typedef struct {
    uart_ring_t rx;
    uart_ring_t tx;
    uart_manager_port_config_t cfg;
    uart_manager_stats_t stats;
} uart_port_ctx_t;

static uart_port_ctx_t s_port;
static bool s_inited;
static volatile bool s_uart_task_run;
static SemaphoreHandle_t s_uart_task_exit_sem;

static SemaphoreHandle_t s_port_mtx;
static SemaphoreHandle_t s_rx_sem;
static EventGroupHandle_t s_evtgroup;
static QueueHandle_t s_uart_queue;
static TaskHandle_t s_uart_task;
static bool s_logged_unsupported_port;

static void free_pools(void);

static uart_port_t s_uart_num(void)
{
    return (uart_port_t)CONFIG_UART_MGR_F1_UART_NUM;
}

static void log_unsupported_port_once(void)
{
    if (!s_logged_unsupported_port) {
        s_logged_unsupported_port = true;
        ESP_LOGW(TAG, "Frontier_1: только порт %u; для других индексов — ESP_ERR_NOT_SUPPORTED / нулевой результат",
                 (unsigned)UART_F1_ACTIVE_PORT);
    }
}

/** Откат частичной инициализации `uart_manager_init` (порядок обратный созданию). */
static void uart_mgr_init_abort(uart_port_t uport, bool have_pools, bool have_sync, bool have_driver)
{
    if (have_driver) {
        uart_driver_delete(uport);
        s_uart_queue = NULL;
    }
    if (have_sync) {
        if (s_port_mtx) {
            vSemaphoreDelete(s_port_mtx);
            s_port_mtx = NULL;
        }
        if (s_rx_sem) {
            vSemaphoreDelete(s_rx_sem);
            s_rx_sem = NULL;
        }
        if (s_evtgroup) {
            vEventGroupDelete(s_evtgroup);
            s_evtgroup = NULL;
        }
    }
    if (have_pools) {
        free_pools();
    }
    s_uart_task = NULL;
}

static uart_manager_port_config_t default_port_config(void)
{
    uart_manager_port_config_t c = {
        .baud = UART_MGR_DEFAULT_BAUD,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = UART_MGR_PARITY_NONE,
        .line_mode = UART_MGR_LINE_RS232,
        .ring_rx_size = 4096,
        .ring_tx_size = 4096,
        .overflow = UART_MGR_OVERFLOW_DISCARD_OLD,
    };
    return c;
}

static size_t ring_effective_cap(uint32_t requested)
{
    if (requested < 2) {
        return 2;
    }
    if (requested > UART_MGR_F1_RING_POOL_BYTES) {
        return UART_MGR_F1_RING_POOL_BYTES;
    }
    return (size_t)requested;
}

static void ring_reset(uart_ring_t *r, size_t cap)
{
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static bool ring_put(uart_ring_t *r, uint8_t b, uart_mgr_overflow_policy_t pol, uint32_t *overflow_cnt)
{
    if (r->count >= r->cap) {
        if (pol == UART_MGR_OVERFLOW_DISCARD_OLD) {
            if (r->count > 0) {
                r->tail = (r->tail + 1) % r->cap;
                r->count--;
                if (overflow_cnt) {
                    (*overflow_cnt)++;
                }
            }
        } else {
            if (overflow_cnt) {
                (*overflow_cnt)++;
            }
            return false;
        }
    }
    r->pool[r->head] = b;
    r->head = (r->head + 1) % r->cap;
    r->count++;
    return true;
}

static bool ring_get(uart_ring_t *r, uint8_t *b)
{
    if (r->count == 0) {
        return false;
    }
    *b = r->pool[r->tail];
    r->tail = (r->tail + 1) % r->cap;
    r->count--;
    return true;
}

/** Вернуть один байт в начало очереди (после неудачной передачи в UART). */
static bool ring_unget_byte(uart_ring_t *r, uint8_t b)
{
    if (r->count >= r->cap) {
        return false;
    }
    if (r->tail == 0) {
        r->tail = r->cap - 1;
    } else {
        r->tail--;
    }
    r->pool[r->tail] = b;
    r->count++;
    return true;
}

static size_t ring_put_multi(uart_ring_t *r, const uint8_t *data, size_t len, uart_mgr_overflow_policy_t pol,
                             uint32_t *overflow_cnt, size_t *accepted)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (!ring_put(r, data[i], pol, overflow_cnt)) {
            break;
        }
        n++;
    }
    if (accepted) {
        *accepted = n;
    }
    return n;
}

static size_t ring_get_multi(uart_ring_t *r, uint8_t *data, size_t len)
{
    size_t n = 0;
    while (n < len && r->count > 0) {
        uint8_t b;
        if (!ring_get(r, &b)) {
            break;
        }
        data[n++] = b;
    }
    return n;
}

static esp_err_t nvs_blob_to_cfg(const uart_nvs_f1_payload_t *b, uart_manager_port_config_t *out)
{
    if (b->magic != UART_NVS_MAGIC || (b->version != UART_NVS_VERSION_V1 && b->version != UART_NVS_VERSION)) {
        return ESP_ERR_INVALID_STATE;
    }
    out->baud = b->baud;
    if (out->baud < 300 || out->baud > 2000000) {
        return ESP_ERR_INVALID_STATE;
    }
    out->data_bits = b->data_bits;
    out->stop_bits = b->stop_bits;
    out->parity = (uart_mgr_parity_t)b->parity;
    out->line_mode = (uart_mgr_line_mode_t)b->line_mode;
    out->ring_rx_size = b->ring_rx_size;
    out->ring_tx_size = b->ring_tx_size;
    out->overflow = (uart_mgr_overflow_policy_t)b->overflow;
    if (out->data_bits < 5 || out->data_bits > 8) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out->stop_bits < 1 || out->stop_bits > 3) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out->parity > UART_MGR_PARITY_EVEN) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out->line_mode > UART_MGR_LINE_RS422) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out->overflow > UART_MGR_OVERFLOW_STOP_RX) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void cfg_to_blob(const uart_manager_port_config_t *cfg, uart_nvs_f1_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->pl.magic = UART_NVS_MAGIC;
    b->pl.version = UART_NVS_VERSION;
    b->pl.baud = cfg->baud;
    b->pl.data_bits = cfg->data_bits;
    b->pl.stop_bits = cfg->stop_bits;
    b->pl.parity = (uint8_t)cfg->parity;
    b->pl.line_mode = (uint8_t)cfg->line_mode;
    b->pl.ring_rx_size = cfg->ring_rx_size;
    b->pl.ring_tx_size = cfg->ring_tx_size;
    b->pl.overflow = (uint8_t)cfg->overflow;
    b->crc = esp_crc32_le(0, (const uint8_t *)&b->pl, sizeof(b->pl));
}

static esp_err_t uart_hw_apply_params(void)
{
    const uart_manager_port_config_t *c = &s_port.cfg;
    uart_config_t ucfg = {
        .baud_rate = (int)c->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    switch (c->data_bits) {
        case 5:
            ucfg.data_bits = UART_DATA_5_BITS;
            break;
        case 6:
            ucfg.data_bits = UART_DATA_6_BITS;
            break;
        case 7:
            ucfg.data_bits = UART_DATA_7_BITS;
            break;
        default:
            ucfg.data_bits = UART_DATA_8_BITS;
            break;
    }

    switch (c->parity) {
        case UART_MGR_PARITY_ODD:
            ucfg.parity = UART_PARITY_ODD;
            break;
        case UART_MGR_PARITY_EVEN:
            ucfg.parity = UART_PARITY_EVEN;
            break;
        default:
            ucfg.parity = UART_PARITY_DISABLE;
            break;
    }

    if (c->stop_bits == 2) {
        ucfg.stop_bits = UART_STOP_BITS_2;
    } else if (c->stop_bits == 1) {
        ucfg.stop_bits = UART_STOP_BITS_1;
    } else {
        ucfg.stop_bits = UART_STOP_BITS_1_5;
    }

    return uart_param_config(s_uart_num(), &ucfg);
}

static void uart_mgr_notify_task(void)
{
    if (s_uart_task) {
        xTaskNotifyGive(s_uart_task);
    }
}

static void rx_signal_non_empty_task(void)
{
    xSemaphoreGive(s_rx_sem);
}

static void uart_mgr_status_log_periodic(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_uart_status_log_tick == 0) {
        s_uart_status_log_tick = now;
        return;
    }
    if ((now - s_uart_status_log_tick) < pdMS_TO_TICKS(10000)) {
        return;
    }
    s_uart_status_log_tick = now;

    if (xSemaphoreTake(s_port_mtx, pdMS_TO_TICKS(40)) != pdTRUE) {
        return;
    }
    size_t rx_cap = ring_effective_cap(s_port.cfg.ring_rx_size);
    size_t tx_cap = ring_effective_cap(s_port.cfg.ring_tx_size);
    unsigned rx_n = (unsigned)s_port.rx.count;
    unsigned tx_n = (unsigned)s_port.tx.count;
    const uart_manager_stats_t st = s_port.stats;
    const uart_mgr_overflow_policy_t pol = s_port.cfg.overflow;
    xSemaphoreGive(s_port_mtx);

    ESP_LOGI(TAG,
             "status: rx=%lu B tx=%lu B | rx_overflows=%lu framing_err=%lu | RX ring %u/%u B TX ring %u/%u B | overflow_pol=%d",
             (unsigned long)st.rx_bytes, (unsigned long)st.tx_bytes, (unsigned long)st.rx_overflows,
             (unsigned long)st.framing_errors, rx_n, (unsigned)rx_cap, tx_n, (unsigned)tx_cap, (int)pol);
}

#if defined(CONFIG_UART_MGR_VERBOSE_LOG) && CONFIG_UART_MGR_VERBOSE_LOG
static void uart_mgr_debug_heartbeat(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_uart_mgr_hb_tick == 0) {
        s_uart_mgr_hb_tick = now;
        return;
    }
    if ((now - s_uart_mgr_hb_tick) < pdMS_TO_TICKS(5000)) {
        return;
    }
    s_uart_mgr_hb_tick = now;

    if (xSemaphoreTake(s_port_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    size_t rx_cap = ring_effective_cap(s_port.cfg.ring_rx_size);
    size_t tx_cap = ring_effective_cap(s_port.cfg.ring_tx_size);
    unsigned rx_n = (unsigned)s_port.rx.count;
    unsigned tx_n = (unsigned)s_port.tx.count;
    uart_manager_stats_t st = s_port.stats;
    xSemaphoreGive(s_port_mtx);

    ESP_LOGD(TAG,
             "heartbeat: RX кольцо %u/%u B, TX кольцо %u/%u B | всего rx=%lu tx=%lu ovf=%lu fe=%lu",
             rx_n, (unsigned)rx_cap, tx_n, (unsigned)tx_cap, (unsigned long)st.rx_bytes, (unsigned long)st.tx_bytes,
             (unsigned long)st.rx_overflows, (unsigned long)st.framing_errors);
}
#else
static void uart_mgr_debug_heartbeat(void) {}
#endif

/** Один элемент из очереди событий `uart_driver_install` (ошибки линии, переполнения; UART_DATA — no-op). */
static void uart_mgr_dispatch_uart_event(const uart_event_t *evt)
{
#if defined(CONFIG_UART_MGR_VERBOSE_LOG) && CONFIG_UART_MGR_VERBOSE_LOG
    ESP_LOGD(TAG, "событие UART: type=%d size=%d", (int)evt->type, (int)evt->size);
#endif
    switch (evt->type) {
    case UART_FRAME_ERR:
    case UART_PARITY_ERR:
        if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
            s_port.stats.framing_errors++;
            uint32_t fe = s_port.stats.framing_errors;
            xSemaphoreGive(s_port_mtx);
            TickType_t tn = xTaskGetTickCount();
            if ((tn - s_uart_last_line_err_log_tick) >= pdMS_TO_TICKS(2000)) {
                s_uart_last_line_err_log_tick = tn;
                ESP_LOGW(TAG, "UART line error: type=%d (FRAME/PARITY), framing_errors_total=%lu", (int)evt->type,
                         (unsigned long)fe);
            }
        }
        xEventGroupSetBits(s_evtgroup, UART_MGR_EVT_LINE_ERR);
        break;
    case UART_BUFFER_FULL:
        if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
            s_port.stats.rx_overflows++;
            uint32_t ov = s_port.stats.rx_overflows;
            xSemaphoreGive(s_port_mtx);
            ESP_LOGW(TAG, "UART hardware RX buffer full (UART_BUFFER_FULL), rx_overflows_total=%lu", (unsigned long)ov);
        }
        xEventGroupSetBits(s_evtgroup, UART_MGR_EVT_RX_OVERFLOW);
        break;
    case UART_FIFO_OVF:
        if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
            s_port.stats.rx_overflows++;
            uint32_t ov = s_port.stats.rx_overflows;
            xSemaphoreGive(s_port_mtx);
            ESP_LOGW(TAG, "UART HW RX FIFO overflow (UART_FIFO_OVF), rx_overflows_total=%lu", (unsigned long)ov);
        }
        xEventGroupSetBits(s_evtgroup, UART_MGR_EVT_RX_OVERFLOW);
        break;
    case UART_DATA:
#if defined(CONFIG_UART_MGR_VERBOSE_LOG) && CONFIG_UART_MGR_VERBOSE_LOG
        ESP_LOGD(TAG, "UART_DATA size=%d timeout=%d", (int)evt->size, (int)evt->timeout_flag);
#endif
        break;
    default:
        break;
    }
}

/**
 * Ждём TX notify / deinit, событие RX из `s_uart_queue` (UART_DATA и др.) или истечение ~20 ms (heartbeat).
 * Реакция на RX привязана к событиям драйвера; слайс очереди — CONFIG_UART_MGR_RX_EVENT_WAIT_MS.
 */
static void uart_mgr_wait_for_work(void)
{
    TickType_t period = pdMS_TO_TICKS(20);
    TickType_t slice_cfg = pdMS_TO_TICKS(CONFIG_UART_MGR_RX_EVENT_WAIT_MS);
    if (period == 0) {
        period = 1;
    }
    if (slice_cfg == 0) {
        slice_cfg = 1;
    }
    TickType_t start = xTaskGetTickCount();
    uart_event_t evt;

    for (;;) {
        if (!s_uart_task_run) {
            return;
        }

        if (ulTaskNotifyTake(pdTRUE, 0) > 0U) {
            return;
        }

        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= period) {
            return;
        }

        TickType_t remain = period - elapsed;
        TickType_t slice = slice_cfg;
        if (slice > remain) {
            slice = remain;
        }
        if (xQueueReceive(s_uart_queue, &evt, slice) == pdTRUE) {
            uart_mgr_dispatch_uart_event(&evt);
            return;
        }
    }
}

static void uart_mgr_task(void *arg)
{
    (void)arg;
    uint8_t scratch[SCRATCH_READ];
    uart_event_t evt;

    for (;;) {
        if (!s_uart_task_run) {
            break;
        }

        uart_mgr_wait_for_work();

        if (!s_uart_task_run) {
            break;
        }

        int nread;
        do {
            nread = uart_read_bytes(s_uart_num(), scratch, sizeof(scratch), 0);
            if (nread > 0) {
#if defined(CONFIG_UART_MGR_VERBOSE_LOG) && CONFIG_UART_MGR_VERBOSE_LOG
                ESP_LOGD(TAG, "из UART (драйвер) -> кольцо RX: %d B", nread);
#endif
                if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
                    uint32_t ov_before = s_port.stats.rx_overflows;
                    for (int i = 0; i < nread; i++) {
                        bool ok = ring_put(&s_port.rx, scratch[i], s_port.cfg.overflow, &s_port.stats.rx_overflows);
                        if (ok) {
                            s_port.stats.rx_bytes++;
                        }
                        if (!ok && s_port.cfg.overflow == UART_MGR_OVERFLOW_STOP_RX) {
                            break;
                        }
                    }
                    bool had_data = s_port.rx.count > 0;
                    if (s_port.stats.rx_overflows > ov_before) {
                        uint32_t d = s_port.stats.rx_overflows - ov_before;
                        ESP_LOGW(TAG,
                                 "RX software ring: overflow counter +%lu (total=%lu, policy=%d; при DISCARD — счётчик на потерянный байт)",
                                 (unsigned long)d, (unsigned long)s_port.stats.rx_overflows, (int)s_port.cfg.overflow);
                        xEventGroupSetBits(s_evtgroup, UART_MGR_EVT_RX_OVERFLOW);
                    }
                    xSemaphoreGive(s_port_mtx);
                    if (had_data) {
                        rx_signal_non_empty_task();
                        xEventGroupSetBits(s_evtgroup, UART_MGR_EVT_RX_READY);
                    }
                }
            }
        } while (nread > 0);

        for (;;) {
            if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
                break;
            }
            if (s_port.tx.count == 0) {
                xSemaphoreGive(s_port_mtx);
                break;
            }
            uint8_t b;
            if (!ring_get(&s_port.tx, &b)) {
                xSemaphoreGive(s_port_mtx);
                break;
            }
            xSemaphoreGive(s_port_mtx);

            int w = uart_write_bytes(s_uart_num(), &b, 1);
            if (w <= 0) {
                if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
                    (void)ring_unget_byte(&s_port.tx, b);
                    xSemaphoreGive(s_port_mtx);
                }
                break;
            }
            if (w > 0) {
                if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) == pdTRUE) {
                    s_port.stats.tx_bytes += (uint32_t)w;
                    xSemaphoreGive(s_port_mtx);
                }
            }
        }

        while (xQueueReceive(s_uart_queue, &evt, 0) == pdTRUE) {
            uart_mgr_dispatch_uart_event(&evt);
        }

        uart_mgr_status_log_periodic();
        uart_mgr_debug_heartbeat();
    }

    if (s_uart_task_exit_sem) {
        xSemaphoreGive(s_uart_task_exit_sem);
    }
    vTaskDelete(NULL);
}

static esp_err_t alloc_pools(void)
{
    s_port.rx.pool = (uint8_t *)heap_caps_malloc(UART_MGR_F1_RING_POOL_BYTES, MALLOC_CAP_INTERNAL);
    s_port.tx.pool = (uint8_t *)heap_caps_malloc(UART_MGR_F1_RING_POOL_BYTES, MALLOC_CAP_INTERNAL);
    if (!s_port.rx.pool || !s_port.tx.pool) {
        free_pools();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void free_pools(void)
{
    heap_caps_free(s_port.rx.pool);
    heap_caps_free(s_port.tx.pool);
    s_port.rx.pool = NULL;
    s_port.tx.pool = NULL;
}

static esp_err_t apply_ring_caps_from_cfg(void)
{
    size_t rx_cap = ring_effective_cap(s_port.cfg.ring_rx_size);
    size_t tx_cap = ring_effective_cap(s_port.cfg.ring_tx_size);
    ring_reset(&s_port.rx, rx_cap);
    ring_reset(&s_port.tx, tx_cap);
    return ESP_OK;
}

esp_err_t uart_manager_load_config_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(UART_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    uart_nvs_f1_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, UART_NVS_KEY_F1, &blob, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS: нет сохранённого конфига uart, оставляем текущие/дефолтные");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_get_blob");

    if (sz != sizeof(uart_nvs_f1_payload_t) && sz != sizeof(uart_nvs_f1_blob_t)) {
        ESP_LOGW(TAG, "NVS: неверный размер блоба uart (%u B, ожид. %u или %u), игнор",
                 (unsigned)sz, (unsigned)sizeof(uart_nvs_f1_payload_t), (unsigned)sizeof(uart_nvs_f1_blob_t));
        return ESP_OK;
    }

    uart_manager_port_config_t tmp;
    esp_err_t dec = ESP_ERR_INVALID_STATE;
    if (sz == sizeof(uart_nvs_f1_payload_t)) {
        dec = nvs_blob_to_cfg((const uart_nvs_f1_payload_t *)&blob, &tmp);
    } else {
        uint32_t crc_expect = esp_crc32_le(0, (const uint8_t *)&blob.pl, sizeof(blob.pl));
        if (crc_expect != blob.crc) {
            ESP_LOGW(TAG, "NVS: CRC блоба uart не совпадает, игнор");
            return ESP_OK;
        }
        dec = nvs_blob_to_cfg(&blob.pl, &tmp);
    }
    if (dec != ESP_OK) {
        ESP_LOGW(TAG, "NVS: битый или устаревший блоб uart, игнор");
        return ESP_OK;
    }

    if (s_inited) {
        return uart_manager_configure_port(UART_F1_ACTIVE_PORT, &tmp);
    }
    s_port.cfg = tmp;
    return ESP_OK;
}

esp_err_t uart_manager_save_config_to_nvs(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    uart_nvs_f1_blob_t blob;
    cfg_to_blob(&s_port.cfg, &blob);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(UART_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open rw");
    esp_err_t err = nvs_set_blob(h, UART_NVS_KEY_F1, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs save");
    ESP_LOGI(TAG, "Конфиг uart сохранён в NVS");
    return ESP_OK;
}

esp_err_t uart_manager_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    const uart_port_t uport = s_uart_num();
    bool have_pools = false;
    bool have_sync = false;
    bool have_driver = false;

    memset(&s_port.stats, 0, sizeof(s_port.stats));
    s_port.cfg = default_port_config();

    esp_err_t err = uart_manager_load_config_from_nvs();
    if (err != ESP_OK) {
        return err;
    }

    err = alloc_pools();
    if (err != ESP_OK) {
        return err;
    }
    have_pools = true;

    err = apply_ring_caps_from_cfg();
    if (err != ESP_OK) {
        uart_mgr_init_abort(uport, true, false, false);
        return err;
    }

    s_port_mtx = xSemaphoreCreateMutex();
    s_rx_sem = xSemaphoreCreateBinary();
    s_evtgroup = xEventGroupCreate();
    if (!s_port_mtx || !s_rx_sem || !s_evtgroup) {
        uart_mgr_init_abort(uport, have_pools, false, false);
        return ESP_ERR_NO_MEM;
    }
    have_sync = true;

    err = uart_driver_install(uport, UART_DRIVER_RX_BUF, UART_DRIVER_TX_BUF, UART_QUEUE_LEN, &s_uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        uart_mgr_init_abort(uport, have_pools, have_sync, false);
        return err;
    }
    have_driver = true;

    err = uart_hw_apply_params();
    if (err != ESP_OK) {
        uart_mgr_init_abort(uport, have_pools, have_sync, have_driver);
        return err;
    }

    err = uart_set_pin(uport, CONFIG_UART_MGR_F1_TX_GPIO, CONFIG_UART_MGR_F1_RX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_mgr_init_abort(uport, have_pools, have_sync, have_driver);
        return err;
    }

    s_uart_task_exit_sem = xSemaphoreCreateBinary();
    if (!s_uart_task_exit_sem) {
        uart_mgr_init_abort(uport, have_pools, have_sync, have_driver);
        return ESP_ERR_NO_MEM;
    }

    s_uart_task_run = true;
    BaseType_t ok = xTaskCreatePinnedToCore(uart_mgr_task, "uart_mgr", CONFIG_UART_MGR_TASK_STACK, NULL,
                                            CONFIG_UART_MGR_TASK_PRIORITY, &s_uart_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_uart_task_exit_sem);
        s_uart_task_exit_sem = NULL;
        s_uart_task_run = false;
        uart_mgr_init_abort(uport, have_pools, have_sync, have_driver);
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    {
        TickType_t period_ticks = pdMS_TO_TICKS(20);
        TickType_t slice_ticks = pdMS_TO_TICKS(CONFIG_UART_MGR_RX_EVENT_WAIT_MS);
        if (period_ticks == 0) {
            period_ticks = 1;
        }
        if (slice_ticks == 0) {
            slice_ticks = 1;
        }
        const uint32_t tick_hz = (uint32_t)configTICK_RATE_HZ;
        const uint32_t period_ms_eff = (uint32_t)((1000ULL * (uint64_t)period_ticks) / (uint64_t)tick_hz);
        const uint32_t slice_ms_eff = (uint32_t)((1000ULL * (uint64_t)slice_ticks) / (uint64_t)tick_hz);
        ESP_LOGI(TAG,
                 "rx wait cfg: tick_hz=%lu, cfg_wait_ms=%d => slice=%lu tick (~%lu ms), period=%lu tick (~%lu ms)",
                 (unsigned long)tick_hz, (int)CONFIG_UART_MGR_RX_EVENT_WAIT_MS, (unsigned long)slice_ticks,
                 (unsigned long)slice_ms_eff, (unsigned long)period_ticks, (unsigned long)period_ms_eff);
    }
    ESP_LOGI(TAG, "uart_manager init OK (UART%d, TX=%d RX=%d)", (int)s_uart_num(), CONFIG_UART_MGR_F1_TX_GPIO,
             CONFIG_UART_MGR_F1_RX_GPIO);
    ESP_LOGD(TAG,
             "дефолты порта: baud=%lu data=%u stop=%u parity=%d line=%d ring_rx=%lu ring_tx=%lu overflow=%d",
             (unsigned long)s_port.cfg.baud, (unsigned)s_port.cfg.data_bits, (unsigned)s_port.cfg.stop_bits,
             (int)s_port.cfg.parity, (int)s_port.cfg.line_mode, (unsigned long)s_port.cfg.ring_rx_size,
             (unsigned long)s_port.cfg.ring_tx_size, (int)s_port.cfg.overflow);
    return ESP_OK;
}

void uart_manager_deinit(void)
{
    if (!s_inited) {
        return;
    }

    /* Block new API users as early as possible; then stop task and release resources. */
    s_inited = false;
    s_uart_task_run = false;
    uart_mgr_notify_task();

    const TickType_t join_wait = pdMS_TO_TICKS(5000);
    if (s_uart_task_exit_sem) {
        if (xSemaphoreTake(s_uart_task_exit_sem, join_wait) != pdTRUE) {
            ESP_LOGW(TAG, "deinit: uart_mgr не завершилась за 5 с (избегайте deinit при удержании s_port_mtx)");
        }
        vSemaphoreDelete(s_uart_task_exit_sem);
        s_uart_task_exit_sem = NULL;
    }
    s_uart_task = NULL;

    uart_driver_delete(s_uart_num());
    s_uart_queue = NULL;

    vSemaphoreDelete(s_port_mtx);
    s_port_mtx = NULL;
    vSemaphoreDelete(s_rx_sem);
    s_rx_sem = NULL;
    vEventGroupDelete(s_evtgroup);
    s_evtgroup = NULL;

    free_pools();
}

esp_err_t uart_manager_configure_port(uart_port_id_t port, const uart_manager_port_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port != UART_F1_ACTIVE_PORT) {
        log_unsupported_port_once();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_manager_port_config_t c = *cfg;
    c.ring_rx_size = (uint32_t)ring_effective_cap(c.ring_rx_size);
    c.ring_tx_size = (uint32_t)ring_effective_cap(c.ring_tx_size);

    if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    s_port.cfg = c;
    apply_ring_caps_from_cfg();
    esp_err_t e = uart_hw_apply_params();
    xSemaphoreGive(s_port_mtx);

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "uart_hw_apply_params: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t uart_manager_get_port_config(uart_port_id_t port, uart_manager_port_config_t *out_cfg)
{
    if (!out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port != UART_F1_ACTIVE_PORT) {
        log_unsupported_port_once();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_port_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_cfg = s_port.cfg;
    xSemaphoreGive(s_port_mtx);
    return ESP_OK;
}

esp_err_t uart_manager_set_line_mode(uart_port_id_t port, uart_mgr_line_mode_t mode)
{
    if (port != UART_F1_ACTIVE_PORT) {
        log_unsupported_port_once();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mode > UART_MGR_LINE_RS422) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    s_port.cfg.line_mode = mode;
    xSemaphoreGive(s_port_mtx);
    ESP_LOGI(TAG, "line_mode=%d (Frontier_1: без внешнего мультиплексора, только конфиг)", (int)mode);
    return ESP_OK;
}

size_t uart_manager_send(uart_port_id_t port, const void *data, size_t len, TickType_t ticks_to_wait)
{
    if (port != UART_F1_ACTIVE_PORT) {
        if (s_inited) {
            log_unsupported_port_once();
        }
        return 0;
    }
    if (!data || !s_inited || len == 0) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t total = 0;
    const TickType_t start = xTaskGetTickCount();

    while (total < len) {
        if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
            break;
        }
        size_t accepted = 0;
        ring_put_multi(&s_port.tx, p + total, len - total, s_port.cfg.overflow, NULL, &accepted);
        xSemaphoreGive(s_port_mtx);

        if (accepted > 0) {
            uart_mgr_notify_task();
        }
        total += accepted;

        if (total >= len) {
            break;
        }
        if (ticks_to_wait == 0) {
            break;
        }

        TickType_t wait = portMAX_DELAY;
        if (ticks_to_wait != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= ticks_to_wait) {
                break;
            }
            wait = ticks_to_wait - elapsed;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
    return total;
}

size_t uart_manager_receive(uart_port_id_t port, void *buf, size_t len, TickType_t ticks_to_wait)
{
    if (port != UART_F1_ACTIVE_PORT) {
        if (s_inited) {
            log_unsupported_port_once();
        }
        return 0;
    }
    if (!buf || len == 0 || !s_inited) {
        return 0;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t total = 0;
    const TickType_t start = xTaskGetTickCount();

    while (total < len) {
        if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
            break;
        }
        size_t n = ring_get_multi(&s_port.rx, out + total, len - total);
        bool still_more = s_port.rx.count > 0;
        xSemaphoreGive(s_port_mtx);

        if (n > 0) {
            total += n;
            if (!still_more) {
                xEventGroupClearBits(s_evtgroup, UART_MGR_EVT_RX_READY);
            }
        }

        if (total >= len) {
            break;
        }
        if (ticks_to_wait == 0) {
            break;
        }
        if (n > 0 && still_more) {
            continue;
        }

        TickType_t wait = portMAX_DELAY;
        if (ticks_to_wait != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= ticks_to_wait) {
                break;
            }
            wait = ticks_to_wait - elapsed;
        }
        if (xSemaphoreTake(s_rx_sem, wait) != pdTRUE) {
            break;
        }
    }

    return total;
}

EventBits_t uart_manager_wait_event(uart_port_id_t port, EventBits_t bits, bool clear_on_exit,
                                    TickType_t ticks_to_wait)
{
    if (port != UART_F1_ACTIVE_PORT || !s_inited) {
        if (s_inited && port != UART_F1_ACTIVE_PORT) {
            log_unsupported_port_once();
        }
        return 0;
    }
    EventBits_t b =
        xEventGroupWaitBits(s_evtgroup, bits, clear_on_exit ? pdTRUE : pdFALSE, pdFALSE, ticks_to_wait);
    return b;
}

esp_err_t uart_manager_get_stats(uart_port_id_t port, uart_manager_stats_t *out_stats)
{
    if (!out_stats || !s_inited) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port != UART_F1_ACTIVE_PORT) {
        log_unsupported_port_once();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    *out_stats = s_port.stats;
    xSemaphoreGive(s_port_mtx);
    return ESP_OK;
}

esp_err_t uart_manager_reset_stats(uart_port_id_t port)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port != UART_F1_ACTIVE_PORT) {
        log_unsupported_port_once();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_port_mtx, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    memset(&s_port.stats, 0, sizeof(s_port.stats));
    xSemaphoreGive(s_port_mtx);
    return ESP_OK;
}

#if defined(CONFIG_UART_MGR_ENABLE_TEST_TASK) && CONFIG_UART_MGR_ENABLE_TEST_TASK
void uart_mgr_consumer_test_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    TickType_t last_tx = xTaskGetTickCount();
    TickType_t last_status = last_tx;
    uint32_t seq = 0;
    uint32_t loop_count = 0;

    /* WARN — гарантированно видно при esp_log_level_set("MAIN", WARN) на других тегах. */
    ESP_LOGW(TAG_ECHO, "=== СТАРТ uart_echo: тест TX-RX, период 1 с, лог RX/TX (порт 0) ===");
    ESP_LOGI(TAG_ECHO, "FreeRTOS: задача=%s приоритет=%d", pcTaskGetName(NULL), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        loop_count++;
        size_t n = uart_manager_receive(UART_F1_ACTIVE_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            ESP_LOGI(TAG_ECHO, "RX %u байт:", (unsigned)n);
            ESP_LOG_BUFFER_HEXDUMP(TAG_ECHO, buf, n, ESP_LOG_INFO);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_tx) >= pdMS_TO_TICKS(1000)) {
            last_tx = now;
            seq++;
            char txline[64];
            int len = snprintf(txline, sizeof(txline), "LBK %lu\r\n", (unsigned long)seq);
            if (len <= 0 || len >= (int)sizeof(txline)) {
                const char *fallback = "LBK ?\r\n";
                len = (int)strlen(fallback);
                memcpy(txline, fallback, (size_t)len);
            }

            ESP_LOGI(TAG_ECHO, "TX цикл #%lu, пакет %d байт:", (unsigned long)seq, len);
            ESP_LOG_BUFFER_HEXDUMP(TAG_ECHO, txline, (size_t)len, ESP_LOG_INFO);

            size_t sent = 0;
            while (sent < (size_t)len) {
                sent += uart_manager_send(UART_F1_ACTIVE_PORT, txline + sent, (size_t)len - sent, portMAX_DELAY);
            }
            ESP_LOGI(TAG_ECHO, "в линию ушло %u байт", (unsigned)sent);
        }

        /* Раз в 3 с: задача жива, стек, число итераций цикла за интервал. */
        if ((now - last_status) >= pdMS_TO_TICKS(3000)) {
            last_status = now;
            UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
            TickType_t next_tx_at = last_tx + pdMS_TO_TICKS(1000);
            uint32_t ms_to_tx = (now < next_tx_at) ? pdTICKS_TO_MS(next_tx_at - now) : 0;
            ESP_LOGW(TAG_ECHO,
                     "статус: OK tick=%lu, итераций_за_3с=%lu, до след.TX ~%lu ms, стек своб. min=%u слов",
                     (unsigned long)now, (unsigned long)loop_count, (unsigned long)ms_to_tx, (unsigned)hw);
            loop_count = 0;
        }
    }
}
#endif
