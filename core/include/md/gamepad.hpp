#pragma once

#include <functional>

#include "md/input.hpp"
#include "md/types.hpp"

namespace md {

// A control pad plugged into a controller port (3-button or 6-button).
//
// The console selects which buttons it reads with the TH line (data bit 6).
// Reads return 6 active-low bits (0 = pressed):
//
//   TH = 1:  C B Right Left Down Up
//   TH = 0:  Start A 0 0 Down Up              (the two 0s identify a pad)
//
// A 6-button pad counts TH transitions. Starting from TH = 1 (phase 0):
//
//   phase 0,2,4 (TH=1): C B Right Left Down Up
//   phase 1,3   (TH=0): Start A 0 0 Down Up
//   phase 5     (TH=0): Start A 0 0 0 0       <- identifies a 6-button pad
//   phase 6     (TH=1): C B Mode X Y Z
//   phase 7     (TH=0): Start A 1 1 1 1
//
// The counter resets when TH hasn't changed for ~1.5 ms, which games rely on
// between their once-per-frame reads.
class Gamepad {
public:
    enum class Type { ThreeButton, SixButton };

    // ~1.5 ms in master cycles.
    static constexpr s64 kSixButtonTimeout = 80'000;

    using TimeSource = std::function<s64()>;

    Gamepad() = default;
    explicit Gamepad(Type type) : type_(type) {}

    void reset();

    void set_type(Type type) noexcept { type_ = type; }
    [[nodiscard]] Type type() const noexcept { return type_; }

    void set_state(ControllerState state) noexcept { state_ = state; }
    [[nodiscard]] ControllerState state() const noexcept { return state_; }

    // Master-cycle clock for the 6-button timeout (no timeout if unset).
    void set_time_source(TimeSource source) { time_source_ = std::move(source); }

    // Level of TH as driven by the console (or pulled high when TH is an input).
    void set_th(bool level);

    // Bits 6-0 as seen on the port: bit 6 = TH, bits 5-0 = buttons (active low).
    [[nodiscard]] u8 read() const;

private:
    [[nodiscard]] s64 now() const { return time_source_ ? time_source_() : 0; }

    Type type_ = Type::SixButton;
    ControllerState state_{};
    TimeSource time_source_;
    bool th_ = true;
    int  phase_ = 0;
    s64  last_th_change_ = 0;
};

} // namespace md
