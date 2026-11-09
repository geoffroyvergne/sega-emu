#include "md/psg.hpp"

#include <cmath>

namespace md {

namespace {

// Attenuation: 2 dB per step, step 15 = silence. Max amplitude per channel is
// chosen so the PSG sits a little below a full-volume FM channel in the mix.
constexpr s32 kMaxAmplitude = 2400;

const std::array<s32, 16>& volume_table() {
    static const std::array<s32, 16> table = [] {
        std::array<s32, 16> t{};
        for (int i = 0; i < 15; ++i) {
            t[static_cast<std::size_t>(i)] =
                static_cast<s32>(kMaxAmplitude * std::pow(10.0, -0.1 * i));  // -2 dB per step
        }
        t[15] = 0;
        return t;
    }();
    return table;
}

} // namespace

Psg::Psg() { reset(); }

void Psg::reset() {
    tone_.fill(0);
    volume_.fill(0x0F);
    noise_ = 0;
    counter_.fill(0);
    output_.fill(false);
    lfsr_ = 0x8000;
    latched_channel_ = 0;
    latched_volume_ = false;
    accumulator_ = 0;
    accumulated_ticks_ = 0;
}

// Byte protocol:
//   1 CC T DDDD   latch channel CC, type T (1 = volume), low 4 data bits
//   0 - DDDDDD    data: high 6 bits of a tone period, or 4 bits of volume/noise
void Psg::write(u8 value) {
    if (value & 0x80) {
        latched_channel_ = (value >> 5) & 3;
        latched_volume_  = (value & 0x10) != 0;
        const u8 data = value & 0x0F;
        if (latched_volume_) {
            volume_[static_cast<std::size_t>(latched_channel_)] = data;
        } else if (latched_channel_ < 3) {
            auto& t = tone_[static_cast<std::size_t>(latched_channel_)];
            t = static_cast<u16>((t & 0x3F0) | data);
        } else {
            noise_ = data & 0x07;
            lfsr_ = 0x8000;  // writing the noise register resets the shift register
        }
        return;
    }

    if (latched_volume_) {
        volume_[static_cast<std::size_t>(latched_channel_)] = value & 0x0F;
    } else if (latched_channel_ < 3) {
        auto& t = tone_[static_cast<std::size_t>(latched_channel_)];
        t = static_cast<u16>((t & 0x00F) | ((value & 0x3F) << 4));
    } else {
        noise_ = value & 0x07;
        lfsr_ = 0x8000;
    }
}

void Psg::tick() {
    // Tone channels: count down, flip the output at zero and reload.
    // Periods 0 and 1 hold the output high (used for PCM playback via volume).
    for (std::size_t ch = 0; ch < 3; ++ch) {
        if (tone_[ch] <= 1) {
            output_[ch] = true;
            continue;
        }
        if (counter_[ch] == 0 || --counter_[ch] == 0) {
            counter_[ch] = tone_[ch];
            output_[ch] = !output_[ch];
        }
    }

    // Noise: rate is 16/32/64 ticks, or follows tone channel 2. The LFSR
    // shifts on each rising edge of the noise flip-flop.
    static constexpr std::array<u16, 3> kNoisePeriods = {0x10, 0x20, 0x40};
    const u16 period = (noise_ & 3) == 3 ? tone_[2] : kNoisePeriods[noise_ & 3];
    if (counter_[3] == 0 || --counter_[3] == 0) {
        counter_[3] = period ? period : 1;
        output_[3] = !output_[3];
        if (output_[3]) {
            // Sega variant: 16-bit LFSR, white noise taps bits 0 and 3.
            const u16 feedback = (noise_ & 4) ? static_cast<u16>(((lfsr_ ^ (lfsr_ >> 3)) & 1))
                                              : static_cast<u16>(lfsr_ & 1);
            lfsr_ = static_cast<u16>((lfsr_ >> 1) | (feedback << 15));
        }
    }

    accumulator_ += current_output();
    ++accumulated_ticks_;
}

s32 Psg::current_output() const noexcept {
    const auto& vol = volume_table();
    s32 out = 0;
    for (std::size_t ch = 0; ch < 3; ++ch) {
        const s32 v = vol[volume_[ch]];
        out += output_[ch] ? v : -v;
    }
    const s32 nv = vol[volume_[3]];
    out += (lfsr_ & 1) ? nv : -nv;
    return out;
}

s32 Psg::take_average() {
    if (accumulated_ticks_ == 0) return current_output();
    const auto avg = static_cast<s32>(accumulator_ / accumulated_ticks_);
    accumulator_ = 0;
    accumulated_ticks_ = 0;
    return avg;
}

} // namespace md
