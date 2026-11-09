#pragma once

#include <array>

#include "md/types.hpp"

namespace md {

// Texas Instruments SN76489 (Sega variant, integrated in the VDP):
// 3 square-wave tone channels + 1 noise channel, 4-bit attenuation each.
//
// Clock: master / 15 (3.58 MHz). The tone counters tick every 16 clocks,
// i.e. every 240 master cycles. A tone channel with period N produces
// 3579545 / (32 * N) Hz.
class Psg {
public:
    static constexpr int kMasterCyclesPerTick = 15 * 16;  // 240
    static constexpr int kChannels = 4;

    Psg();

    void reset();

    // Register write (latch/data byte protocol).
    void write(u8 value);

    // Advances one internal tick (240 master cycles) and accumulates output.
    void tick();

    // Average output of the ticks since the previous call (box-filter
    // decimation to the mixer's rate). Signed, mono.
    [[nodiscard]] s32 take_average();

    // Inspection (tests, debugger).
    [[nodiscard]] u16 tone_period(int channel) const noexcept { return tone_[static_cast<std::size_t>(channel)]; }
    [[nodiscard]] u8  attenuation(int channel) const noexcept { return volume_[static_cast<std::size_t>(channel)]; }
    [[nodiscard]] u8  noise_control() const noexcept { return noise_; }

private:
    [[nodiscard]] s32 current_output() const noexcept;

    std::array<u16, 3> tone_{};            // 10-bit periods
    std::array<u8, kChannels> volume_{};   // 4-bit attenuation, 15 = off
    u8  noise_ = 0;                        // bit 2: white noise, bits 1-0: rate

    std::array<u16, kChannels> counter_{};
    std::array<bool, kChannels> output_{}; // square-wave flip-flops
    u16 lfsr_ = 0x8000;

    int latched_channel_ = 0;
    bool latched_volume_ = false;

    s64 accumulator_ = 0;
    int accumulated_ticks_ = 0;
};

} // namespace md
