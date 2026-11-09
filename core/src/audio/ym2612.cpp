#include "md/ym2612.hpp"

#include <ymfm_opn.h>

namespace md {

namespace {

// ymfm asks its host to keep time: timers and the busy flag are expressed in
// chip clocks. We track a clock counter that generate() advances.
class Interface final : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        deadline_[tnum] = duration_in_clocks < 0 ? -1 : clock_ + duration_in_clocks;
    }

    void ymfm_set_busy_end(uint32_t clocks) override { busy_end_ = clock_ + clocks; }
    bool ymfm_is_busy() override { return clock_ < busy_end_; }

    // The YM2612's IRQ pin is not connected on the Mega Drive.
    void ymfm_update_irq(bool) override {}

    void advance(s64 clocks) {
        const s64 target = clock_ + clocks;
        for (;;) {
            // Fire timers in order, with the clock set to the exact expiry
            // time so ymfm's automatic reload doesn't accumulate drift.
            int next = -1;
            for (int t = 0; t < 2; ++t) {
                if (deadline_[t] >= 0 && deadline_[t] <= target &&
                    (next < 0 || deadline_[t] < deadline_[next])) {
                    next = t;
                }
            }
            if (next < 0) break;
            clock_ = deadline_[next];
            deadline_[next] = -1;
            m_engine->engine_timer_expired(static_cast<uint32_t>(next));
        }
        clock_ = target;
    }

    void reset() {
        clock_ = 0;
        busy_end_ = 0;
        deadline_[0] = deadline_[1] = -1;
    }

private:
    s64 clock_ = 0;
    s64 busy_end_ = 0;
    s64 deadline_[2] = {-1, -1};
};

} // namespace

struct Ym2612::Impl {
    Interface intf;
    ymfm::ym2612 chip{intf};  // original YM2612, with the DAC "ladder effect"
};

Ym2612::Ym2612() : impl_(std::make_unique<Impl>()) { reset(); }
Ym2612::~Ym2612() = default;

void Ym2612::reset() {
    impl_->intf.reset();
    impl_->chip.reset();
}

void Ym2612::write(u32 port, u8 value) {
    impl_->chip.write(port & 3, value);
}

u8 Ym2612::read_status() {
    return impl_->chip.read_status();
}

void Ym2612::generate(s32& left, s32& right) {
    ymfm::ym2612::output_data out;
    impl_->chip.generate(&out, 1);
    impl_->intf.advance(kClocksPerSample);
    left  = out.data[0];
    right = out.data[1];
}

} // namespace md
