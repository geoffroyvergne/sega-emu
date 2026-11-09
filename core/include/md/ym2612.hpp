#pragma once

#include <memory>

#include "md/types.hpp"

namespace md {

// Yamaha YM2612 (OPN2): 6 FM channels, channel 6 can be an 8-bit DAC,
// 2 timers. Clock: master / 7 (7.67 MHz); one output sample every 144 clocks
// (~53.3 kHz NTSC).
//
// Wrapper around the ymfm library (hidden behind a pimpl). Time only moves
// forward through generate(): the caller must generate samples up to the
// current emulated time before every read or write.
class Ym2612 {
public:
    static constexpr int kClocksPerSample = 144;

    Ym2612();
    ~Ym2612();

    Ym2612(const Ym2612&) = delete;
    Ym2612& operator=(const Ym2612&) = delete;

    void reset();

    // port 0: address (part I), 1: data (part I), 2: address (part II), 3: data (part II)
    void write(u32 port, u8 value);

    // Status: bit 7 = busy, bit 1 = timer B overflow, bit 0 = timer A overflow.
    [[nodiscard]] u8 read_status();

    // Produces one stereo sample (signed, roughly 16-bit range) and advances
    // the chip, including its timers, by 144 clocks.
    void generate(s32& left, s32& right);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace md
