#ifndef TELEMETRY_PROTOCOL_H
#define TELEMETRY_PROTOCOL_H

#include <stddef.h>
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

/* Deterministic 4-byte UART frame constants: [SYNC 0xAA][SEQ][STATE][CRC8]. */
#define TELEMETRY_FRAME_SYNC_BYTE       0xAAU
#define TELEMETRY_FRAME_SIZE_BYTES      4U

/**
 * @brief Abstracted state flags carried inside the 4-byte UART air-gap frame.
 *
 * Raw environmental samples remain inside STM32 SRAM; only these state codes
 * wrapped in [0xAA][SEQ_ID][STATE][CRC8] cross UART4 to the ESP32 bridge.
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

/**
 * @brief Compute CRC-8 (poly 0x31, init 0xFF, no final XOR) matching Sensirion.
 */
static inline uint8_t telemetry_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFFU;
    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x80U) != 0U) {
                crc = (uint8_t)((crc << 1U) ^ 0x31U);
            } else {
                crc = (uint8_t)(crc << 1U);
            }
        }
    }
    return crc;
}

static inline int telemetry_is_valid_state(uint8_t raw_state) {
    return (raw_state == (uint8_t)TELEMETRY_STATE_NORMAL) ||
           (raw_state == (uint8_t)TELEMETRY_STATE_ANOMALY) ||
           (raw_state == (uint8_t)TELEMETRY_STATE_CALIBRATING) ||
           (raw_state == (uint8_t)TELEMETRY_STATE_SENSOR_FAULT);
}

/**
 * @brief Pack a 4-byte air-gapped telemetry frame [0xAA][SEQ][STATE][CRC8].
 */
static inline void telemetry_pack_frame(uint8_t seq_id,
                                        telemetry_state_t state,
                                        uint8_t out_frame[TELEMETRY_FRAME_SIZE_BYTES]) {
    out_frame[0] = TELEMETRY_FRAME_SYNC_BYTE;
    out_frame[1] = seq_id;
    out_frame[2] = (uint8_t)state;
    out_frame[3] = telemetry_crc8(out_frame, 3U);
}

/**
 * @brief Validate and unpack a 4-byte telemetry frame.
 * @return 1 if sync, CRC-8, and state code are valid; 0 otherwise.
 */
static inline int telemetry_unpack_frame(const uint8_t frame[TELEMETRY_FRAME_SIZE_BYTES],
                                         uint8_t* out_seq_id,
                                         telemetry_state_t* out_state) {
    if (frame == NULL || out_seq_id == NULL || out_state == NULL) {
        return 0;
    }
    if (frame[0] != TELEMETRY_FRAME_SYNC_BYTE) {
        return 0;
    }
    if (telemetry_crc8(frame, 3U) != frame[3]) {
        return 0;
    }
    if (!telemetry_is_valid_state(frame[2])) {
        return 0;
    }
    *out_seq_id = frame[1];
    *out_state = (telemetry_state_t)frame[2];
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_PROTOCOL_H */
