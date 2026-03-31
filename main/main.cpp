// dev branch *dev*

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/uart.h"
#include "esp_log.h"

static const char* TAG = "DEMO";

QueueHandle_t json_queue;   // аналог mutex

// uart init
void uart_init() {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);

    // tx = GPIO2, rx = GPIO3
    uart_set_pin(UART_NUM_1, 2, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

typedef struct {
    char json[64];
} JsonMessage;

// parser

int extract_second_value(const char* json) {
    int a, b;
    sscanf(json, "{\"a\":%d,\"b\":%d}", &a, &b);
    return b;
}

// task1

void json_producer_task(void* arg) {
    JsonMessage msg;

    while (true) {
        int a = rand() % 100;
        int b = rand() % 100;

        snprintf(msg.json, sizeof(msg.json), "{\"a\":%d,\"b\":%d}", a, b);

        ESP_LOGI(TAG, "Generated: %s", msg.json);

        xQueueSend(json_queue, &msg, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// task 2 

void uart_consumer_task(void* arg) {
    JsonMessage msg;

    while (true) {
        if (xQueueReceive(json_queue, &msg, portMAX_DELAY)) {

            ESP_LOGI(TAG, "Received: %s", msg.json);

            int value = extract_second_value(msg.json);

            char out[32];
            snprintf(out, sizeof(out), "%d\n", value);

            uart_write_bytes(UART_NUM_1, out, strlen(out));

            ESP_LOGI(TAG, "UART sent: %d", value);
        }
    }
}

// 

extern "C" void app_main(void) {

    uart_init();

    json_queue = xQueueCreate(5, sizeof(JsonMessage));

    xTaskCreate(json_producer_task, "producer", 4096, NULL, 5, NULL);
    xTaskCreate(uart_consumer_task, "consumer", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System started");
}

/*

#include "esp_log.h"
static const char* TAG = "MAIN";

auto router = std::make_shared<Router>();  // умный указатель на Router
auto authHandler = std::make_shared<AuthHandler>();  // умный укказатель на AutHandler


router->addRoute("POST", "/api/auth/login", [authHandler](const Request& req, Response& res)
{
     authHandler->handleLogin(req, res);
     ESP_LOGI(TAG, "lambda worked!!!"); 
});

static const char* TAG = "MAIN";

int sum1(int a, int b){
 return a + b;
}

auto suml = [TAG](int a, int b) {
   std::cout << TAG << std::endl;
   return a + b;  
};


int x = 10;

auto f = []() {
    std::cout << "Testing message" << std::endl;  // работает но тянет лишние зависимости
    ESP_LOGI("MAIN", "System started");  // метод из "esp_log.h"
}

auto z = [&x]() {

}

router->addRoute("POST", "/api/auth/login",
    [authHandler](const Request& req, Response& res) {
        authHandler->handleLogin(req, res);
    });


ESP_LOGI(TAG, "Message");
*/










