#ifndef SENSIRION_CODEC_H
#define SENSIRION_CODEC_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edge_ai {

/**
 * @brief Hardware-independent Sensirion I2C frame codec (SHT45 + SGP41).
 *
 * Implements Sensirion CRC-8 (poly 0x31, init 0xFF), SHT45 Temp/RH decoding,
 * and SGP41 humidity/temperature-compensated command encoding per datasheet.
 * Zero heap allocation; safe for bare-metal STM32 DMA buffers and PC tests.
 */
namespace sensirion {

inline constexpr std::uint8_t SHT45_CMD_MEASURE_HIGH_PRECISION = 0xFDU;
inline constexpr std::array<std::uint8_t, 2> SGP41_CMD_HEATER_OFF = {0x36U, 0x15U};
inline constexpr std::array<std::uint8_t, 2> SGP41_CMD_SELF_TEST  = {0x28U, 0x0EU};

inline constexpr std::array<std::uint8_t, 8> SGP41_DEFAULT_CONDITIONING_CMD = {
    0x26U, 0x12U, 0x80U, 0x00U, 0xA2U, 0x66U, 0x66U, 0x93U
};

inline constexpr std::array<std::uint8_t, 8> SGP41_DEFAULT_MEASURE_CMD = {
    0x26U, 0x19U, 0x80U, 0x00U, 0xA2U, 0x66U, 0x66U, 0x93U
};

/**
 * @brief Compute Sensirion CRC-8 (polynomial 0x31, init 0xFF, no final XOR).
 */
constexpr std::uint8_t crc8(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint8_t crc = 0xFFU;
    for (std::size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x80U) != 0U) {
                crc = static_cast<std::uint8_t>((crc << 1U) ^ 0x31U);
            } else {
                crc = static_cast<std::uint8_t>(crc << 1U);
            }
        }
    }
    return crc;
}

constexpr std::uint16_t unpack_u16_be(std::uint8_t msb, std::uint8_t lsb) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(msb) << 8U) | lsb);
}

inline float clampf(float value, float min_val, float max_val) noexcept {
    if (!std::isfinite(value)) {
        return min_val;
    }
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

/**
 * @brief Convert relative humidity (%) to SGP41 protocol ticks (Datasheet Table 12).
 */
inline std::uint16_t encode_rh_ticks(float rel_humidity_pct) noexcept {
    const float clamped = clampf(rel_humidity_pct, 0.0f, 100.0f);
    return static_cast<std::uint16_t>(std::lround((clamped * 65535.0f) / 100.0f));
}

/**
 * @brief Convert temperature (deg C) to SGP41 protocol ticks (Datasheet Table 12).
 */
inline std::uint16_t encode_temp_ticks(float temp_c) noexcept {
    const float clamped = clampf(temp_c, -45.0f, 130.0f);
    return static_cast<std::uint16_t>(std::lround(((clamped + 45.0f) * 65535.0f) / 175.0f));
}

/**
 * @brief Build the 8-byte SGP41 measure_raw_signals command with SHT45 RH/T compensation.
 */
inline std::array<std::uint8_t, 8> build_sgp41_measure_cmd(float rel_humidity_pct,
                                                           float temp_c) noexcept {
    const std::uint16_t rh_ticks = encode_rh_ticks(rel_humidity_pct);
    const std::uint16_t t_ticks = encode_temp_ticks(temp_c);

    std::array<std::uint8_t, 8> cmd = {
        0x26U,
        0x19U,
        static_cast<std::uint8_t>((rh_ticks >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(rh_ticks & 0xFFU),
        0x00U,
        static_cast<std::uint8_t>((t_ticks >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(t_ticks & 0xFFU),
        0x00U
    };
    cmd[4] = crc8(&cmd[2], 2U);
    cmd[7] = crc8(&cmd[5], 2U);
    return cmd;
}

/**
 * @brief Build the 8-byte SGP41 execute_conditioning command with RH/T compensation.
 */
inline std::array<std::uint8_t, 8> build_sgp41_conditioning_cmd(float rel_humidity_pct,
                                                                float temp_c) noexcept {
    std::array<std::uint8_t, 8> cmd = build_sgp41_measure_cmd(rel_humidity_pct, temp_c);
    cmd[1] = 0x12U;
    return cmd;
}

/**
 * @brief Decode a 6-byte SHT45 response frame after verifying both CRC-8 bytes.
 * @return true if both CRCs pass; false leaves output parameters untouched.
 */
inline bool decode_sht45_frame(const std::uint8_t rx[6],
                               float& out_temp_c,
                               float& out_rel_humidity_pct) noexcept {
    if (rx == nullptr) {
        return false;
    }
    if (crc8(&rx[0], 2U) != rx[2] || crc8(&rx[3], 2U) != rx[5]) {
        return false;
    }

    const std::uint16_t raw_t = unpack_u16_be(rx[0], rx[1]);
    const std::uint16_t raw_rh = unpack_u16_be(rx[3], rx[4]);

    out_temp_c = -45.0f + (175.0f * static_cast<float>(raw_t) / 65535.0f);
    out_rel_humidity_pct = clampf(-6.0f + (125.0f * static_cast<float>(raw_rh) / 65535.0f),
                                  0.0f,
                                  100.0f);
    return true;
}

/**
 * @brief Decode a 3-byte SGP41 conditioning response (SRAW_VOC + CRC).
 */
inline bool decode_sgp41_conditioning(const std::uint8_t rx[3],
                                      std::uint16_t& out_voc_raw) noexcept {
    if (rx == nullptr || crc8(&rx[0], 2U) != rx[2]) {
        return false;
    }
    out_voc_raw = unpack_u16_be(rx[0], rx[1]);
    return true;
}

/**
 * @brief Decode a 6-byte SGP41 measure_raw_signals response (VOC + NOx + CRCs).
 */
inline bool decode_sgp41_frame(const std::uint8_t rx[6],
                               std::uint16_t& out_voc_raw,
                               std::uint16_t& out_nox_raw) noexcept {
    if (rx == nullptr) {
        return false;
    }
    if (crc8(&rx[0], 2U) != rx[2] || crc8(&rx[3], 2U) != rx[5]) {
        return false;
    }

    out_voc_raw = unpack_u16_be(rx[0], rx[1]);
    out_nox_raw = unpack_u16_be(rx[3], rx[4]);
    return true;
}

}  // namespace sensirion
}  // namespace edge_ai

#endif /* SENSIRION_CODEC_H */
