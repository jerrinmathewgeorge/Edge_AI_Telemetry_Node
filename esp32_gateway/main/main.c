#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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

#define UART_PORT_NUM       UART_NUM_2
#define UART_BUF_SIZE       256
#define FRAME_SYNC_BYTE     0xAAU
#define FRAME_SIZE_BYTES    4U

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;

static uint8_t compute_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((crc & 0x80U) != 0U) {
                crc = (uint8_t)((crc << 1U) ^ 0x31U);
            } else {
                crc = (uint8_t)(crc << 1U);
            }
        }
    }
    return crc;
}

static bool is_valid_state(uint8_t state)
{
    return (state == 0x00U) || (state == 0x01U) || (state == 0x02U) || (state == 0xFFU);
}

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

static size_t resync_frame_buffer(uint8_t frame[FRAME_SIZE_BYTES])
{
    for (size_t i = 1; i < FRAME_SIZE_BYTES; ++i) {
        if (frame[i] == FRAME_SYNC_BYTE) {
            const size_t remaining = FRAME_SIZE_BYTES - i;
            memmove(frame, &frame[i], remaining);
            return remaining;
        }
    }
    return 0;
}

static void publish_validated_frame(const uint8_t frame[FRAME_SIZE_BYTES])
{
    ESP_LOGI(TAG, "UART Frame OK: seq=%u state=0x%02X", frame[1], frame[2]);

    if (s_mqtt_client != NULL && s_mqtt_connected) {
        int msg_id = esp_mqtt_client_publish(
            s_mqtt_client,
            CONFIG_EDGE_AI_MQTT_TOPIC,
            (const char *)frame,
            FRAME_SIZE_BYTES,
            0,
            0
        );
        ESP_LOGI(TAG, "MQTT TX [%s] seq=%u msg_id=%d", CONFIG_EDGE_AI_MQTT_TOPIC, frame[1], msg_id);
    } else {
        ESP_LOGW(TAG, "MQTT offline; dropped seq=%u state=0x%02X", frame[1], frame[2]);
    }
}

static void uart_to_mqtt_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t rx_buffer[UART_BUF_SIZE];
    uint8_t frame[FRAME_SIZE_BYTES];
    size_t frame_idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        for (int i = 0; i < len; ++i) {
            const uint8_t byte = rx_buffer[i];
            if (frame_idx == 0 && byte != FRAME_SYNC_BYTE) {
                continue;
            }
            frame[frame_idx++] = byte;

            if (frame_idx == FRAME_SIZE_BYTES) {
                const bool crc_ok = (compute_crc8(frame, 3U) == frame[3]);
                if (crc_ok && is_valid_state(frame[2])) {
                    publish_validated_frame(frame);
                    frame_idx = 0;
                } else {
                    ESP_LOGW(TAG, "Rejected UART frame (crc_ok=%d state=0x%02X); resyncing",
                             crc_ok ? 1 : 0, frame[2]);
                    frame_idx = resync_frame_buffer(frame);
                }
            }
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
