#pragma once

// Keyboard and gamepad input for the SDL frontend.

#include <SDL3/SDL.h>

#include <array>
#include <string>

#include "md/input.hpp"

namespace frontend {

class InputManager {
public:
    static constexpr int kPlayers = 2;

    InputManager();
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Feed every SDL event; handles gamepad hot-plugging. Returns a message to
    // show the user when a pad was connected or disconnected (else empty).
    std::string handle_event(const SDL_Event& event);

    // Current pad state for a player (0 or 1). Player 1 combines the keyboard
    // and the first gamepad; player 2 is the second gamepad.
    [[nodiscard]] md::ControllerState state(int player) const;

    [[nodiscard]] int connected_gamepads() const;

private:
    struct Slot {
        SDL_Gamepad*   pad = nullptr;
        SDL_JoystickID id  = 0;
    };

    std::string add(SDL_JoystickID id);
    std::string remove(SDL_JoystickID id);

    std::array<Slot, kPlayers> slots_{};
};

// Keyboard -> Mega Drive pad (player 1).
[[nodiscard]] md::ControllerState read_keyboard();

} // namespace frontend
