#pragma once

#include <cstdint>

namespace md {

// Buttons of a 6-button Mega Drive pad. The frontend maps its own input
// devices (keyboard, gamepad, ...) onto this; the core never sees SDL keys.
enum class Button : std::uint16_t {
    Up    = 1u << 0,
    Down  = 1u << 1,
    Left  = 1u << 2,
    Right = 1u << 3,
    A     = 1u << 4,
    B     = 1u << 5,
    C     = 1u << 6,
    Start = 1u << 7,
    X     = 1u << 8,
    Y     = 1u << 9,
    Z     = 1u << 10,
    Mode  = 1u << 11,
};

struct ControllerState {
    std::uint16_t buttons = 0;

    [[nodiscard]] constexpr bool is_pressed(Button b) const noexcept {
        return (buttons & static_cast<std::uint16_t>(b)) != 0;
    }

    constexpr void set(Button b, bool pressed) noexcept {
        const auto mask = static_cast<std::uint16_t>(b);
        buttons = static_cast<std::uint16_t>(pressed ? (buttons | mask) : (buttons & ~mask));
    }
};

} // namespace md
