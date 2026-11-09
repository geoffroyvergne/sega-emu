#include "md/gamepad.hpp"

namespace md {

void Gamepad::reset() {
    th_ = true;
    phase_ = 0;
    last_th_change_ = now();
}

void Gamepad::set_th(bool level) {
    if (level == th_) return;

    const s64 t = now();
    if (time_source_ && t - last_th_change_ > kSixButtonTimeout) {
        // Idle too long: the pad's counter has reset (phase 0 = TH high,
        // phase 1 = TH low), so this edge starts a fresh sequence.
        phase_ = th_ ? 0 : 1;
    }
    last_th_change_ = t;
    th_ = level;
    phase_ = (phase_ + 1) & 7;
}

u8 Gamepad::read() const {
    // Bit set = released (active low). bit(b) is 1 when button b is NOT pressed.
    const auto bit = [this](Button b, int position) -> u8 {
        return state_.is_pressed(b) ? 0 : static_cast<u8>(1u << position);
    };

    // Within the timeout window the 6-button sequence applies; a read after
    // a long idle period sees phase 0/1 again.
    int phase = phase_;
    if (time_source_ && now() - last_th_change_ > kSixButtonTimeout) phase = th_ ? 0 : 1;
    if (type_ == Type::ThreeButton) phase = th_ ? 0 : 1;

    const u8 th = th_ ? 0x40 : 0x00;

    switch (phase) {
    case 5:  // TH=0: Start A 0 0 0 0
        return static_cast<u8>(th | bit(Button::Start, 5) | bit(Button::A, 4));
    case 6:  // TH=1: C B Mode X Y Z
        return static_cast<u8>(th | bit(Button::C, 5) | bit(Button::B, 4) | bit(Button::Mode, 3) |
                               bit(Button::X, 2) | bit(Button::Y, 1) | bit(Button::Z, 0));
    case 7:  // TH=0: Start A 1 1 1 1
        return static_cast<u8>(th | bit(Button::Start, 5) | bit(Button::A, 4) | 0x0F);
    default:
        break;
    }

    if (th_) {  // C B Right Left Down Up
        return static_cast<u8>(th | bit(Button::C, 5) | bit(Button::B, 4) | bit(Button::Right, 3) |
                               bit(Button::Left, 2) | bit(Button::Down, 1) | bit(Button::Up, 0));
    }
    // Start A 0 0 Down Up
    return static_cast<u8>(th | bit(Button::Start, 5) | bit(Button::A, 4) |
                           bit(Button::Down, 1) | bit(Button::Up, 0));
}

} // namespace md
