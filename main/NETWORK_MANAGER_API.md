# API модуля `network_manager`

Задача FreeRTOS **`network_mgr`**: TCP/UDP-сокеты (lwIP), прозрачный мост с **`uart_manager`**, очередь команд, NVS (`net_mgr`), реакция на `IP_EVENT_ETH_GOT_IP` / `ETH_LOST_IP`.

Версия модуля: макрос `NETWORK_MANAGER_MODULE_VERSION` в `network_manager.h`.

## Порядок инициализации

1. `network_config_init()` (NVS).
2. `uart_manager_init()`.
3. `network_manager_init()` — очередь, mutex статистики, регистрация IP-событий, старт задачи `network_mgr`.
4. После Ethernet и успешной аутентификации EAP-TLS, когда есть IP: `network_config_apply_saved()`, затем **`network_manager_start()`** (ставит в очередь `NM_CMD_LOAD_AND_APPLY_ALL`: чтение NVS `net_mgr` и открытие сокетов).

Не вызывать публичные функции модуля из ISR.

## Число физических COM

- Kconfig: `CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS` (1…16). Frontier_1: **1**.
- `network_manager_get_physical_com_count()` — то же значение.
- Команды с `port_id` вне диапазона `[0, physical_com_count)` отклоняются.

## Очередь команд

Единая точка постановки: **`network_manager_post_command(const nm_command_t *cmd, TickType_t ticks_to_wait)`** — копия структуры в очередь (`CONFIG_NETWORK_MGR_CMD_QUEUE_LEN`).

Типы (`nm_cmd_type_t`):

| Команда | Назначение |
|---------|------------|
| `NM_CMD_APPLY_CONFIG` | Поля `port_id`, `net_cfg`, опционально `apply_uart_line` + `uart_cfg`. Сначала UART через `uart_manager_configure_port` + `uart_manager_save_config_to_nvs` (только порт `UART_MGR_F1_PORT_ID` при `apply_uart_line`), затем пересоздание сокетов порта и **`nm_save_nvs`**. |
| `NM_CMD_STOP_PORT` | Закрыть сокеты порта, режим OFF, сохранить NVS. |
| `NM_CMD_SHUTDOWN` | Все порты OFF, закрыть сокеты, NVS. |
| `NM_CMD_LOAD_AND_APPLY_ALL` | Загрузить блоб из NVS, переоткрыть все порты. |
| `NM_CMD_REAPPLY_RUNNING` | Без чтения NVS: закрыть и открыть сокеты по RAM-конфигу (после ETH_GOT_IP). |
| `NM_CMD_LINK_DOWN` | Только закрыть сокеты (ETH_LOST_IP), конфиг и NVS не менять. |

## NVS

- Namespace: **`net_mgr`**, ключ: **`cfg_v1`**.
- Блоб: магия `0x4e4d4731`, версия структуры 1, до **16** записей `nm_stored_net_t`; активны первые `CONFIG_NETWORK_MGR_PHYSICAL_COM_PORTS` слотов.
- Если магия/версия неверны или блоб отсутствует — дефолт **OFF** для всех портов (лог WARN).
- Если в сохранённом блобе `physical_ports` ≠ текущему Kconfig — предупреждение в лог, применяется пересечение индексов.

Параметры линии UART хранятся в NVS модуля **`uart_manager`** (не дублируются в `net_mgr`).

## Статистика

`network_manager_get_stats(port_id, &nm_port_stats_t)` — mutex, не из ISR.

## TCP/UDP поведение (кратко)

- **TCP server**: до **5** клиентов; `TCP_NODELAY`; keep-alive при поддержке `TCP_KEEPIDLE`; данные с UART дублируются **всем** подключённым клиентам.
- **TCP client**: переподключение с периодом `CONFIG_NETWORK_MGR_TCP_RECONNECT_MS`.
- **UDP**: `bind` на `local_tcp_udp_port`, обмен с `remote_ip` / `remote_port`.

Транспорт: **lwIP** через POSIX-сокеты ESP-IDF, интерфейс Ethernet — `esp_netif`.

## Тестовая задача (`nm_test`)

Включается **`CONFIG_NETWORK_MGR_ENABLE_TEST_TASK=y`**. Сценарий и адреса задаются в **[`network_manager_test_scenarios.h`](network_manager_test_scenarios.h)** (ровно **один** активный макрос `NM_TEST_SCENARIO_*`, иначе ошибка препроцессора).

| ID | Макрос | Поведение | Проверка с ПК |
|----|--------|-----------|----------------|
| S1 | `NM_TEST_SCENARIO_TCP_SERVER` | `NM_CMD_APPLY_CONFIG`: TCP server на `NM_TEST_LOCAL_PORT` | Один клиент: `nc`/PuTTY на IP устройства |
| S2 | `NM_TEST_SCENARIO_TCP_SERVER_5CLIENTS_MANUAL` | То же, что S1 (режим в прошивке один); в логе — напоминание про **до 5** сессий по ТЗ | Вручную 5 TCP-сессий к тому же порту |
| S3 | `NM_TEST_SCENARIO_TCP_CLIENT` | TCP client к `NM_TEST_REMOTE_IP`:`NM_TEST_REMOTE_PORT` | На ПК слушатель, например `nc -l -p NM_TEST_REMOTE_PORT` |
| S4 | `NM_TEST_SCENARIO_UDP` | UDP: bind `NM_TEST_LOCAL_PORT`, обмен с `NM_TEST_REMOTE_IP`:`NM_TEST_REMOTE_PORT` | `nc -u` или Python UDP |
| S5 | `NM_TEST_SCENARIO_TCP_THEN_STOP` | TCP server как S1, пауза `NM_TEST_THEN_STOP_DELAY_MS`, затем `NM_CMD_STOP_PORT` | Проверка снятия моста |

Параметры задержки старта, портов и IP — макросы в том же заголовке (`NM_TEST_START_DELAY_MS`, `NM_TEST_LOCAL_PORT`, `NM_TEST_REMOTE_IP`, `NM_TEST_REMOTE_PORT`, …). Порт COM — всегда `0` (F1). UART через тест не меняется (`apply_uart_line = false`).

**S6 (NVS после reboot):** автоматически не реализован; проверяйте вручную после записи конфигурации.

**Конфликт с `uart_echo`:** при проверке моста отключите `CONFIG_UART_MGR_ENABLE_TEST_TASK` или учитывайте разделение RX.

## Отладочные логи

`CONFIG_NETWORK_MGR_VERBOSE_LOG=y` — в `main.c` для тега `network_mgr` выставляется уровень DEBUG.
