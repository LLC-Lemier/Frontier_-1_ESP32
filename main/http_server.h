#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"

static const char *HTTP_TAG = "HTTP_TASK";


static httpd_handle_t server_handle = NULL; // глобальный обработчик


static esp_err_t root_handler(httpd_req_t *req)
{
    const char* resp = "<html><body><h1>Сервер работает в отдельной таске!</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* Обработчик GET "/status" */
static esp_err_t status_handler(httpd_req_t *req)
{
    const char* resp = "{\"server\": \"running\", \"task\": \"http_server_task\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}


static void register_uri_handlers(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t status = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &status);
}

void http_server_task(void *pvParameters)
{
    ESP_LOGI(HTTP_TAG, "HTTP серверная задача запущена на ядре %d", xPortGetCoreID());

    // Настройка сервера
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.task_priority = tskIDLE_PRIORITY + 5;
    config.stack_size = 8192;  // Можно увеличить под свои нужды
    
    // Запуск сервера
    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(HTTP_TAG, "Ошибка запуска HTTP сервера: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    // Регистрация обработчиков
    register_uri_handlers(server_handle);
    ESP_LOGI(HTTP_TAG, "HTTP сервер успешно запущен на порту %d", config.server_port);

    // ГЛАВНЫЙ ЦИКЛ ЗАДАЧИ
    // Сервер работает в фоновых задачах, наша задача просто ждёт команды на остановку
    while (1) {
        // Здесь можно делать что-то полезное:
        // - мониторить состояние сервера
        // - обновлять статистику
        // - ждать команду на остановку через очередь или флаг
        
        vTaskDelay(pdMS_TO_TICKS(1000));  // Проверяем каждую секунду
        
        // Пример: проверка глобального флага остановки
        // if (g_stop_server_flag) break;
    }

    // Остановка сервера при выходе из цикла
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = NULL;
        ESP_LOGI(HTTP_TAG, "HTTP сервер остановлен");
    }

    ESP_LOGI(HTTP_TAG, "HTTP серверная задача завершается");
    vTaskDelete(NULL);
}

/* Функция для остановки сервера извне */
void stop_http_server(void)
{
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = NULL;
        ESP_LOGI(HTTP_TAG, "HTTP сервер остановлен по внешней команде");
    }
}

/* Запуск сервера в отдельной задаче */
void start_http_server_task(void)
{
    // Создаём задачу с приоритетом 5 и стеком 8KB
    xTaskCreatePinnedToCore(
        http_server_task,          // Функция задачи
        "http_server",             // Имя задачи
        8192,                      // Размер стека (байт)
        NULL,                      // Параметры
        5,                         // Приоритет (выше, чем у main)
        NULL,                      // Хендл задачи (можно сохранить)
        tskNO_AFFINITY             // Ядро (любое)
    );
}
