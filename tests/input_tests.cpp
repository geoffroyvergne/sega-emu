// Controller ports: 3/6-button pad protocol through the 68000 I/O registers.

#include <vector>

#include "md/emulator.hpp"
#include "md/gamepad.hpp"
#include "md/memory.hpp"
#include "test.hpp"

using md::Button;
using md::ControllerState;
using md::Gamepad;
using md::s64;
using md::u8;

namespace {

constexpr md::u32 kDataA = 0xA10003;
constexpr md::u32 kCtrlA = 0xA10009;
constexpr md::u32 kDataB = 0xA10005;

ControllerState pressed(std::initializer_list<Button> buttons) {
    ControllerState s;
    for (Button b : buttons) s.set(b, true);
    return s;
}

// A memory bus with a pad on port A, and a controllable clock.
struct PadFixture {
    md::Memory mem;
    Gamepad pad;
    s64 time = 0;

    explicit PadFixture(Gamepad::Type type = Gamepad::Type::SixButton) : pad(type) {
        pad.set_time_source([this] { return time; });
        mem.attach_pad(0, &pad);
        mem.write_byte(kCtrlA, 0x40);   // TH is an output, as every game sets it
    }

    // What games do: write TH, then read the port (low 6 bits = buttons).
    u8 read_with_th(bool th) {
        mem.write_byte(kDataA, th ? 0x40 : 0x00);
        time += 100;                     // a few CPU cycles between accesses
        return mem.read_byte(kDataA) & 0x3F;
    }
};

} // namespace

TEST(pad_nothing_connected_reads_pulled_up) {
    md::Memory mem;
    mem.write_byte(kCtrlA, 0x40);
    mem.write_byte(kDataA, 0x00);
    CHECK((mem.read_byte(kDataA) & 0x3F) == 0x3F);
}

TEST(pad_three_button_protocol) {
    PadFixture f{Gamepad::Type::ThreeButton};
    CHECK(f.read_with_th(true)  == 0x3F);    // nothing pressed: all high
    CHECK(f.read_with_th(false) == 0x33);    // Start A 0 0 Down Up -> 11 0011

    f.pad.set_state(pressed({Button::Up, Button::Right, Button::B, Button::C}));
    CHECK(f.read_with_th(true) == 0b000110); // C B R L D U = 0 0 0 1 1 0

    f.pad.set_state(pressed({Button::Start, Button::A, Button::Down}));
    CHECK(f.read_with_th(false) == 0b000001); // S A 0 0 D U = 0 0 0 0 0 1
    CHECK(f.read_with_th(true)  == 0b111101); // Down also visible with TH=1
}

TEST(pad_th_bit_reads_back_latched_value) {
    PadFixture f;
    f.mem.write_byte(kDataA, 0x40);
    CHECK((f.mem.read_byte(kDataA) & 0x40) == 0x40);
    f.mem.write_byte(kDataA, 0x00);
    CHECK((f.mem.read_byte(kDataA) & 0x40) == 0x00);
}

TEST(pad_six_button_sequence) {
    PadFixture f;
    f.pad.set_state(pressed({Button::X, Button::Mode, Button::A, Button::Left}));
    f.time = 1'000'000;                       // long after reset: counter idle

    std::vector<u8> reads;
    for (int i = 0; i < 8; ++i) reads.push_back(f.read_with_th(i % 2 == 0));

    CHECK(reads[0] == 0b111011);  // TH=1: C B R L D U (Left pressed)
    CHECK(reads[1] == 0b100011);  // TH=0: S A 0 0 D U (A pressed)
    CHECK(reads[2] == 0b111011);
    CHECK(reads[3] == 0b100011);
    CHECK(reads[4] == 0b111011);
    CHECK(reads[5] == 0b100000);  // TH=0: S A 0 0 0 0  <- 6-button ID
    CHECK(reads[6] == 0b110011);  // TH=1: C B M X Y Z  (Mode, X pressed)
    CHECK(reads[7] == 0b101111);  // TH=0: S A 1 1 1 1
}

