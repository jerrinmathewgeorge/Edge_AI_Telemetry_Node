#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "sensor_pipeline.h"

namespace s = edge_ai::sensirion;

static_assert(sizeof(edge_ai::SensorPipeline<32U>) < TELEMETRY_MAX_MODEL_RAM_BYTES,
              "SensorPipeline state + static ring buffer exceeds RAM budget");

namespace {

struct MockBus {
    edge_ai::SensorPipeline<16U>* pipeline = nullptr;

    std::uint16_t next_voc_raw = 30000U;
    std::uint16_t next_nox_raw = 15000U;
    bool nack_next_write = false;
    bool stall_next_rx = false;
    bool corrupt_sgp_crc = false;

    std::uint8_t last_sgp_cmd[8]{};
    std::uint32_t sgp_conditioning_cmds = 0U;
    std::uint32_t sgp_measure_cmds = 0U;

    std::array<std::uint8_t, 4> last_uart_frame{};
    std::uint8_t last_seq = 0U;
    telemetry_state_t last_state = TELEMETRY_STATE_CALIBRATING;
    std::uint32_t uart_frames_sent = 0U;

    // Pending DMA simulation
    enum class Pending { NONE, TX, RX } pending = Pending::NONE;
    std::uint8_t active_addr = 0U;
    std::uint8_t* active_rx = nullptr;
    std::size_t active_len = 0U;

    static void pack_word_with_crc(std::uint16_t word, std::uint8_t* dst) {
        dst[0] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
        dst[1] = static_cast<std::uint8_t>(word & 0xFFU);
        dst[2] = s::crc8(dst, 2U);
    }

    static bool i2c_write_dma(void* ctx, std::uint8_t addr, const std::uint8_t* tx, std::size_t len) {
        auto* self = static_cast<MockBus*>(ctx);
        if (self->nack_next_write) {
            self->nack_next_write = false;
            return false;
        }
        if (addr == SENSOR_ADDR_SGP41 && len == 8U) {
            for (std::size_t i = 0U; i < 8U; ++i) {
                self->last_sgp_cmd[i] = tx[i];
            }
            if (tx[0] == 0x26U && tx[1] == 0x12U) {
                ++self->sgp_conditioning_cmds;
            } else if (tx[0] == 0x26U && tx[1] == 0x19U) {
                ++self->sgp_measure_cmds;
            }
        }
        self->pending = Pending::TX;
        self->active_addr = addr;
        self->active_len = len;
        return true;
    }

    static bool i2c_read_dma(void* ctx, std::uint8_t addr, std::uint8_t* rx, std::size_t len) {
        auto* self = static_cast<MockBus*>(ctx);
        if (self->stall_next_rx) {
            self->stall_next_rx = false;
            return true;  // Accepted by HAL, but ISR never arrives -> forces timeout
        }
        self->pending = Pending::RX;
        self->active_addr = addr;
        self->active_rx = rx;
        self->active_len = len;
        return true;
    }

    static void uart_send_frame(void* ctx, const std::uint8_t frame[TELEMETRY_FRAME_SIZE_BYTES]) {
        auto* self = static_cast<MockBus*>(ctx);
        for (std::size_t i = 0U; i < TELEMETRY_FRAME_SIZE_BYTES; ++i) {
            self->last_uart_frame[i] = frame[i];
        }
        assert(telemetry_unpack_frame(frame, &self->last_seq, &self->last_state) == 1);
        ++self->uart_frames_sent;
    }

