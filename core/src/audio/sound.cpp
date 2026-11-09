#include "md/sound.hpp"

#include <algorithm>
#include <cmath>

namespace md {

namespace {

constexpr double kNtscMasterClock = 53'693'175.0;
constexpr double kPalMasterClock  = 53'203'424.0;

s16 clamp16(s32 v) {
    return static_cast<s16>(std::clamp<s32>(v, -32768, 32767));
}

} // namespace

Sound::Sound() {
    buffer_.reserve(4096);
    reset();
}

void Sound::reset() {
    ym_.reset();
    psg_.reset();
    next_sample_time_ = kMasterCyclesPerSample;
    psg_time_ = 0;
    buffer_.clear();
    dc_primed_ = false;
}

s16 Sound::dc_block(int channel, s32 input) {
    // y[n] = x[n] - x[n-1] + R * y[n-1], with R = 1 - 2*pi*fc/fs (fc ~ 10 Hz).
    constexpr double kR = 1.0 - 2.0 * 3.14159265358979 * 10.0 / 53267.0;
    const auto c = static_cast<std::size_t>(channel);
    const double x = input;
    const double y = x - dc_prev_in_[c] + kR * dc_prev_out_[c];
    dc_prev_in_[c] = x;
    dc_prev_out_[c] = y;
    return clamp16(static_cast<s32>(std::lround(y)));
}

double Sound::sample_rate(Region region) noexcept {
    return (region == Region::PAL ? kPalMasterClock : kNtscMasterClock) / kMasterCyclesPerSample;
}

void Sound::advance_psg(s64 master_time) {
    while (psg_time_ + Psg::kMasterCyclesPerTick <= master_time) {
        psg_.tick();
        psg_time_ += Psg::kMasterCyclesPerTick;
    }
}

void Sound::sync(s64 master_time) {
    while (next_sample_time_ <= master_time) {
        advance_psg(next_sample_time_);
        const s32 psg = psg_.take_average();
        s32 left = 0;
        s32 right = 0;
        ym_.generate(left, right);
        left += psg;
        right += psg;
        if (!dc_primed_) {
            // Start the filter at the current level, so playback doesn't
            // begin with a thump while the offset decays.
            dc_prev_in_[0] = left;
            dc_prev_in_[1] = right;
            dc_primed_ = true;
        }
        buffer_.push_back(dc_block(0, left));
        buffer_.push_back(dc_block(1, right));
        next_sample_time_ += kMasterCyclesPerSample;
    }
}

void Sound::sync_now() {
    if (time_source_) {
        const s64 now = time_source_();
        sync(now);
        advance_psg(now);
    }
}

u8 Sound::YmPort::read(u32 /*port*/) {
    // All four ports return the status register on the YM2612.
    sound_.sync_now();
    return sound_.ym_.read_status();
}

void Sound::YmPort::write(u32 port, u8 value) {
    sound_.sync_now();
    sound_.ym_.write(port, value);
}

void Sound::PsgPort::write(u32 /*port*/, u8 value) {
    sound_.sync_now();
    sound_.psg_.write(value);
}

} // namespace md