TEST(pad_six_button_counter_resets_after_timeout) {
    PadFixture f;
    f.pad.set_state(pressed({Button::Z}));
    f.time = 1'000'000;
    for (int i = 0; i < 6; ++i) (void)f.read_with_th(i % 2 == 0);  // up to phase 5

    // Next frame: well over 1.5 ms later, the sequence starts again.
    f.time += 1'000'000;
    CHECK(f.read_with_th(true)  == 0x3F);        // phase 0 (normal), not XYZ
    CHECK(f.read_with_th(false) == 0x33);        // phase 1
}

TEST(pad_three_button_ignores_extended_sequence) {
    PadFixture f{Gamepad::Type::ThreeButton};
    f.pad.set_state(pressed({Button::X}));
    for (int i = 0; i < 8; ++i) {
        const u8 v = f.read_with_th(i % 2 == 0);
        CHECK(v == (i % 2 == 0 ? 0x3F : 0x33));   // never reports X
    }
}

TEST(pad_th_as_input_is_pulled_high) {
    PadFixture f{Gamepad::Type::ThreeButton};
    f.pad.set_state(pressed({Button::Up}));
    f.mem.write_byte(kCtrlA, 0x00);              // TH becomes an input
    f.mem.write_byte(kDataA, 0x00);              // data latch ignored for TH
    CHECK((f.mem.read_byte(kDataA) & 0x7F) == 0x7E);  // TH high, Up pressed
}

TEST(emulator_routes_controller_to_ports) {
    md::Emulator emu;
    emu.set_pad_type(Gamepad::Type::ThreeButton);
    emu.set_controller(0, pressed({Button::Start}));
    emu.set_controller(1, pressed({Button::Right}));
    auto& mem = emu.memory();

    mem.write_byte(kCtrlA, 0x40);
    mem.write_byte(kDataA, 0x00);
    CHECK((mem.read_byte(kDataA) & 0x20) == 0);      // Start pressed on pad 1

    mem.write_byte(0xA1000B, 0x40);                  // ctrl B
    mem.write_byte(kDataB, 0x40);
    CHECK((mem.read_byte(kDataB) & 0x08) == 0);      // Right pressed on pad 2
}

// --- Full system: a 68000 program reads the pad like a game ------------------

TEST(system_68k_reads_pad) {
    // 68000 program: set TH as output, read TH=1 and TH=0 halves into RAM.
    std::vector<u8> rom(0x1000, 0);
    rom[0x01] = 0xFF; rom[0x02] = 0xFE;   // SSP 0x00FFFE00
    rom[0x06] = 0x02;                     // PC  0x00000200
    const std::vector<md::u16> code = {
        0x13FC, 0x0040, 0x00A1, 0x0009,   // move.b #$40,($A10009).l   ctrl A: TH out
        // loop:
        0x13FC, 0x0040, 0x00A1, 0x0003,   // move.b #$40,($A10003).l   TH = 1
        0x4E71, 0x4E71,                   // nop; nop (settle)
        0x13F9, 0x00A1, 0x0003, 0x00FF, 0x0000,  // move.b ($A10003).l,($FF0000).l
        0x13FC, 0x0000, 0x00A1, 0x0003,   // move.b #$00,($A10003).l   TH = 0
        0x4E71, 0x4E71,
        0x13F9, 0x00A1, 0x0003, 0x00FF, 0x0001,  // move.b ($A10003).l,($FF0001).l
        0x60D2,                           // bra.s loop (0x234 -> 0x208)
    };
    std::size_t pc = 0x200;
    for (auto w : code) { rom[pc++] = static_cast<u8>(w >> 8); rom[pc++] = static_cast<u8>(w); }

    md::Emulator emu;
    emu.load_rom(rom);
    // This loop toggles TH far faster than the 1.5 ms timeout, so a 6-button
    // pad would (correctly) cycle through its extended sequence.
    emu.set_pad_type(Gamepad::Type::ThreeButton);
    emu.set_controller(0, pressed({Button::Right, Button::A}));
    emu.run_frame();

    const u8 th_high = emu.memory().read_byte(0xFF0000) & 0x3F;
    const u8 th_low  = emu.memory().read_byte(0xFF0001) & 0x3F;
    CHECK(th_high == 0b110111);  // C B R L D U: Right pressed
    CHECK(th_low  == 0b100011);  // S A 0 0 D U: A pressed
}
