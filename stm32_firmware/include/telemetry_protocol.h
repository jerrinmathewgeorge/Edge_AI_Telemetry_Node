#ifndef TELEMETRY_PROTOCOL_H
#define TELEMETRY_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inter-MCU UART & memory budget bounds (see root README.md). */
#define TELEMETRY_UART_BAUD_RATE        115200U
#define TELEMETRY_MAX_MODEL_RAM_BYTES   2048U
#define TELEMETRY_MAX_MODEL_FLASH_BYTES 15360U

/* 7-bit I2C addresses for STM32-local environmental payload. */
#define SENSOR_ADDR_SHT45               0x44U
#define SENSOR_ADDR_SGP41               0x59U

/**
 * @brief Abstracted 1-byte state flags sent across the STM32 -> ESP32 UART air-gap.
 *
 * Raw environmental samples remain inside STM32 SRAM; only these 1-byte codes
 * cross UART4 to the ESP32 MQTT bridge and TimescaleDB backend.
 */
typedef enum {
    TELEMETRY_STATE_NORMAL       = 0x00U,
    TELEMETRY_STATE_ANOMALY      = 0x01U,
    TELEMETRY_STATE_CALIBRATING  = 0x02U,
    TELEMETRY_STATE_SENSOR_FAULT = 0xFFU
} telemetry_state_t;

/**
 * @brief Local-only environmental sample retained strictly in STM32 SRAM.
 */
typedef struct {
    float temp_c;
    float rel_humidity_pct;
    uint16_t sgp41_voc_raw;
    uint16_t sgp41_nox_raw;
} env_sample_t;

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_PROTOCOL_H */
