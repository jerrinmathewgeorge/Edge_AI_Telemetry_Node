#ifndef SENSOR_PIPELINE_H
#define SENSOR_PIPELINE_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ewma_filter.h"
#include "ring_buffer.h"
#include "sensirion_codec.h"
#include "telemetry_protocol.h"

namespace edge_ai {

/**
 * @brief Hardware abstraction callbacks for non-blocking DMA I2C & UART4 TX.
 *
 * All callbacks are plain function pointers (zero heap, zero RTTI/vtables) so
 * STM32 HAL DMA interrupts and host C++ mocks share the exact same FSM logic.
 */
struct PlatformOps {
    void* ctx = nullptr;
    bool (*i2c_write_dma)(void* ctx, std::uint8_t dev_addr_7bit, const std::uint8_t* tx, std::size_t len) = nullptr;
    bool (*i2c_read_dma)(void* ctx, std::uint8_t dev_addr_7bit, std::uint8_t* rx, std::size_t len) = nullptr;
    void (*uart_send_frame)(void* ctx, const std::uint8_t frame[TELEMETRY_FRAME_SIZE_BYTES]) = nullptr;
    telemetry_state_t (*classify_fn)(void* ctx,
                                     const env_sample_t& sample,
                                     float voc_residual,
                                     bool warmed_up) = nullptr;
};

struct PipelineConfig {
    std::uint32_t sample_period_ms = 1000U;
    std::uint32_t sht45_wait_ms = 10U;
    std::uint32_t sgp41_wait_ms = 50U;
    std::uint32_t i2c_timeout_ms = 50U;
    std::uint32_t conditioning_cycles = 10U;
    float ewma_alpha = 0.05f;
    std::uint32_t ewma_warmup_samples = 20U;
    float fallback_voc_threshold = 450.0f;
};

/**
 * @brief Non-blocking DMA I2C acquisition, conditioning, EWMA, and framing FSM.
 *
 * Enforces SGP41 10-second conditioning (0x26 0x12) prior to RH-compensated
 * measurement (0x26 0x19), zero HAL_Delay loops, static SRAM storage, and
 * automatic NACK/timeout/CRC recovery via TELEMETRY_STATE_SENSOR_FAULT (0xFF).
 */
template <std::size_t RingCapacity = 32U>
class SensorPipeline {
public:
    enum class State : std::uint8_t {
        IDLE = 0,
        SHT45_TX,
        SHT45_WAIT,
        SHT45_RX,
        SGP41_TX,
        SGP41_WAIT,
        SGP41_RX,
        FAULT_BACKOFF
    };

    explicit SensorPipeline(const PlatformOps& ops, const PipelineConfig& cfg = PipelineConfig{}) noexcept
        : ops_(ops),
          cfg_(cfg),
          ewma_(cfg.ewma_alpha, cfg.ewma_warmup_samples) {}

    void reset(std::uint32_t now_ms = 0U) noexcept {
        state_ = State::IDLE;
        last_cycle_start_ms_ = now_ms;
        state_entered_ms_ = now_ms;
        conditioning_done_ = 0U;
        seq_id_ = 0U;
        fault_count_ = 0U;
        had_fault_ = false;
        last_temp_c_ = 25.0f;
        last_rh_pct_ = 50.0f;
        last_state_ = TELEMETRY_STATE_CALIBRATING;
        ring_buffer_.clear();
        ewma_.reset();
    }

    /**
     * @brief Periodic non-blocking tick (call from timer or main superloop).
     */
    void on_tick(std::uint32_t now_ms) noexcept {
        switch (state_) {
            case State::IDLE:
                if (has_elapsed(now_ms, last_cycle_start_ms_, cfg_.sample_period_ms)) {
                    start_sht45_measurement(now_ms);
                }
                break;

            case State::SHT45_WAIT:
                if (has_elapsed(now_ms, state_entered_ms_, cfg_.sht45_wait_ms)) {
                    start_sht45_read(now_ms);
                }
                break;

            case State::SGP41_WAIT:
                if (has_elapsed(now_ms, state_entered_ms_, cfg_.sgp41_wait_ms)) {
                    start_sgp41_read(now_ms);
                }
                break;

            case State::SHT45_TX:
            case State::SHT45_RX:
            case State::SGP41_TX:
            case State::SGP41_RX:
                if (has_elapsed(now_ms, state_entered_ms_, cfg_.i2c_timeout_ms)) {
                    enter_fault(now_ms);
                }
                break;

            case State::FAULT_BACKOFF:
                if (has_elapsed(now_ms, state_entered_ms_, cfg_.sample_period_ms)) {
                    start_sht45_measurement(now_ms);
                }
                break;
        }
    }

