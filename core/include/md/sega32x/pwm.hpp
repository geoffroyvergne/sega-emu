#pragma once

#include <array>
#include <deque>
#include <functional>

#include "md/types.hpp"

namespace md::sega32x {

// The 32X's PWM sound: two channels of pulse widths, fed through 3-word
// FIFOs and played at a programmable rate. The FIFOs drain one entry per
// sample, the FULL/EMPTY flags games poll follow them, and the timer raises
// the PWM interrupt.
//
// Audio: a pulse width w out of a period of `cycle` SH-2 clocks is a level
// of w / cycle (filtered into a voltage on hardware); half the period is
// silence (as is a channel that has never been fed). Each sample's level, routed to the outputs, is kept with its SH-2
// time, and output_at() reads the outputs at any time for the mixer.
//
// Registers (68000 0xA15130+, SH-2 0x4030+):
//   00  control   TM (11-8) | RTP (7) | Rch (3-2) | Lch (1-0)
//                   TM: PWM interrupt every TM samples (0 = 16)
//                   RTP: DREQ1 request to the SH-2 DMA with each timer event
//                   Lch: left output  0 = off, 1 = L, 2 = R, 3 = off
//                   Rch: right output 0 = off, 1 = R, 2 = L, 3 = off
//                 The 68000 can write only bits 3-0.
//   02  cycle     12 bits: one sample every (cycle - 1) SH-2 cycles
//                 (e.g. 1047: 23.01 MHz / 1046 = 22 kHz); 1 = stopped
//   04  L width   W: into the left FIFO   R: FULL (15) | EMPTY (14)
//   06  R width   W: into the right FIFO  R: FULL (15) | EMPTY (14)
//   08  mono      W: into both FIFOs      R: FULL if either is full,
//                                            EMPTY if both are empty
// A write to a full FIFO replaces its newest entry.
class Pwm {
public:
    static constexpr std::size_t kFifoDepth = 3;
    static constexpr u16 kFull  = 0x8000;
    static constexpr u16 kEmpty = 0x4000;
    // Output level of a full-width (or zero-width) pulse.
    static constexpr s32 kFullScale = 16384;
    // Samples kept for a reader running behind (the mixer can be a frame
    // late): ~0.37 s at 22 kHz.
    static constexpr std::size_t kMaxBuffered = 8192;

    Pwm() { reset(); }
    void reset();

    // `offset` is relative to the PWM block (0x00-0x0E).
    [[nodiscard]] u16 read(u32 offset) const noexcept;
    void write(u32 offset, u16 value, u16 lanes, bool from_sh2);

    // Advances the sample clock by `sh2_cycles`: one FIFO entry per channel
    // leaves at each sample, and every TM samples the interrupt callback
    // fires. Nothing happens while both channels are off or cycle <= 1.
    void advance(s64 sh2_cycles);
    void set_interrupt_callback(std::function<void()> cb) { interrupt_ = std::move(cb); }
    // With RTP (control bit 7), the timer also requests a DMA transfer
    // (DREQ1 to the SH-2s) every TM samples, to refill the FIFOs.
    void set_dreq_callback(std::function<void()> cb) { dreq_ = std::move(cb); }

    // The stereo output at `sh2_time` (SH-2 cycles, the timeline advance()
    // runs on; reads must come in increasing time). Interpolates between the
    // samples around that time; holds the last one past the newest, and
    // gives the level before the oldest kept for earlier times.
    void output_at(s64 sh2_time, s32& left, s32& right);
    // The current output level, as output_at() gives it once reached.
    [[nodiscard]] std::array<s32, 2> level() const noexcept { return level_; }

    // --- Inspection ---
    [[nodiscard]] u32 sample_period() const noexcept;    // SH-2 cycles, 0 = stopped
    [[nodiscard]] int timer_interval() const noexcept;   // samples per interrupt
    [[nodiscard]] bool running() const noexcept { return (control_ & 0x0F) != 0 && sample_period() != 0; }
    [[nodiscard]] std::size_t fifo_size(int channel) const noexcept { return fifo_[static_cast<std::size_t>(channel & 1)].count; }
    [[nodiscard]] u16 output(int channel) const noexcept { return output_[static_cast<std::size_t>(channel & 1)]; }
    [[nodiscard]] u64 samples() const noexcept { return samples_; }
    [[nodiscard]] s64 time() const noexcept { return time_; }          // SH-2 time reached
    [[nodiscard]] std::size_t buffered() const noexcept { return history_.size(); }

private:
    struct Fifo {
        std::array<u16, kFifoDepth> data{};
        std::size_t head = 0;
        std::size_t count = 0;
        void push(u16 v) noexcept;
        bool pop(u16& v) noexcept;
        [[nodiscard]] u16 flags() const noexcept {
            return static_cast<u16>((count == kFifoDepth ? kFull : 0) | (count == 0 ? kEmpty : 0));
        }
    };
    void sample();
    struct Output {
        s64 time;
        std::array<s32, 2> level;
    };

    u16 control_ = 0;
    u16 cycle_ = 0;
    std::array<Fifo, 2> fifo_{};
    std::array<u16, 2> output_{};
    std::array<bool, 2> fed_{};     // a channel is silent until its first sample
    s64 clock_ = 0;        // SH-2 cycles into the current sample
    int timer_ = 0;        // samples since the last interrupt
    u64 samples_ = 0;
    s64 time_ = 0;                  // SH-2 time reached by advance()
    std::array<s32, 2> level_{};    // routed output, signed
    std::deque<Output> history_;    // samples not yet passed by the reader
    std::array<s32, 2> before_{};   // the level before history_.front()
    std::function<void()> interrupt_;
    std::function<void()> dreq_;
};

} // namespace md::sega32x
