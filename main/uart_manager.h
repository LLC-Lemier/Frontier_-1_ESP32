/*
 * uart_manager — менеджер UART (модуль uart_mng).
 *
 * Версия модуля (ПО): 0.3.0
 *
 * Аппаратная линия: Frontier_1. ТЗ: одноканальный UART, без SPI (см. HARDWARE.md — разводка GPIO).
 * Реализация: кольцевые буферы RX/TX (пул по 64 КиБ), драйвер UART, NVS, задача uart_mgr.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t uart_port_id_t;

/** Версия программного модуля uart_manager (семантика: major.minor.patch). */
#define UART_MANAGER_MODULE_VERSION "0.3.0"

/**
 * Зарезервировано под много портов (ТЗ изделия). На этапе Frontier_1 фактически используется только
 * порт с индексом UART_MGR_F1_PORT_ID; для остальных индексов API с esp_err_t возвращает ESP_ERR_NOT_SUPPORTED,
 * send/receive/wait_event — 0 без передачи данных.
 */
#define UART_MGR_NUM_PORTS 16
/** Единственный поддерживаемый на Frontier_1 логический порт. */
#define UART_MGR_F1_PORT_ID ((uart_port_id_t)0)

#define UART_MGR_BAUD_2400 2400u
#define UART_MGR_BAUD_4800 4800u
#define UART_MGR_BAUD_9600 9600u
#define UART_MGR_BAUD_19200 19200u
#define UART_MGR_BAUD_38400 38400u
#define UART_MGR_BAUD_57600 57600u
#define UART_MGR_BAUD_115200 115200u
#define UART_MGR_DEFAULT_BAUD UART_MGR_BAUD_115200

/** Frontier_1: размер выделяемого при старте пула под каждое кольцо RX/TX (логическая глубина ≤ этого значения). */
#define UART_MGR_F1_RING_POOL_BYTES (64u * 1024u)

typedef enum {
    UART_MGR_LINE_RS232 = 0,
    UART_MGR_LINE_RS485 = 1,
    UART_MGR_LINE_RS422 = 2,
} uart_mgr_line_mode_t;

typedef enum {
    UART_MGR_PARITY_NONE = 0,
    UART_MGR_PARITY_ODD = 1,
    UART_MGR_PARITY_EVEN = 2,
} uart_mgr_parity_t;

typedef enum {
    UART_MGR_OVERFLOW_DISCARD_OLD = 0,
    UART_MGR_OVERFLOW_STOP_RX = 1,
} uart_mgr_overflow_policy_t;

typedef struct {
    uint32_t baud;
    uint8_t data_bits;
    uint8_t stop_bits;
    uart_mgr_parity_t parity;
    uart_mgr_line_mode_t line_mode;
    /** Логическая глубина RX внутри пула (Frontier_1: пул UART_MGR_F1_RING_POOL_BYTES). */
    uint32_t ring_rx_size;
    /** Логическая глубина TX внутри пула (Frontier_1: пул UART_MGR_F1_RING_POOL_BYTES). */
    uint32_t ring_tx_size;
    uart_mgr_overflow_policy_t overflow;
} uart_manager_port_config_t;

typedef struct {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overflows;
    uint32_t framing_errors;
} uart_manager_stats_t;

typedef enum {
    UART_MGR_EVT_RX_READY = 1 << 0,
    /** Фронт: бит выставляется при новом переполнении (рост счётчика rx_overflows или событие UART_BUFFER_FULL). */
    UART_MGR_EVT_RX_OVERFLOW = 1 << 1,
    UART_MGR_EVT_LINE_ERR = 1 << 2,
} uart_mgr_event_mask_t;

/**
 * Инициализация модуля uart_manager (один раз за время работы прошивки).
 *
 * Отвечает за: выделение пулов RX/TX по UART_MGR_F1_RING_POOL_BYTES, создание мьютексов/семафоров/группы событий,
 * установку драйвера UART на GPIO из Kconfig (Frontier_1), запуск задачи uart_mgr (чтение из UART в кольцо RX,
 * выгрузка кольца TX в UART). Вызов `uart_manager_load_config_from_nvs()` выполняется внутри при старте,
 * чтобы восстановить параметры линии и логические глубины из NVS; при отсутствии или ошибке — значения по умолчанию.
 *
 * Параметры: нет.
 * Возврат: ESP_OK при успехе; ESP_ERR_NO_MEM при нехватке RAM; коды драйвера UART/NVS при сбоях.
 *
 * Порядок вызова в прошивке (рекомендуется):
 *   1) `network_config_init()` или иной код, выполняющий `nvs_flash_init()`;
 *   2) `uart_manager_init()`.
 *
 * Пример:
 *   ESP_ERROR_CHECK(network_config_init());
 *   ESP_ERROR_CHECK(uart_manager_init());
 */
