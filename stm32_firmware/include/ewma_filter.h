#ifndef EWMA_FILTER_H
#define EWMA_FILTER_H

#include <cmath>
#include <cstdint>

namespace edge_ai {

/**
 * @brief Exponentially Weighted Moving Average (EWMA) baseline drift filter.
 *
 * Tracks slow environmental macro-drift (e.g., diurnal temperature and humidity
 * shifts affecting MOX gas sensors) in O(1) time and static memory, yielding
 * zero-mean residuals for TinyML anomaly inference.
 */
class EwmaBaselineFilter {
public:
    /**
     * @param alpha          Smoothing factor in (0, 1]. Smaller values track
     *                       slower baseline drift; out-of-range values clamp.
     * @param warmup_samples Number of initial samples before residuals are valid.
     */
    constexpr explicit EwmaBaselineFilter(float alpha = 0.02f,
                                          std::uint32_t warmup_samples = 30U) noexcept
        : alpha_(sanitize_alpha(alpha)),
          warmup_samples_(warmup_samples) {}

    void reset() noexcept {
        baseline_ = 0.0f;
        sample_count_ = 0U;
        initialized_ = false;
    }

    /**
     * @brief Update the baseline estimate and return the drift-subtracted residual.
     * @return (sample - updated_baseline), or 0.0f on first/non-finite sample.
     */
    float update(float sample) noexcept {
        if (!std::isfinite(sample)) {
            return 0.0f;
        }

        if (!initialized_) {
            baseline_ = sample;
            initialized_ = true;
            sample_count_ = 1U;
            return 0.0f;
        }

        baseline_ = (alpha_ * sample) + ((1.0f - alpha_) * baseline_);
        ++sample_count_;
        return sample - baseline_;
    }

    [[nodiscard]] bool is_warmed_up() const noexcept {
        return initialized_ && (sample_count_ >= warmup_samples_);
    }

    [[nodiscard]] float baseline() const noexcept { return baseline_; }
    [[nodiscard]] float alpha() const noexcept { return alpha_; }
    [[nodiscard]] std::uint32_t sample_count() const noexcept { return sample_count_; }

private:
    static constexpr float sanitize_alpha(float alpha) noexcept {
        if (alpha <= 0.0f) {
            return 0.001f;
        }
        if (alpha > 1.0f) {
            return 1.0f;
        }
        return alpha;
    }

    float alpha_;
    std::uint32_t warmup_samples_;
    float baseline_ = 0.0f;
    std::uint32_t sample_count_ = 0U;
    bool initialized_ = false;
};

}  // namespace edge_ai

#endif /* EWMA_FILTER_H */
