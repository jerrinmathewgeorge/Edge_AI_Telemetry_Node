#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/uart.h"

static const char *TAG = "EDGE_AI_GATEWAY";

#define UART_PORT_NUM UART_NUM_2
#define UART_BUF_SIZE 256

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT broker connected (%s)", CONFIG_EDGE_AI_MQTT_BROKER_URI);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT broker disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT transport error");
            break;
        default:
            break;
    }
}

static void init_uart(void)
{
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_EDGE_AI_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT_NUM,
        CONFIG_EDGE_AI_UART_TX_GPIO,
        CONFIG_EDGE_AI_UART_RX_GPIO,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
}

static void uart_to_mqtt_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t rx_buffer[UART_BUF_SIZE];

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        ESP_LOGI(TAG, "UART RX (%d B), head=0x%02X", len, rx_buffer[0]);

        if (s_mqtt_client != NULL && s_mqtt_connected) {
            int msg_id = esp_mqtt_client_publish(
                s_mqtt_client,
                CONFIG_EDGE_AI_MQTT_TOPIC,
                (const char *)rx_buffer,
                len,
                0,
                0
            );
            ESP_LOGI(TAG, "MQTT TX [%s] msg_id=%d", CONFIG_EDGE_AI_MQTT_TOPIC, msg_id);
        } else {
            ESP_LOGW(TAG, "MQTT offline; dropped %d B state payload", len);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_EDGE_AI_MQTT_BROKER_URI,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));

    init_uart();
    xTaskCreate(uart_to_mqtt_task, "uart_to_mqtt_task", 4096, NULL, 5, NULL);
}