    /**
     * @brief Trigger an immediate acquisition cycle (useful on boot or timer edge).
     */
    void trigger_sample(std::uint32_t now_ms) noexcept {
        if (state_ == State::IDLE || state_ == State::FAULT_BACKOFF) {
            start_sht45_measurement(now_ms);
        }
    }

    /**
     * @brief Notify FSM that DMA I2C Master Tx completed (HAL_I2C_MasterTxCpltCallback).
     */
    void on_i2c_tx_done(std::uint32_t now_ms) noexcept {
        if (state_ == State::SHT45_TX) {
            transition(State::SHT45_WAIT, now_ms);
        } else if (state_ == State::SGP41_TX) {
            transition(State::SGP41_WAIT, now_ms);
        }
    }

    /**
     * @brief Notify FSM that DMA I2C Master Rx completed (HAL_I2C_MasterRxCpltCallback).
     */
    void on_i2c_rx_done(std::uint32_t now_ms) noexcept {
        if (state_ == State::SHT45_RX) {
            float temp_c = 0.0f;
            float rh_pct = 0.0f;
            if (!sensirion::decode_sht45_frame(rx_buf_.data(), temp_c, rh_pct)) {
                enter_fault(now_ms);
                return;
            }
            last_temp_c_ = temp_c;
            last_rh_pct_ = rh_pct;
            start_sgp41_command(now_ms);
            return;
        }

        if (state_ == State::SGP41_RX) {
            handle_sgp41_response(now_ms);
        }
    }

    /**
     * @brief Notify FSM of I2C NACK / bus error (HAL_I2C_ErrorCallback).
     */
    void on_i2c_error(std::uint32_t now_ms) noexcept {
        if (state_ != State::IDLE && state_ != State::FAULT_BACKOFF) {
            enter_fault(now_ms);
        }
    }

    [[nodiscard]] State fsm_state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t conditioning_done() const noexcept { return conditioning_done_; }
    [[nodiscard]] std::uint32_t fault_count() const noexcept { return fault_count_; }
    [[nodiscard]] std::uint8_t next_seq_id() const noexcept { return seq_id_; }
    [[nodiscard]] telemetry_state_t last_telemetry_state() const noexcept { return last_state_; }
    [[nodiscard]] const StaticRingBuffer<env_sample_t, RingCapacity>& ring_buffer() const noexcept {
        return ring_buffer_;
    }
    [[nodiscard]] const EwmaBaselineFilter& ewma() const noexcept { return ewma_; }

private:
    static bool has_elapsed(std::uint32_t now_ms,
                            std::uint32_t since_ms,
                            std::uint32_t duration_ms) noexcept {
        return static_cast<std::uint32_t>(now_ms - since_ms) >= duration_ms;
    }

    void transition(State next, std::uint32_t now_ms) noexcept {
        state_ = next;
        state_entered_ms_ = now_ms;
    }

    void emit_state(telemetry_state_t state) noexcept {
        last_state_ = state;
        if (ops_.uart_send_frame == nullptr) {
            return;
        }
        std::uint8_t frame[TELEMETRY_FRAME_SIZE_BYTES]{};
        telemetry_pack_frame(seq_id_, state, frame);
        ++seq_id_;
        ops_.uart_send_frame(ops_.ctx, frame);
    }

    void enter_fault(std::uint32_t now_ms) noexcept {
        ++fault_count_;
        had_fault_ = true;
        transition(State::FAULT_BACKOFF, now_ms);
        emit_state(TELEMETRY_STATE_SENSOR_FAULT);
    }

    void start_sht45_measurement(std::uint32_t now_ms) noexcept {
        last_cycle_start_ms_ = now_ms;
        tx_buf_[0] = sensirion::SHT45_CMD_MEASURE_HIGH_PRECISION;
        if (ops_.i2c_write_dma == nullptr) {
            enter_fault(now_ms);
            return;
        }
        transition(State::SHT45_TX, now_ms);
        if (!ops_.i2c_write_dma(ops_.ctx, SENSOR_ADDR_SHT45, tx_buf_.data(), 1U)) {
            enter_fault(now_ms);
        }
    }

    void start_sht45_read(std::uint32_t now_ms) noexcept {
        if (ops_.i2c_read_dma == nullptr) {
            enter_fault(now_ms);
            return;
        }
        transition(State::SHT45_RX, now_ms);
        if (!ops_.i2c_read_dma(ops_.ctx, SENSOR_ADDR_SHT45, rx_buf_.data(), 6U)) {
            enter_fault(now_ms);
        }
    }