esp_err_t uart_manager_init(void);

/**
 * Остановка модуля: мягкая остановка задачи uart_mgr (флаг run + ожидание до 5 с), затем удаление драйвера UART,
 * освобождение пулов и объектов синхронизации. Не вызывать из контекста, удерживающего внутренний мьютекс порта
 * (например, не держать блокировку во время долгого вызова другого API и параллельно deinit).
 * Внешние задачи после deinit не должны вызывать send/receive до повторного `uart_manager_init()`.
 */
void uart_manager_deinit(void);

/**
 * Применить конфигурацию порта (скорость, кадр, режим линии, логические размеры колец, политика переполнения).
 * На Frontier_1 поддерживается только порт UART_MGR_F1_PORT_ID; для остальных индексов — ESP_ERR_NOT_SUPPORTED
 * (однократное предупреждение в лог). Логические `ring_rx_size` / `ring_tx_size` ограничиваются сверху
 * UART_MGR_F1_RING_POOL_BYTES и снизу (минимум 2). Обновление RAM-конфигурации и `uart_param_config` выполняется
 * под одним мьютексом (нет рассинхрона «конфиг в памяти vs регистры UART» между двумя configure).
 * После применения кольца сбрасываются (данные в программных буферах теряются).
 *
 * Параметры:
 *   `port` — индекс порта (0 для Frontier_1);
 *   `cfg` — указатель на заполненную структуру (не NULL).
 * Возврат: ESP_OK; ESP_ERR_INVALID_ARG (cfg == NULL); ESP_ERR_NOT_SUPPORTED (порт ≠ F1); коды ESP-IDF при ошибке UART.
 *
 * Типичная последовательность: заполнить `uart_manager_port_config_t` → `uart_manager_configure_port(0, &cfg)` → при необходимости `uart_manager_save_config_to_nvs()`.
 */
esp_err_t uart_manager_configure_port(uart_port_id_t port, const uart_manager_port_config_t *cfg);

/**
 * Текущая конфигурация линии UART в памяти uart_manager (после init / configure / NVS).
 * На Frontier_1 — только порт UART_MGR_F1_PORT_ID.
 */
esp_err_t uart_manager_get_port_config(uart_port_id_t port, uart_manager_port_config_t *out_cfg);

/**
 * Сменить только режим линии (RS-232 / RS-485 / RS-422) в конфигурации порта.
 * На Frontier_1 без внешнего мультиплексора значение сохраняется в структуре; аппаратное переключение линий не выполняется.
 *
 * Параметры: `port` — 0 для Frontier_1; `mode` — режим из `uart_mgr_line_mode_t`.
 * Возврат: ESP_OK; ESP_ERR_NOT_SUPPORTED (порт ≠ F1); ESP_ERR_INVALID_ARG (неверный mode).
 */
esp_err_t uart_manager_set_line_mode(uart_port_id_t port, uart_mgr_line_mode_t mode);

/**
 * Передать байты в линию UART через программное кольцо TX.
 * Блокирует вызывающую задачу не дольше `ticks_to_wait` тиков FreeRTOS на ожидание места в кольце (если оно заполнено).
 * При `ticks_to_wait == 0` не ждёт: возвращает сразу столько байт, сколько удалось положить.
 * Возвращаемое значение — число принятых модулем байт (может быть меньше `len`). Неверный индекс порта при
 * инициализированном модуле: 0 и однократный WARN в лог (см. UART_MGR_F1_PORT_ID).
 *
 * Параметры:
 *   `port` — 0 для Frontier_1;
 *   `data` — буфер источника;
 *   `len` — число байт для передачи;
 *   `ticks_to_wait` — таймаут ожидания места в TX-кольце (0 = без ожидания).
 *
 * Пример (отправить блок целиком с ожиданием):
 *   size_t off = 0;
 *   while (off < n) {
 *       off += uart_manager_send(0, buf + off, n - off, pdMS_TO_TICKS(1000));
 *   }
 */
size_t uart_manager_send(uart_port_id_t port, const void *data, size_t len, TickType_t ticks_to_wait);

