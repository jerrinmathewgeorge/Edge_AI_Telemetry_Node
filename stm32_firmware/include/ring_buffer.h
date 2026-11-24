#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <array>
#include <cstddef>

namespace edge_ai {

/**
 * @brief Compile-time fixed-capacity SRAM ring buffer (zero heap allocation).
 *
 * Enforces deterministic memory bounds without malloc/free or dynamic STL
 * containers. Designed for DMA-fed sensor sampling on STM32 Cortex-M4F.
 */
template <typename T, std::size_t Capacity>
class StaticRingBuffer {
    static_assert(Capacity > 0U, "StaticRingBuffer capacity must be non-zero");

public:
    StaticRingBuffer() = default;

    void clear() noexcept {
        head_ = 0U;
        count_ = 0U;
    }

    /**
     * @brief Push a sample, overwriting the oldest entry when full.
     */
    void push(const T& item) noexcept {
        buffer_[head_] = item;
        head_ = (head_ + 1U) % Capacity;
        if (count_ < Capacity) {
            ++count_;
        }
    }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0U; }
    [[nodiscard]] bool full() const noexcept { return count_ == Capacity; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    /**
     * @brief Read element in chronological order (index 0 = oldest retained item).
     */
    bool at(std::size_t index, T& out) const noexcept {
        if (index >= count_) {
            return false;
        }
        const std::size_t oldest = (head_ + Capacity - count_) % Capacity;
        out = buffer_[(oldest + index) % Capacity];
        return true;
    }

    /**
     * @brief Copy retained elements into a contiguous static array (oldest -> newest).
     * @return Number of elements copied.
     */
    std::size_t copy_chronological(T* dst, std::size_t max_len) const noexcept {
        if (dst == nullptr || max_len == 0U || count_ == 0U) {
            return 0U;
        }
        const std::size_t to_copy = (count_ < max_len) ? count_ : max_len;
        const std::size_t oldest = (head_ + Capacity - count_) % Capacity;
        for (std::size_t i = 0U; i < to_copy; ++i) {
            dst[i] = buffer_[(oldest + i) % Capacity];
        }
        return to_copy;
    }

private:
    std::array<T, Capacity> buffer_{};
    std::size_t head_ = 0U;
    std::size_t count_ = 0U;
};

}  // namespace edge_ai

#endif /* RING_BUFFER_H */
