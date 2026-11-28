#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "ewma_filter.h"
#include "ring_buffer.h"
#include "sensirion_codec.h"
#include "telemetry_protocol.h"

namespace s = edge_ai::sensirion;

static_assert(sizeof(edge_ai::StaticRingBuffer<env_sample_t, 32>) < TELEMETRY_MAX_MODEL_RAM_BYTES,
              "Static ring buffer exceeds RAM budget");
static_assert(sizeof(edge_ai::EwmaBaselineFilter) <= 32U,
              "EWMA filter state must remain compact");

namespace {

telemetry_state_t classify_sample(edge_ai::EwmaBaselineFilter& ewma,
                                  float raw_voc,
                                  bool crc_ok,
                                  float residual_threshold) noexcept {
    if (!crc_ok) {
        return TELEMETRY_STATE_SENSOR_FAULT;
    }
    const float residual = ewma.update(raw_voc);
    if (!ewma.is_warmed_up()) {
        return TELEMETRY_STATE_CALIBRATING;
    }
    return (std::fabs(residual) > residual_threshold) ? TELEMETRY_STATE_ANOMALY
                                                      : TELEMETRY_STATE_NORMAL;
}

void test_sensirion_codec() {
    const std::uint8_t d1[2] = {0x80U, 0x00U};
    const std::uint8_t d2[2] = {0x66U, 0x66U};
    const std::uint8_t d3[2] = {0x00U, 0x00U};
    const std::uint8_t d4[2] = {0xFFU, 0xFFU};
    assert(s::crc8(d1, 2U) == 0xA2U);
    assert(s::crc8(d2, 2U) == 0x93U);
    assert(s::crc8(d3, 2U) == 0x81U);
    assert(s::crc8(d4, 2U) == 0xACU);

    assert(s::build_sgp41_measure_cmd(50.0f, 25.0f) == s::SGP41_DEFAULT_MEASURE_CMD);
    assert(s::build_sgp41_conditioning_cmd(50.0f, 25.0f) == s::SGP41_DEFAULT_CONDITIONING_CMD);

    std::uint8_t sht_rx[6] = {0x66U, 0x66U, 0x93U, 0x80U, 0x00U, 0xA2U};
    float temp_c = 0.0f;
    float rh_pct = 0.0f;
    assert(s::decode_sht45_frame(sht_rx, temp_c, rh_pct));
    assert(std::fabs(temp_c - 25.0f) < 0.01f);
    assert(rh_pct > 56.0f && rh_pct < 57.0f);

    sht_rx[5] ^= 0x01U;
    assert(!s::decode_sht45_frame(sht_rx, temp_c, rh_pct));

    std::uint8_t sgp_rx[6] = {0x75U, 0x30U, s::crc8(std::array<std::uint8_t, 2>{0x75U, 0x30U}.data(), 2U),
                              0x3AU, 0x98U, s::crc8(std::array<std::uint8_t, 2>{0x3AU, 0x98U}.data(), 2U)};
    std::uint16_t voc = 0U;
    std::uint16_t nox = 0U;
    assert(s::decode_sgp41_frame(sgp_rx, voc, nox));
    assert(voc == 30000U && nox == 15000U);

    sgp_rx[2] ^= 0x01U;
    assert(!s::decode_sgp41_frame(sgp_rx, voc, nox));
}

void test_static_ring_buffer() {
    edge_ai::StaticRingBuffer<env_sample_t, 4> rb;
    assert(rb.empty());
    assert(!rb.full());

    for (std::uint16_t i = 0U; i < 6U; ++i) {
        rb.push(env_sample_t{20.0f + static_cast<float>(i), 50.0f, static_cast<std::uint16_t>(30000U + i), 15000U});
    }

    assert(rb.full());
    assert(rb.size() == 4U);

    env_sample_t chronological[4]{};
    assert(rb.copy_chronological(chronological, 4U) == 4U);
    assert(chronological[0].sgp41_voc_raw == 30002U);
    assert(chronological[3].sgp41_voc_raw == 30005U);
}

void test_diurnal_drift_and_voc_spike() {
    edge_ai::StaticRingBuffer<env_sample_t, 16> rb;
    edge_ai::EwmaBaselineFilter ewma(0.05f, 20U);
    constexpr float kVocAnomalyThreshold = 450.0f;

    // 1. Warm-up + slow diurnal baseline drift (+4 ticks/sample = +480 ticks total)
    float raw_voc = 30000.0f;
    for (int step = 0; step < 120; ++step) {
        raw_voc += 4.0f;
        rb.push(env_sample_t{22.0f + 0.02f * static_cast<float>(step),
                             50.0f,
                             static_cast<std::uint16_t>(raw_voc),
                             15000U});
        const telemetry_state_t state = classify_sample(ewma, raw_voc, true, kVocAnomalyThreshold);
        if (step < 19) {
            assert(state == TELEMETRY_STATE_CALIBRATING);
        } else {
            assert(state == TELEMETRY_STATE_NORMAL);
        }
    }

    // 2. Sharp VOC exposure drops SRAW_VOC (per SGP41 datasheet sensitivity)
    const telemetry_state_t spike_state =
        classify_sample(ewma, raw_voc - 1200.0f, true, kVocAnomalyThreshold);
    assert(spike_state == TELEMETRY_STATE_ANOMALY);

    // 3. CRC failure maps cleanly to 0xFF fault state
    const telemetry_state_t fault_state =
        classify_sample(ewma, raw_voc, false, kVocAnomalyThreshold);
    assert(fault_state == TELEMETRY_STATE_SENSOR_FAULT);
}

}  // namespace

int main() {
    test_sensirion_codec();
    test_static_ring_buffer();
    test_diurnal_drift_and_voc_spike();
    std::cout << "ALL_PIPELINE_TESTS_PASSED\n";
    return 0;
}
