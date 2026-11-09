#pragma once

// Mapping from a modern gamepad to a Mega Drive 6-button pad.
// Pure logic (no SDL types) so it can be unit-tested on its own.

#include "md/input.hpp"

namespace frontend {

// State of a standard-layout gamepad, by button *position* (SDL's naming:
// south = bottom face button, i.e. Xbox A / PlayStation Cross).
struct PadSnapshot {
    bool south = false, east = false, west = false, north = false;
    bool left_shoulder = false, right_shoulder = false;
    bool back = false, start = false;
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
    float left_x = 0.0f;  // -1 (left) .. +1 (right)
    float left_y = 0.0f;  // -1 (up)   .. +1 (down)
};

// Stick deflection needed to count as a direction (half travel).
inline constexpr float kStickThreshold = 0.5f;

// Same layout as RetroArch's Genesis cores: the three main buttons follow the
// arc of the face buttons (left, bottom, right), the second row goes on top.
//
//   Mega Drive   A     B       C      X               Y       Z               Start  Mode
//   Gamepad      west  south   east   left shoulder   north   right shoulder  start  back
inline md::ControllerState map_pad(const PadSnapshot& p) {
    md::ControllerState s;
    s.set(md::Button::A, p.west);
    s.set(md::Button::B, p.south);
    s.set(md::Button::C, p.east);
    s.set(md::Button::X, p.left_shoulder);
    s.set(md::Button::Y, p.north);
    s.set(md::Button::Z, p.right_shoulder);
    s.set(md::Button::Start, p.start);
    s.set(md::Button::Mode, p.back);

    // D-pad or left stick. Opposite directions cancel out, as on a real
    // pad's rocker (some games misbehave with Left+Right held together).
    const bool up    = p.dpad_up    || p.left_y < -kStickThreshold;
    const bool down  = p.dpad_down  || p.left_y >  kStickThreshold;
    const bool left  = p.dpad_left  || p.left_x < -kStickThreshold;
    const bool right = p.dpad_right || p.left_x >  kStickThreshold;
    s.set(md::Button::Up,    up && !down);
    s.set(md::Button::Down,  down && !up);
    s.set(md::Button::Left,  left && !right);
    s.set(md::Button::Right, right && !left);
    return s;
}

// Combines two input sources for the same player (e.g. keyboard + gamepad).
inline md::ControllerState merge(md::ControllerState a, md::ControllerState b) {
    a.buttons = static_cast<decltype(a.buttons)>(a.buttons | b.buttons);
    return a;
}

} // namespace frontend
