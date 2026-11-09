// Frontend input: gamepad -> Mega Drive pad mapping, and the SDL gamepad path
// exercised with SDL's virtual gamepads (no physical device needed).

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "input.hpp"
#include "pad_mapping.hpp"
#include "test.hpp"

using frontend::PadSnapshot;
using md::Button;

// --- Pure mapping ------------------------------------------------------------

TEST(map_pad_face_and_shoulder_buttons) {
    const auto only = [](auto field) {
        PadSnapshot p;
        p.*field = true;
        return frontend::map_pad(p);
    };
    CHECK(only(&PadSnapshot::west).is_pressed(Button::A));
    CHECK(only(&PadSnapshot::south).is_pressed(Button::B));
    CHECK(only(&PadSnapshot::east).is_pressed(Button::C));
    CHECK(only(&PadSnapshot::left_shoulder).is_pressed(Button::X));
    CHECK(only(&PadSnapshot::north).is_pressed(Button::Y));
    CHECK(only(&PadSnapshot::right_shoulder).is_pressed(Button::Z));
    CHECK(only(&PadSnapshot::start).is_pressed(Button::Start));
    CHECK(only(&PadSnapshot::back).is_pressed(Button::Mode));

    // Each button maps to exactly one Mega Drive button.
    CHECK(only(&PadSnapshot::south).buttons == static_cast<std::uint16_t>(Button::B));
    CHECK(frontend::map_pad(PadSnapshot{}).buttons == 0);
}

TEST(map_pad_dpad_and_stick) {
    PadSnapshot p;
    p.dpad_up = true;
    CHECK(frontend::map_pad(p).is_pressed(Button::Up));

    PadSnapshot s;
    s.left_x = 0.4f;                                // inside the dead zone
    CHECK(frontend::map_pad(s).buttons == 0);
    s.left_x = 0.8f;
    s.left_y = -0.9f;
    const auto st = frontend::map_pad(s);
    CHECK(st.is_pressed(Button::Right) && st.is_pressed(Button::Up));
    CHECK(!st.is_pressed(Button::Left) && !st.is_pressed(Button::Down));
}

TEST(map_pad_opposite_directions_cancel) {
    PadSnapshot p;
    p.dpad_left = true;
    p.left_x = 1.0f;                                // stick right + d-pad left
    const auto s = frontend::map_pad(p);
    CHECK(!s.is_pressed(Button::Left) && !s.is_pressed(Button::Right));
}

TEST(merge_combines_sources) {
    md::ControllerState a, b;
    a.set(Button::A, true);
    b.set(Button::Start, true);
    const auto m = frontend::merge(a, b);
    CHECK(m.is_pressed(Button::A) && m.is_pressed(Button::Start));
}

// --- SDL path with virtual gamepads -------------------------------------------

namespace {

// SDL with only virtual gamepads: real devices plugged into the machine are
// hidden, so the tests don't depend on what's connected.
struct SdlGamepads {
    bool ok = init();
    ~SdlGamepads() { SDL_Quit(); }

    static bool init() {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        return SDL_Init(SDL_INIT_GAMEPAD);
    }
};

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;

    VirtualPad() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "sega-emu virtual pad";
        id = SDL_AttachVirtualJoystick(&desc);
        joystick = id ? SDL_OpenJoystick(id) : nullptr;
    }
    ~VirtualPad() {
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
    }

    void button(SDL_GamepadButton b, bool down) {
        SDL_SetJoystickVirtualButton(joystick, static_cast<int>(b), down);
    }
    void axis(SDL_GamepadAxis a, Sint16 value) {
        SDL_SetJoystickVirtualAxis(joystick, static_cast<int>(a), value);
    }
};

// Delivers pending SDL events to the input manager; returns its messages.
std::vector<std::string> pump(frontend::InputManager& input) {
    SDL_UpdateJoysticks();
    std::vector<std::string> messages;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (auto m = input.handle_event(e); !m.empty()) messages.push_back(m);
    }
    return messages;
}

} // namespace

TEST(virtual_gamepad_drives_player_1_and_2) {
    SdlGamepads sdl;
    CHECK(sdl.ok);
    if (!sdl.ok) return;

    frontend::InputManager input;
    VirtualPad pad1;
    CHECK(pad1.id != 0);
    auto messages = pump(input);
    CHECK(input.connected_gamepads() == 1);
    CHECK(!messages.empty() && messages[0].find("player 1") != std::string::npos);

    pad1.button(SDL_GAMEPAD_BUTTON_SOUTH, true);    // -> B
    pad1.button(SDL_GAMEPAD_BUTTON_START, true);    // -> Start
    pad1.axis(SDL_GAMEPAD_AXIS_LEFTX, -32767);      // -> Left
    pump(input);
    const auto s1 = input.state(0);
    CHECK(s1.is_pressed(Button::B));
    CHECK(s1.is_pressed(Button::Start));
    CHECK(s1.is_pressed(Button::Left));
    CHECK(!s1.is_pressed(Button::A));
    CHECK(input.state(1).buttons == 0);

    {
        VirtualPad pad2;
        messages = pump(input);
        CHECK(input.connected_gamepads() == 2);
        CHECK(!messages.empty() && messages[0].find("player 2") != std::string::npos);

        pad2.button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);  // -> X
        pad2.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);      // -> Down
        pump(input);
        const auto s2 = input.state(1);
        CHECK(s2.is_pressed(Button::X) && s2.is_pressed(Button::Down));
        CHECK(!s2.is_pressed(Button::B));               // pad 1's buttons stay on player 1
    }

    // Unplugging pad 2 frees player 2; pad 1 keeps working.
    messages = pump(input);
    CHECK(input.connected_gamepads() == 1);
    CHECK(!messages.empty() && messages[0].find("disconnected (player 2)") != std::string::npos);
    CHECK(input.state(1).buttons == 0);
    pad1.button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump(input);
    CHECK(!input.state(0).is_pressed(Button::B));
    CHECK(input.state(0).is_pressed(Button::Start));
}

TEST(virtual_gamepad_reconnect_takes_free_slot) {
    SdlGamepads sdl;
    if (!sdl.ok) { CHECK(sdl.ok); return; }

    frontend::InputManager input;
    {
        VirtualPad pad;
        pump(input);
        CHECK(input.connected_gamepads() == 1);
    }
    pump(input);
    CHECK(input.connected_gamepads() == 0);

    VirtualPad again;
    const auto messages = pump(input);
    CHECK(input.connected_gamepads() == 1);
    CHECK(!messages.empty() && messages[0].find("player 1") != std::string::npos);
}