    void complete_dma(std::uint32_t now_ms) {
        if (pending == Pending::TX) {
            pending = Pending::NONE;
            pipeline->on_i2c_tx_done(now_ms);
        } else if (pending == Pending::RX) {
            pending = Pending::NONE;
            if (active_addr == SENSOR_ADDR_SHT45 && active_len == 6U) {
                pack_word_with_crc(0x6666U, &active_rx[0]);  // ~25 C
                pack_word_with_crc(0x8000U, &active_rx[3]);  // ~56.5% RH
            } else if (active_addr == SENSOR_ADDR_SGP41) {
                pack_word_with_crc(next_voc_raw, &active_rx[0]);
                if (active_len == 6U) {
                    pack_word_with_crc(next_nox_raw, &active_rx[3]);
                }
                if (corrupt_sgp_crc) {
                    corrupt_sgp_crc = false;
                    active_rx[2] ^= 0xFFU;
                }
            }
            pipeline->on_i2c_rx_done(now_ms);
        }
    }
};

void step_one_cycle(MockBus& bus, edge_ai::SensorPipeline<16U>& fsm, std::uint32_t& now_ms) {
    fsm.on_tick(now_ms);      // Starts SHT45 TX
    now_ms += 1U;
    bus.complete_dma(now_ms); // SHT45 TX done -> SHT45_WAIT
    now_ms += 10U;
    fsm.on_tick(now_ms);      // Starts SHT45 RX
    now_ms += 1U;
    bus.complete_dma(now_ms); // SHT45 RX done -> SGP41 TX
    now_ms += 1U;
    bus.complete_dma(now_ms); // SGP41 TX done -> SGP41_WAIT
    now_ms += 50U;
    fsm.on_tick(now_ms);      // Starts SGP41 RX
    now_ms += 1U;
    bus.complete_dma(now_ms); // SGP41 RX done -> IDLE + UART frame
    now_ms += 936U;           // Complete 1000 ms period
}

void test_conditioning_drift_and_spike() {
    MockBus bus{};
    edge_ai::PipelineConfig cfg{};
    cfg.conditioning_cycles = 10U;
    cfg.ewma_warmup_samples = 15U;

    edge_ai::PlatformOps ops{};
    ops.ctx = &bus;
    ops.i2c_write_dma = &MockBus::i2c_write_dma;
    ops.i2c_read_dma = &MockBus::i2c_read_dma;
    ops.uart_send_frame = &MockBus::uart_send_frame;

    edge_ai::SensorPipeline<16U> fsm(ops, cfg);
    bus.pipeline = &fsm;

    std::uint32_t now_ms = 1000U;

    // 1. First 10 cycles must run SGP41 conditioning (0x26 0x12) and emit CALIBRATING
    for (int i = 0; i < 10; ++i) {
        step_one_cycle(bus, fsm, now_ms);
        assert(bus.last_state == TELEMETRY_STATE_CALIBRATING);
        assert(bus.last_seq == static_cast<std::uint8_t>(i));
    }
    assert(bus.sgp_conditioning_cmds == 10U);
    assert(bus.sgp_measure_cmds == 0U);

    // 2. Cycles 11..14 finish EWMA warmup using measure_raw_signals (0x26 0x19)
    for (int i = 10; i < 14; ++i) {
        bus.next_voc_raw += 3U;
        step_one_cycle(bus, fsm, now_ms);
        assert(bus.last_state == TELEMETRY_STATE_CALIBRATING);
    }
    assert(bus.sgp_measure_cmds == 4U);

    // Verify SHT45 RH/T compensation (~56.5% RH, ~25 C) replaced defaults (not 0x80 0x00)
    assert(bus.last_sgp_cmd[0] == 0x26U && bus.last_sgp_cmd[1] == 0x19U);
    assert(bus.last_sgp_cmd[2] != 0x80U || bus.last_sgp_cmd[3] != 0x00U);

    // 3. Slow diurnal drift stays NORMAL (zero false alarms)
    for (int i = 0; i < 20; ++i) {
        bus.next_voc_raw += 4U;
        step_one_cycle(bus, fsm, now_ms);
        assert(bus.last_state == TELEMETRY_STATE_NORMAL);
    }

    // 4. Sudden VOC drop triggers ANOMALY (0x01)
    bus.next_voc_raw = static_cast<std::uint16_t>(bus.next_voc_raw - 1200U);
    step_one_cycle(bus, fsm, now_ms);
    assert(bus.last_state == TELEMETRY_STATE_ANOMALY);
}

void test_nack_timeout_and_crc_recovery() {
    MockBus bus{};
    edge_ai::PipelineConfig cfg{};
    cfg.conditioning_cycles = 2U;
    cfg.ewma_warmup_samples = 4U;

    edge_ai::PlatformOps ops{};
    ops.ctx = &bus;
    ops.i2c_write_dma = &MockBus::i2c_write_dma;
    ops.i2c_read_dma = &MockBus::i2c_read_dma;
    ops.uart_send_frame = &MockBus::uart_send_frame;

    edge_ai::SensorPipeline<16U> fsm(ops, cfg);
    bus.pipeline = &fsm;

    std::uint32_t now_ms = 1000U;
    for (int i = 0; i < 4; ++i) {
        step_one_cycle(bus, fsm, now_ms);
    }
    assert(bus.last_state == TELEMETRY_STATE_NORMAL);

    // 1. I2C NACK -> immediate FAULT_BACKOFF + 0xFF frame
    bus.nack_next_write = true;
    fsm.on_tick(now_ms);
    assert(fsm.fsm_state() == edge_ai::SensorPipeline<16U>::State::FAULT_BACKOFF);
    assert(bus.last_state == TELEMETRY_STATE_SENSOR_FAULT);
    assert(fsm.fault_count() == 1U);

    // Wait backoff period and recover cleanly (EWMA resets -> CALIBRATING then NORMAL)
    now_ms += 1000U;
    step_one_cycle(bus, fsm, now_ms);
    assert(bus.last_state == TELEMETRY_STATE_CALIBRATING);
    for (int i = 0; i < 3; ++i) {
        step_one_cycle(bus, fsm, now_ms);
    }
    assert(bus.last_state == TELEMETRY_STATE_NORMAL);

    // 2. Stalled DMA RX -> timeout recovery -> 0xFF frame
    bus.stall_next_rx = true;
    fsm.on_tick(now_ms);        // SHT45 TX
    now_ms += 1U;
    bus.complete_dma(now_ms);   // SHT45 WAIT
    now_ms += 10U;
    fsm.on_tick(now_ms);        // SHT45 RX (stalled!)
    assert(fsm.fsm_state() == edge_ai::SensorPipeline<16U>::State::SHT45_RX);
    now_ms += 60U;              // Exceeds 50 ms timeout
    fsm.on_tick(now_ms);
    assert(fsm.fsm_state() == edge_ai::SensorPipeline<16U>::State::FAULT_BACKOFF);
    assert(bus.last_state == TELEMETRY_STATE_SENSOR_FAULT);
    assert(fsm.fault_count() == 2U);

    // 3. Corrupted SGP41 CRC -> 0xFF frame without polluting ring buffer
    const std::size_t prev_samples = fsm.ring_buffer().size();
    now_ms += 1000U;
    bus.corrupt_sgp_crc = true;
    step_one_cycle(bus, fsm, now_ms);
    assert(bus.last_state == TELEMETRY_STATE_SENSOR_FAULT);
    assert(fsm.fault_count() == 3U);
    assert(fsm.ring_buffer().size() == prev_samples);
}

}  // namespace

int main() {
    test_conditioning_drift_and_spike();
    test_nack_timeout_and_crc_recovery();
    std::cout << "ALL_FSM_TESTS_PASSED\n";
    return 0;
}
