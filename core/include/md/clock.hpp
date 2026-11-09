#pragma once

#include "md/types.hpp"

namespace md {

// Converts elapsed master-clock cycles into cycles of another clock running
// at an unrelated frequency (e.g. the Mega-CD's 12.5 MHz sub CPU against the
// 53.69 MHz Mega Drive master clock), without floating point and without
// drift: the fractional remainder is carried from one call to the next.
//
//   target cycles = master cycles x target_hz / master_hz   (exactly, over time)
class ClockConverter {
public:
    ClockConverter(u64 target_hz, u64 master_hz) noexcept
        : target_hz_(target_hz), master_hz_(master_hz) {}

    // Returns how many target cycles elapse during `master_cycles`.
    s64 advance(s64 master_cycles) noexcept {
        // remainder_ < master_hz_ and master_cycles is a slice (a few
        // thousand), so this product stays far below 2^63.
        remainder_ += static_cast<u64>(master_cycles) * target_hz_;
        const u64 whole = remainder_ / master_hz_;
        remainder_ %= master_hz_;
        total_ += static_cast<s64>(whole);
        return static_cast<s64>(whole);
    }

    // Target cycles elapsed since construction/reset.
    [[nodiscard]] s64 total() const noexcept { return total_; }

    void reset() noexcept {
        remainder_ = 0;
        total_ = 0;
    }

private:
    u64 target_hz_;
    u64 master_hz_;
    u64 remainder_ = 0;
    s64 total_ = 0;
};

} // namespace md
