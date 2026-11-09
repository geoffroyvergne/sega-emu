#include "input.hpp"

#include "pad_mapping.hpp"

namespace frontend {

namespace {

PadSnapshot snapshot(SDL_Gamepad* pad) {
    const auto button = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const auto axis = [pad](SDL_GamepadAxis a) {
        return static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f;
    };

    PadSnapshot p;
    p.south          = button(SDL_GAMEPAD_BUTTON_SOUTH);
    p.east           = button(SDL_GAMEPAD_BUTTON_EAST);
    p.west           = button(SDL_GAMEPAD_BUTTON_WEST);
    p.north          = button(SDL_GAMEPAD_BUTTON_NORTH);
    p.left_shoulder  = button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    p.right_shoulder = button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    p.back           = button(SDL_GAMEPAD_BUTTON_BACK);
    p.start          = button(SDL_GAMEPAD_BUTTON_START);
    p.dpad_up        = button(SDL_GAMEPAD_BUTTON_DPAD_UP);
    p.dpad_down      = button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    p.dpad_left      = button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    p.dpad_right     = button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    p.left_x         = axis(SDL_GAMEPAD_AXIS_LEFTX);
    p.left_y         = axis(SDL_GAMEPAD_AXIS_LEFTY);
    return p;
}

} // namespace

InputManager::InputManager() {
    // Community mappings for unusual pads, if the user drops the file next to
    // the executable (https://github.com/mdqinc/SDL_GameControllerDB).
    SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
}

InputManager::~InputManager() {
    for (auto& slot : slots_) {
        if (slot.pad) SDL_CloseGamepad(slot.pad);
    }
}

std::string InputManager::handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:   return add(event.gdevice.which);
    case SDL_EVENT_GAMEPAD_REMOVED: return remove(event.gdevice.which);
    default:                        return {};
    }
}

std::string InputManager::add(SDL_JoystickID id) {
    for (const auto& slot : slots_) {
        if (slot.pad && slot.id == id) return {};  // already open
    }
    for (std::size_t player = 0; player < slots_.size(); ++player) {
        auto& slot = slots_[player];
        if (slot.pad) continue;
        slot.pad = SDL_OpenGamepad(id);
        if (!slot.pad) return std::string("Could not open gamepad: ") + SDL_GetError();
        slot.id = id;
        const char* name = SDL_GetGamepadName(slot.pad);
        return "Gamepad connected: " + std::string(name ? name : "unknown") +
               " -> player " + std::to_string(player + 1);
    }
    return "Gamepad ignored (both players already have one)";
}

std::string InputManager::remove(SDL_JoystickID id) {
    for (std::size_t player = 0; player < slots_.size(); ++player) {
        auto& slot = slots_[player];
        if (slot.pad && slot.id == id) {
            SDL_CloseGamepad(slot.pad);
            slot = {};
            return "Gamepad disconnected (player " + std::to_string(player + 1) + ")";
        }
    }
    return {};
}

md::ControllerState InputManager::state(int player) const {
    md::ControllerState s;
    if (player < 0 || player >= kPlayers) return s;

    const auto& slot = slots_[static_cast<std::size_t>(player)];
    if (slot.pad) s = map_pad(snapshot(slot.pad));
    if (player == 0) s = merge(s, read_keyboard());
    return s;
}

int InputManager::connected_gamepads() const {
    int n = 0;
    for (const auto& slot : slots_) n += slot.pad ? 1 : 0;
    return n;
}

md::ControllerState read_keyboard() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    md::ControllerState pad;
    if (!keys) return pad;
    pad.set(md::Button::Up,    keys[SDL_SCANCODE_UP]);
    pad.set(md::Button::Down,  keys[SDL_SCANCODE_DOWN]);
    pad.set(md::Button::Left,  keys[SDL_SCANCODE_LEFT]);
    pad.set(md::Button::Right, keys[SDL_SCANCODE_RIGHT]);
    pad.set(md::Button::A,     keys[SDL_SCANCODE_A]);
    pad.set(md::Button::B,     keys[SDL_SCANCODE_S]);
    pad.set(md::Button::C,     keys[SDL_SCANCODE_D]);
    pad.set(md::Button::X,     keys[SDL_SCANCODE_Q]);
    pad.set(md::Button::Y,     keys[SDL_SCANCODE_W]);
    pad.set(md::Button::Z,     keys[SDL_SCANCODE_E]);
    // Alt+Enter toggles fullscreen, so it must not also press Start.
    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
    pad.set(md::Button::Start, keys[SDL_SCANCODE_RETURN] && !alt);
    pad.set(md::Button::Mode,  keys[SDL_SCANCODE_RSHIFT]);
    return pad;
}

} // namespace frontend
