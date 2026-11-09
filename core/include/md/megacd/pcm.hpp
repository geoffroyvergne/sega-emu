#pragma once

#include <array>
#include <deque>
#include <vector>

#include "md/types.hpp"

namespace md::megacd {

// Ricoh RF5C164 PCM sound chip: 8 channels playing 8-bit samples from 64 KB
// of wave RAM, at 12.5 MHz / 384 = 32,552 Hz.
//
// Sub CPU map (0xFF0000-0xFF7FFF, 8-bit device on the odd byte lane,
// mirrored every 16 KB):
//   0xFF0001 + 2n, n = 0-8   registers (write)
//   0xFF0021 + 4c (+2)       channel c's current play address, low (high) byte (read)
//   0xFF2001-0xFF3FFF        4 KB window into wave RAM (bank chosen by CTRL)
//
// Registers (ENV-ST apply to the channel selected by CTRL):
//   0 ENV   volume (0-255)          1 PAN   right (7-4) | left (3-0) volume
//   2 FDL   3 FDH   step per sample, 5.11 fixed point (0x0800 = 1 byte/sample)
//   4 LSL   5 LSH   loop address (where a 0xFF "loop marker" byte jumps to)
//   6 ST    start address / 256
//   7 CTRL  ON (7, sound output) | MOD (6): 1 = select channel (2-0),
//           0 = select wave RAM bank (3-0)
//   8 ONOFF bit c = 1: channel c off (its address is held at its start)
//
// Samples are sign-magnitude: bit 7 = sign (1 = positive), bits 6-0 = level.
// Behaviour follows Genesis Plus GX's model of the chip.
//
// Output filter: the chip holds each sample byte until the address moves to
// the next one, so a sample played slowly (step < 1, as the BIOS's music
// does) comes out as a staircase. On the console, the PCM output goes through
// an analog low-pass filter before mixing. Its cutoff isn't documented, so it is a setting: a
// 2-pole Butterworth low-pass at kDefaultLowpassHz (0 = no filter).
class Pcm {
public:
    static constexpr s64 kCyclesPerSample = 384;   // sub-CPU cycles
    static constexpr u32 kRamSize = 64 * 1024;
    static constexpr int kChannels = 8;
    static constexpr double kSampleRate = 12'500'000.0 / 384.0;  // 32,552 Hz
    static constexpr double kDefaultLowpassHz = 7'000.0;

    Pcm() { set_lowpass(kDefaultLowpassHz); reset(); }

    void reset();

    // Output low-pass cutoff in Hz; 0 (or >= half the sample rate) disables it.
    void set_lowpass(double cutoff_hz);
    [[nodiscard]] double lowpass() const noexcept { return lowpass_hz_; }

    // Generates the chip's samples up to `sub_time` (sub-CPU cycles). Calls
    // with an earlier time are ignored.
    void run_until(s64 sub_time);

    // Output at `sub_time`, linearly interpolated between the two chip
    // samples around it (lagging one sample). The chip keeps the samples it
    // generates until they are read: a register write runs it ahead to the
    // sub CPU's time, but the mixer may read later, for earlier times. Calls
    // must have non-decreasing times; samples before `sub_time` are released.
    void output_at(s64 sub_time, s32& left, s32& right);

    // Sub CPU access; `offset` is relative to 0xFF0000 (byte address).
    [[nodiscard]] u8 read(u32 offset) const noexcept;
    void write(u32 offset, u8 value);

    // CDC DMA: one word at byte `address` of the current wave RAM window.
    void dma_write(u32 address, u16 value) noexcept;

    // --- Inspection (tests, debugger) ----------------------------------------
    [[nodiscard]] u8 ram(u32 address) const noexcept { return ram_[address & (kRamSize - 1)]; }
    [[nodiscard]] u32 channel_address(int c) const noexcept {
        return channels_[static_cast<std::size_t>(c & 7)].addr >> kFraction;
    }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] s64 time() const noexcept { return time_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return output_.size(); }

    // Samples kept for a late reader (~0.5 s); with no reader, the oldest go.
    static constexpr std::size_t kMaxBuffered = 16384;

private:
    static constexpr int kFraction = 11;            // address = byte << 11
    static constexpr u32 kAddressMask = (kRamSize << kFraction) - 1;

    struct Channel {
        u8  env = 0;
        u8  pan = 0;
        u16 fd = 0;      // step
        u16 ls = 0;      // loop address
        u8  st = 0;      // start address >> 8
        u32 addr = 0;    // current address, 16.11 fixed point
    };

    void step();         // one output sample
    void release_front();
    [[nodiscard]] s32 filter(int side, s32 input);

    std::vector<u8> ram_ = std::vector<u8>(kRamSize, 0);
    std::array<Channel, kChannels> channels_{};
    u8   on_ = 0;         // bit c = channel c playing (inverse of ONOFF)
    bool enabled_ = false;
    u8   selected_ = 0;   // channel for registers 0-6
    u32  bank_ = 0;       // wave RAM window base

    s64 time_ = kCyclesPerSample;  // time of the next sample

    // Generated samples not yet read, oldest first; the first was made at
    // output_time_, the rest every kCyclesPerSample. before_ is the sample
    // before the first (the interpolation's starting point).
    std::deque<std::array<s32, 2>> output_;
    s64 output_time_ = kCyclesPerSample;
    std::array<s32, 2> before_{};

    // Biquad low-pass (direct form I), per side.
    double lowpass_hz_ = 0.0;
    bool   filtering_ = false;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::array<std::array<double, 4>, 2> history_{};  // x1, x2, y1, y2
};

} // namespace md::megacd
