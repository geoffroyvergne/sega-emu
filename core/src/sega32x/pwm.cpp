#include "md/sega32x/pwm.hpp"

#include <algorithm>

namespace md::sega32x {

namespace {

constexpr u16 merge(u16 old_value, u16 value, u16 lanes) noexcept {
    return static_cast<u16>((old_value & ~lanes) | (value & lanes));
}

constexpr u16 kMainControlBits = 0x000F;   // channel routing
constexpr u16 kSh2ControlBits  = 0x0F8F;   // + TM, RTP

} // namespace

void Pwm::Fifo::push(u16 v) noexcept {
    if (count == kFifoDepth) {
        data[(head + count - 1) % kFifoDepth] = v;   // full: the newest is replaced
        return;
    }
    data[(head + count) % kFifoDepth] = v;
    ++count;
}

bool Pwm::Fifo::pop(u16& v) noexcept {
    if (count == 0) return false;
    v = data[head];
    head = (head + 1) % kFifoDepth;
    --count;
    return true;
}

void Pwm::reset() {
    control_ = 0;
    cycle_ = 0;
    fifo_ = {};
    output_ = {};
    fed_ = {};
    clock_ = 0;
    timer_ = 0;
    samples_ = 0;
    time_ = 0;
    level_ = {};
    history_.clear();
    before_ = {};
}

u32 Pwm::sample_period() const noexcept {
    return static_cast<u32>((cycle_ - 1u) & 0x0FFFu);
}

int Pwm::timer_interval() const noexcept {
    const int tm = (control_ >> 8) & 0x0F;
    return tm == 0 ? 16 : tm;
}

u16 Pwm::read(u32 offset) const noexcept {
    switch (offset & 0x0E) {
    case 0x00: return control_;
    case 0x02: return cycle_;
    case 0x04: return fifo_[0].flags();
    case 0x06: return fifo_[1].flags();
    case 0x08: {
        const bool full = fifo_[0].count == kFifoDepth || fifo_[1].count == kFifoDepth;
        const bool empty = fifo_[0].count == 0 && fifo_[1].count == 0;
        return static_cast<u16>((full ? kFull : 0) | (empty ? kEmpty : 0));
    }
    default: return 0;
    }
}

void Pwm::write(u32 offset, u16 value, u16 lanes, bool from_sh2) {
    switch (offset & 0x0E) {
    case 0x00: {
        const u16 writable = from_sh2 ? kSh2ControlBits : kMainControlBits;
        control_ = merge(control_, value, static_cast<u16>(lanes & writable));
        break;
    }
    case 0x02: cycle_ = static_cast<u16>(merge(cycle_, value, lanes) & 0x0FFF); break;
    case 0x04: fifo_[0].push(static_cast<u16>(value & 0x0FFF)); break;
    case 0x06: fifo_[1].push(static_cast<u16>(value & 0x0FFF)); break;
    case 0x08:
        fifo_[0].push(static_cast<u16>(value & 0x0FFF));
        fifo_[1].push(static_cast<u16>(value & 0x0FFF));
        break;
    default: break;
    }
}

void Pwm::advance(s64 sh2_cycles) {
    const s64 start = time_;
    time_ += sh2_cycles;
    if (!running()) {
        clock_ = 0;
        if (level_ != std::array<s32, 2>{}) {
            // Channels off: the outputs fall silent.
            level_ = {};
            history_.push_back({time_, level_});
        }
        return;
    }
    const s64 period = sample_period();
    clock_ += sh2_cycles;
    s64 at = start + (sh2_cycles - clock_);   // time of the first sample due
    while (clock_ >= period) {
        clock_ -= period;
        at += period;
        sample();
        history_.push_back({at, level_});
        if (history_.size() > kMaxBuffered) {
            before_ = history_.front().level;
            history_.pop_front();
        }
    }
}

void Pwm::output_at(s64 sh2_time, s32& left, s32& right) {
    // Drop samples the reader is past (keeping the one it is after).
    while (history_.size() >= 2 && history_[1].time <= sh2_time) {
        before_ = history_.front().level;
        history_.pop_front();
    }
    if (history_.empty() || sh2_time < history_.front().time) {
        // Before the oldest sample kept: the level that preceded it. (The
        // mixer may read up to a frame behind the SH-2s.)
        left = before_[0];
        right = before_[1];
        return;
    }
    const Output& a = history_.front();
    if (history_.size() < 2) {
        left = a.level[0];
        right = a.level[1];
        return;
    }
    const Output& b = history_[1];
    const s64 span = b.time - a.time;
    const s64 into = sh2_time - a.time;
    left  = static_cast<s32>(a.level[0] + (b.level[0] - a.level[0]) * into / span);
    right = static_cast<s32>(a.level[1] + (b.level[1] - a.level[1]) * into / span);
}

void Pwm::sample() {
    ++samples_;
    for (std::size_t c = 0; c < 2; ++c) {
        u16 v = 0;
        if (fifo_[c].pop(v)) {
            output_[c] = v;   // an empty FIFO repeats its last width
            fed_[c] = true;
        }
    }
    // Width -> level: half the period is the midpoint (silence).
    const s32 cycle = static_cast<s32>(sample_period());
    const auto signed_level = [&](std::size_t c) {
        if (!fed_[c]) return s32{0};
        const s32 w = std::min<s32>(output_[c], cycle);
        return (2 * w - cycle) * kFullScale / cycle;
    };
    const auto route = [&](u16 mode, std::size_t same, std::size_t swapped) {
        switch (mode & 3) {
        case 1:  return signed_level(same);
        case 2:  return signed_level(swapped);
        default: return s32{0};
        }
    };
    level_[0] = route(control_, 0, 1);
    level_[1] = route(static_cast<u16>(control_ >> 2), 1, 0);
    if (++timer_ >= timer_interval()) {
        timer_ = 0;
        if (interrupt_) interrupt_();
        if ((control_ & 0x0080) && dreq_) dreq_();
    }
}

} // namespace md::sega32x