/**
 * Прочитать байты из программного кольца RX (данные попадают туда из задачи uart_mgr с линии RX).
 * Блокирует не дольше `ticks_to_wait`, пока не появятся данные, если кольцо было пусто.
 * При `ticks_to_wait == 0` сразу возвращает 0, если приёма нет.
 * Возврат — число байт, скопированных в `buf` (не больше `len`). Неверный порт: 0 и WARN в лог (при s_inited).
 *
 * Параметры:
 *   `port` — 0;
 *   `buf` — буфер назначения;
 *   `len` — размер буфера;
 *   `ticks_to_wait` — таймаут ожидания данных.
 *
 * Пример (чтение порции в цикле эхо-задачи):
 *   size_t n = uart_manager_receive(0, tmp, sizeof(tmp), portMAX_DELAY);
 */
size_t uart_manager_receive(uart_port_id_t port, void *buf, size_t len, TickType_t ticks_to_wait);

/**
 * Ожидать одно или несколько событий по маске `bits` (UART_MGR_EVT_*).
 * `clear_on_exit` — сбрасывать ли соответствующие биты в группе после успешного ожидания.
 * Возврат — биты событий, которые были установлены до выхода (по смыслу xEventGroupWaitBits). Неверный порт: 0.
 *
 * Параметры:
 *   `port` — 0;
 *   `bits` — маска ожидаемых событий;
 *   `clear_on_exit` — true, чтобы очистить сработавшие биты;
 *   `ticks_to_wait` — таймаут.
 *
 * Типично используется вместе с `uart_manager_receive`: сначала дождаться UART_MGR_EVT_RX_READY, затем читать.
 */
EventBits_t uart_manager_wait_event(uart_port_id_t port, EventBits_t bits, bool clear_on_exit,
                                    TickType_t ticks_to_wait);

/**
 * Считать накопленную статистику порта (принято/передано байт, переполнения RX, ошибки кадра).
 * Параметры: `port` — UART_MGR_F1_PORT_ID; `out_stats` — не NULL. Неверный порт: ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t uart_manager_get_stats(uart_port_id_t port, uart_manager_stats_t *out_stats);

/** Обнулить статистику порта. Неверный порт: ESP_ERR_NOT_SUPPORTED. */
esp_err_t uart_manager_reset_stats(uart_port_id_t port);

/**
 * Сохранить текущую конфигурацию порта 0 в NVS (пространство имён и ключ задаются реализацией модуля).
 * Вызывать после изменения настроек, если их нужно восстановить после перезагрузки.
 * Требует ранее выполненного `nvs_flash_init()` (как правило через `network_config_init()`).
 *
 * Пример: `uart_manager_configure_port(0, &cfg); ESP_ERROR_CHECK(uart_manager_save_config_to_nvs());`
 */
esp_err_t uart_manager_save_config_to_nvs(void);

/**
 * Загрузить конфигурацию порта 0 из NVS и применить её (если модуль уже инициализирован — через внутренний configure).
 * Если записи нет, неверный размер блоба, CRC (формат v2) не сходится или поля невалидны — ESP_OK и дефолты (с логом).
 * Поддерживаются запись v1 (28 B полезной нагрузки без CRC) и v2 (32 B: полезная нагрузка + CRC32 по 28 байтам).
 */
esp_err_t uart_manager_load_config_from_nvs(void);

/**
* Проверить валидность baud rate. На Frontier_1 поддерживаются только 2400, 4800, 9600, 19200, 38400, 57600, 115200.
*/
esp_err_t uart_manager_validate_baud(uint32_t baud);

/**
 * Проверить валидность бит данных. На Frontier_1 поддерживается 5, 6, 7, 8 бит данных.
 */
esp_err_t uart_manager_validate_data_bits(uint8_t data_bits);



#if defined(CONFIG_UART_MGR_ENABLE_TEST_TASK) && CONFIG_UART_MGR_ENABLE_TEST_TASK
/**
 * Тестовая задача замыкания TX–RX: раз в секунду шлёт на TX строку вида `LBK <n>\r\n`, в консоль (ESP_LOGI, hexdump) выводит
 * каждый принятый и каждый отправляемый блок. Для проверки физики соедините TX и RX.
 * Создаётся из `app_main` с приоритетом ниже задачи uart_mgr (см. Kconfig).
 *
 * Параметр FreeRTOS `arg` не используется.
 *
 * Пример создания:
 *   xTaskCreate(uart_mgr_consumer_test_task, "uart_echo", 4096, NULL, CONFIG_UART_MGR_TASK_PRIORITY - 1, NULL);
 */
void uart_mgr_consumer_test_task(void *arg);
#endif

#ifdef __cplusplus
}
#endif