    void start_sgp41_command(std::uint32_t now_ms) noexcept {
        if (ops_.i2c_write_dma == nullptr) {
            enter_fault(now_ms);
            return;
        }
        const bool is_conditioning = (conditioning_done_ < cfg_.conditioning_cycles);
        const auto cmd = is_conditioning
                             ? sensirion::build_sgp41_conditioning_cmd(last_rh_pct_, last_temp_c_)
                             : sensirion::build_sgp41_measure_cmd(last_rh_pct_, last_temp_c_);
        for (std::size_t i = 0U; i < cmd.size(); ++i) {
            tx_buf_[i] = cmd[i];
        }
        transition(State::SGP41_TX, now_ms);
        if (!ops_.i2c_write_dma(ops_.ctx, SENSOR_ADDR_SGP41, tx_buf_.data(), cmd.size())) {
            enter_fault(now_ms);
        }
    }

    void start_sgp41_read(std::uint32_t now_ms) noexcept {
        if (ops_.i2c_read_dma == nullptr) {
            enter_fault(now_ms);
            return;
        }
        const std::size_t rx_len = (conditioning_done_ < cfg_.conditioning_cycles) ? 3U : 6U;
        transition(State::SGP41_RX, now_ms);
        if (!ops_.i2c_read_dma(ops_.ctx, SENSOR_ADDR_SGP41, rx_buf_.data(), rx_len)) {
            enter_fault(now_ms);
        }
    }

    void handle_sgp41_response(std::uint32_t now_ms) noexcept {
        if (had_fault_) {
            // Re-seed EWMA after sensor recovery to prevent reconnect spike false alarms.
            ewma_.reset();
            had_fault_ = false;
        }

        if (conditioning_done_ < cfg_.conditioning_cycles) {
            std::uint16_t voc_raw = 0U;
            if (!sensirion::decode_sgp41_conditioning(rx_buf_.data(), voc_raw)) {
                enter_fault(now_ms);
                return;
            }
            ++conditioning_done_;
            const env_sample_t sample{last_temp_c_, last_rh_pct_, voc_raw, 0U};
            ring_buffer_.push(sample);
            (void)ewma_.update(static_cast<float>(voc_raw));
            transition(State::IDLE, now_ms);
            emit_state(TELEMETRY_STATE_CALIBRATING);
            return;
        }

        std::uint16_t voc_raw = 0U;
        std::uint16_t nox_raw = 0U;
        if (!sensirion::decode_sgp41_frame(rx_buf_.data(), voc_raw, nox_raw)) {
            enter_fault(now_ms);
            return;
        }

        const env_sample_t sample{last_temp_c_, last_rh_pct_, voc_raw, nox_raw};
        ring_buffer_.push(sample);
        const float residual = ewma_.update(static_cast<float>(voc_raw));
        transition(State::IDLE, now_ms);

        telemetry_state_t out_state = TELEMETRY_STATE_CALIBRATING;
        if (ops_.classify_fn != nullptr) {
            out_state = ops_.classify_fn(ops_.ctx, sample, residual, ewma_.is_warmed_up());
        } else if (ewma_.is_warmed_up()) {
            out_state = (std::fabs(residual) > cfg_.fallback_voc_threshold)
                            ? TELEMETRY_STATE_ANOMALY
                            : TELEMETRY_STATE_NORMAL;
        }
        emit_state(out_state);
    }

    PlatformOps ops_;
    PipelineConfig cfg_;
    StaticRingBuffer<env_sample_t, RingCapacity> ring_buffer_;
    EwmaBaselineFilter ewma_;

    std::array<std::uint8_t, 8U> tx_buf_{};
    std::array<std::uint8_t, 6U> rx_buf_{};

    State state_ = State::IDLE;
    std::uint32_t last_cycle_start_ms_ = 0U;
    std::uint32_t state_entered_ms_ = 0U;
    std::uint32_t conditioning_done_ = 0U;
    std::uint32_t fault_count_ = 0U;
    std::uint8_t seq_id_ = 0U;
    bool had_fault_ = false;
    float last_temp_c_ = 25.0f;
    float last_rh_pct_ = 50.0f;
    telemetry_state_t last_state_ = TELEMETRY_STATE_CALIBRATING;
};

}  // namespace edge_ai

#endif /* SENSOR_PIPELINE_H */
