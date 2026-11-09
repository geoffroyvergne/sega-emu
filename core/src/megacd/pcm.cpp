#include "md/megacd/pcm.hpp"

#include <algorithm>
#include <cmath>

namespace md::megacd {

namespace {

constexpr u8 kLoopMarker = 0xFF;

} // namespace

void Pcm::reset() {
    std::fill(ram_.begin(), ram_.end(), u8{0});
    channels_ = {};
    on_ = 0;
    enabled_ = false;
    selected_ = 0;
    bank_ = 0;
    time_ = kCyclesPerSample;
    output_.clear();
    output_time_ = kCyclesPerSample;
    before_ = {};
    history_ = {};
}

void Pcm::set_lowpass(double cutoff_hz) {
    lowpass_hz_ = cutoff_hz;
    filtering_ = cutoff_hz > 0.0 && cutoff_hz < kSampleRate / 2;
    history_ = {};
    if (!filtering_) return;
    // RBJ cookbook low-pass, Q = 1/sqrt(2) (Butterworth).
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * cutoff_hz / kSampleRate;
    const double alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
    const double cosw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    b0_ = (1.0 - cosw) / 2.0 / a0;
    b1_ = (1.0 - cosw) / a0;
    b2_ = b0_;
    a1_ = -2.0 * cosw / a0;
    a2_ = (1.0 - alpha) / a0;
}

s32 Pcm::filter(int side, s32 input) {
    if (!filtering_) return input;
    auto& h = history_[static_cast<std::size_t>(side)];
    const double x = input;
    const double y = b0_ * x + b1_ * h[0] + b2_ * h[1] - a1_ * h[2] - a2_ * h[3];
    h[1] = h[0];
    h[0] = x;
    h[3] = h[2];
    h[2] = y;
    return static_cast<s32>(std::lround(y));
}

void Pcm::run_until(s64 sub_time) {
    while (time_ <= sub_time) {
        step();
        time_ += kCyclesPerSample;
    }
}

void Pcm::release_front() {
    before_ = output_.front();
    output_.pop_front();
    output_time_ += kCyclesPerSample;
}

void Pcm::output_at(s64 sub_time, s32& left, s32& right) {
    run_until(sub_time);
    // The last sample made at or before sub_time is output_[k]; interpolate
    // from the one before it, which becomes the new before_.
    while (!output_.empty() && output_time_ + kCyclesPerSample <= sub_time) release_front();
    if (output_.empty()) {
        left = before_[0];
        right = before_[1];
        return;
    }
    const auto& to = output_.front();
    const s64 into = std::clamp<s64>(sub_time - output_time_, 0, kCyclesPerSample);
    left  = before_[0] + static_cast<s32>((to[0] - before_[0]) * into / kCyclesPerSample);
    right = before_[1] + static_cast<s32>((to[1] - before_[1]) * into / kCyclesPerSample);
}

void Pcm::step() {
    s32 left = 0;
    s32 right = 0;
    if (enabled_) {
        for (int c = 0; c < kChannels; ++c) {
            auto& ch = channels_[static_cast<std::size_t>(c)];
            if (!(on_ & (1u << c))) {
                ch.addr = u32{ch.st} << (8 + kFraction);  // held at its start while off
                continue;
            }
            u8 data = ram_[(ch.addr >> kFraction) & (kRamSize - 1)];
            if (data == kLoopMarker) {
                ch.addr = u32{ch.ls} << kFraction;
                data = ram_[ch.ls];
            } else {
                ch.addr = (ch.addr + ch.fd) & kAddressMask;
            }
            if (data == kLoopMarker) continue;  // a loop onto a marker plays silence

            const s32 level = (data & 0x80) ? (data & 0x7F) : -(data & 0x7F);
            left  += (level * ch.env * (ch.pan & 0x0F)) >> 5;
            right += (level * ch.env * (ch.pan >> 4)) >> 5;
        }
        left  = std::clamp<s32>(left, -32768, 32767);
        right = std::clamp<s32>(right, -32768, 32767);
    }
    if (output_.size() >= kMaxBuffered) release_front();
    output_.push_back({filter(0, left), filter(1, right)});
}

u8 Pcm::read(u32 offset) const noexcept {
    if (!(offset & 1)) return 0;  // 8-bit device on the odd byte lane
    const u32 i = (offset >> 1) & 0x1FFF;
    if (i >= 0x1000) return ram_[bank_ + (i & 0x0FFF)];
    const u32 reg = i & 0x1F;
    if (reg >= 0x10) {  // 0xFF0021 + 4c: channel c's play address, low then high byte
        const auto& ch = channels_[(reg >> 1) & 7];
        return static_cast<u8>(ch.addr >> (kFraction + ((reg & 1) ? 8 : 0)));
    }
    return 0xFF;  // write-only registers
}

void Pcm::write(u32 offset, u8 value) {
    if (!(offset & 1)) return;
    const u32 i = (offset >> 1) & 0x1FFF;
    if (i >= 0x1000) {
        ram_[bank_ + (i & 0x0FFF)] = value;
        return;
    }
    auto& ch = channels_[selected_];
    switch (i & 0x1F) {
    case 0: ch.env = value; break;
    case 1: ch.pan = value; break;
    case 2: ch.fd = static_cast<u16>((ch.fd & 0xFF00) | value); break;
    case 3: ch.fd = static_cast<u16>((ch.fd & 0x00FF) | (value << 8)); break;
    case 4: ch.ls = static_cast<u16>((ch.ls & 0xFF00) | value); break;
    case 5: ch.ls = static_cast<u16>((ch.ls & 0x00FF) | (value << 8)); break;
    case 6:
        ch.st = value;
        if (!(on_ & (1u << selected_))) ch.addr = u32{ch.st} << (8 + kFraction);  // off: held at its start
        break;
    case 7:
        if (value & 0x40) {
            selected_ = value & 0x07;
        } else {
            bank_ = u32{value & 0x0Fu} << 12;
        }
        enabled_ = (value & 0x80) != 0;
        break;
    case 8:
        for (int c = 0; c < kChannels; ++c) {
            auto& each = channels_[static_cast<std::size_t>(c)];
            if (value & (1u << c)) each.addr = u32{each.st} << (8 + kFraction);
        }
        on_ = static_cast<u8>(~value);
        break;
    default:
        break;
    }
}

void Pcm::dma_write(u32 address, u16 value) noexcept {
    const u32 at = address & 0x0FFE;
    ram_[bank_ + at] = static_cast<u8>(value >> 8);
    ram_[bank_ + at + 1] = static_cast<u8>(value);
}

} // namespace md::megacd
