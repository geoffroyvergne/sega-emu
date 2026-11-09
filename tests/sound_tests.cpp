// Sound chips, Z80 and the full audio path.

#include <cmath>
#include <initializer_list>
#include <vector>

#include "md/emulator.hpp"
#include "md/sound.hpp"
#include "test.hpp"

using md::s16;
using md::s64;
using md::u16;
using md::u32;
using md::u8;

namespace {

constexpr s64 kOneSecond = 53'693'175;  // NTSC master cycles

// Sign changes of the left channel (2 per period of a periodic wave).
int zero_crossings(std::span<const s16> stereo) {
    int crossings = 0;
    bool positive = stereo.size() >= 2 && stereo[0] >= 0;
    for (std::size_t i = 0; i < stereo.size(); i += 2) {
        const bool p = stereo[i] >= 0;
        if (p != positive) { ++crossings; positive = p; }
    }
    return crossings;
}

double rms(std::span<const s16> stereo) {
    if (stereo.empty()) return 0;
    double sum = 0;
    for (std::size_t i = 0; i < stereo.size(); i += 2) sum += double(stereo[i]) * stereo[i];
    return std::sqrt(sum / double(stereo.size() / 2));
}

void ym_reg(md::Sound& s, u8 reg, u8 value, bool part2 = false) {
    s.ym().write(part2 ? 2 : 0, reg);
    s.ym().write(part2 ? 3 : 1, value);
}

} // namespace

// --- PSG ---------------------------------------------------------------------

TEST(psg_tone_frequency) {
    md::Sound sound;
    sound.psg().write(0x8E);  // channel 0 period low 4 bits = 0xE
    sound.psg().write(0x0F);  // high 6 bits -> period 0x0FE = 254
    sound.psg().write(0x90);  // channel 0 attenuation 0 (loudest)
    CHECK(sound.psg().tone_period(0) == 254);

    sound.sync(kOneSecond);
    // 3579545 / (32 * 254) = 440.4 Hz -> ~881 sign changes per second.
    const int crossings = zero_crossings(sound.samples());
    CHECK(crossings > 870 && crossings < 892);
    CHECK(sound.samples().size() / 2 == 53267);
}

TEST(psg_silent_at_full_attenuation) {
    md::Sound sound;
    sound.psg().write(0x8E);
    sound.psg().write(0x0F);   // tone set, but all volumes at 15 after reset
    sound.sync(kOneSecond / 10);
    CHECK(rms(sound.samples()) < 1.0);
}

TEST(psg_noise_and_latch_protocol) {
    md::Psg psg;
    psg.write(0xE4);           // noise: white, rate /512
    CHECK(psg.noise_control() == 4);
    psg.write(0xF0);           // noise attenuation 0
    CHECK(psg.attenuation(3) == 0);
    psg.write(0xB5);           // channel 1 attenuation 5
    psg.write(0x07);           // data byte goes to the latched volume register
    CHECK(psg.attenuation(1) == 7);

    // White noise is not periodic at short scales: many distinct transitions.
    int changes = 0;
    s16 prev = 0;
    for (int i = 0; i < 2000; ++i) {
        for (int t = 0; t < 4; ++t) psg.tick();
        const auto v = static_cast<s16>(psg.take_average());
        if ((v >= 0) != (prev >= 0)) ++changes;
        prev = v;
    }
    CHECK(changes > 50);
}

// --- YM2612 ------------------------------------------------------------------

namespace {

// Channel 1, algorithm 7, only operator 4 audible: a near-pure sine at 440 Hz.
void ym_setup_sine(md::Sound& s) {
    for (u8 op : std::initializer_list<u8>{0x00, 0x04, 0x08, 0x0C}) {
        ym_reg(s, static_cast<u8>(0x30 + op), 0x01);                  // DT=0, MUL=1
        ym_reg(s, static_cast<u8>(0x40 + op), op == 0x0C ? 0x00 : 0x7F);  // TL
        ym_reg(s, static_cast<u8>(0x50 + op), 0x1F);                  // AR=31
        ym_reg(s, static_cast<u8>(0x60 + op), 0x00);                  // D1R
        ym_reg(s, static_cast<u8>(0x70 + op), 0x00);                  // D2R
        ym_reg(s, static_cast<u8>(0x80 + op), 0x0F);                  // SL=0, RR=15
    }
    ym_reg(s, 0xB0, 0x07);   // feedback 0, algorithm 7
    ym_reg(s, 0xB4, 0xC0);   // left + right
    ym_reg(s, 0xA4, 0x24);   // block 4, fnum high = 4
    ym_reg(s, 0xA0, 0x3A);   // fnum = 0x43A -> ~440 Hz
}

} // namespace

TEST(ym2612_fm_tone_and_key_off) {
    md::Sound sound;
    ym_setup_sine(sound);
    ym_reg(sound, 0x28, 0xF0);                 // key on, channel 1, all operators
    sound.sync(kOneSecond / 5);

    const int crossings = zero_crossings(sound.samples());
    CHECK(crossings > 160 && crossings < 192); // ~176 for 440 Hz over 0.2 s
    CHECK(rms(sound.samples()) > 1000);

    ym_reg(sound, 0x28, 0x00);                 // key off: fast release
    sound.clear_samples();
    sound.sync(kOneSecond / 5 + kOneSecond / 10);
    sound.clear_samples();
    sound.sync(kOneSecond / 5 + kOneSecond / 5);
    CHECK(rms(sound.samples()) < 200);
}

TEST(ym2612_timers) {
    md::Sound sound;
    // Timer A = 1023 -> period (1024 - 1023) * 144 clocks = 1 sample.
    ym_reg(sound, 0x24, 0xFF);
    ym_reg(sound, 0x25, 0x03);
    ym_reg(sound, 0x27, 0x05);                 // load + enable flag A
    CHECK((sound.ym().read_status() & 0x01) == 0);
    sound.sync(2 * md::Sound::kMasterCyclesPerSample);
    CHECK((sound.ym().read_status() & 0x01) != 0);
    ym_reg(sound, 0x27, 0x15);                 // reset flag A, keep running
    CHECK((sound.ym().read_status() & 0x01) == 0);

    // Timer B = 255 -> period 16 * (256 - 255) * 144 clocks = 16 samples. Its
    // x16 prescaler is free-running, so the first period is shortened by the
    // prescaler's phase (here 2 samples, since the write happens at sample 2).
    ym_reg(sound, 0x26, 0xFF);
    ym_reg(sound, 0x27, 0x0A);                 // load + enable flag B (A stopped)
    sound.sync(15 * md::Sound::kMasterCyclesPerSample);
    CHECK((sound.ym().read_status() & 0x02) == 0);
    sound.sync(16 * md::Sound::kMasterCyclesPerSample);
    CHECK((sound.ym().read_status() & 0x02) != 0);
    // Later periods are full length.
    ym_reg(sound, 0x27, 0x2A);                 // reset flag B
    sound.sync(31 * md::Sound::kMasterCyclesPerSample);
    CHECK((sound.ym().read_status() & 0x02) == 0);
    sound.sync(32 * md::Sound::kMasterCyclesPerSample);
    CHECK((sound.ym().read_status() & 0x02) != 0);
}

TEST(ym2612_dac) {
    md::Sound sound;
    // Start with silence: the output's DC blocker treats a level present from
    // the very first sample as the baseline.
    sound.sync(10 * md::Sound::kMasterCyclesPerSample);
    ym_reg(sound, 0xB6, 0xC0, true);           // channel 6 panning: left + right
    ym_reg(sound, 0x2B, 0x80);                 // DAC enable
    ym_reg(sound, 0x2A, 0xFF);                 // max positive
    sound.sync(20 * md::Sound::kMasterCyclesPerSample);
    const s16 high = sound.samples()[2 * 12];
    ym_reg(sound, 0x2A, 0x00);                 // max negative
    sound.sync(30 * md::Sound::kMasterCyclesPerSample);
    const s16 low = sound.samples()[2 * 22];
    CHECK(high > 3000);
    CHECK(low < -3000);
}

// --- Z80 and the full system -------------------------------------------------

namespace {

// Minimal 68000 ROM (bra.s *) with room for data at 0x8000.
std::vector<u8> idle_rom() {
    std::vector<u8> rom(0x10000, 0);
    rom[0x01] = 0xFF; rom[0x02] = 0xFE;  // SSP = 0x00FFFE00
    rom[0x06] = 0x02;                    // PC  = 0x00000200
    rom[0x200] = 0x60; rom[0x201] = 0xFE;
    return rom;
}

// Loads a Z80 program the way games do: request the bus, copy into Z80 RAM,
// release reset and the bus.
void load_z80(md::Emulator& emu, std::initializer_list<u8> code, u16 at = 0) {
    auto& mem = emu.memory();
    mem.write_word(0xA11100, 0x0100);  // bus request
    mem.write_word(0xA11200, 0x0000);  // assert reset
    for (u8 b : code) mem.write_byte(0xA00000u + at++, b);
    mem.write_word(0xA11200, 0x0100);  // release reset
    mem.write_word(0xA11100, 0x0000);  // release bus
}

} // namespace

TEST(z80_runs_from_its_ram) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    load_z80(emu, {
        0x3E, 0x42,        // ld a,0x42
        0x32, 0x00, 0x10,  // ld (0x1000),a
        0x76,              // halt
    });
    emu.run_frame();
    CHECK(emu.memory().z80_ram()[0x1000] == 0x42);
}

TEST(z80_bank_window_reads_68k_rom) {
    md::Emulator emu;
    auto rom = idle_rom();
    rom[0x8010] = 0x5A;
    emu.load_rom(rom);
    load_z80(emu, {
        0x21, 0x00, 0x60,                          // ld hl,0x6000
        0x36, 0x01,                                // bank bit 0 = 1
        0x36, 0x00, 0x36, 0x00, 0x36, 0x00, 0x36, 0x00,
        0x36, 0x00, 0x36, 0x00, 0x36, 0x00, 0x36, 0x00,  // bits 1-8 = 0 -> bank 1
        0x3A, 0x10, 0x80,                          // ld a,(0x8010)  -> 68k 0x008010
        0x32, 0x01, 0x10,                          // ld (0x1001),a
        0x76,
    });
    emu.run_frame();
    CHECK(emu.memory().z80_bank_base() == 0x8000);
    CHECK(emu.memory().z80_ram()[0x1001] == 0x5A);
}

TEST(z80_writes_psg) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    load_z80(emu, {
        0x3E, 0x93,        // ld a,0x93  (channel 0 attenuation 3)
        0x32, 0x11, 0x7F,  // ld (0x7F11),a
        0x76,
    });
    emu.run_frame();
    CHECK(emu.sound().psg().attenuation(0) == 3);
}

TEST(z80_vblank_interrupt_once_per_frame) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    load_z80(emu, {
        0xF3,              // di
        0xED, 0x56,        // im 1
        0x21, 0x00, 0x1F,  // ld hl,0x1F00
        0xFB,              // ei
        0x18, 0xFE,        // jr $
    });
    load_z80(emu, {0x34, 0xFB, 0xED, 0x4D}, 0x38);  // inc (hl); ei; reti
    emu.run_frame();
    emu.run_frame();
    emu.run_frame();
    CHECK(emu.memory().z80_ram()[0x1F00] == 3);
}

TEST(z80_paused_while_68k_holds_the_bus) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    load_z80(emu, {
        0x21, 0x00, 0x10,  // ld hl,0x1000
        0x34,              // loop: inc (hl)
        0x18, 0xFD,        // jr loop
    });
    emu.run_frame();
    CHECK(emu.memory().z80_ram()[0x1000] != 0);

    emu.memory().write_word(0xA11100, 0x0100);  // take the bus
    const u8 before = emu.memory().z80_ram()[0x1000];
    emu.run_frame();
    CHECK(emu.memory().z80_ram()[0x1000] == before);
}

TEST(emulator_produces_one_frame_of_audio) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    emu.run_frame();
    const auto frames = emu.audio_samples().size() / 2;
    CHECK(frames == 888 || frames == 889);  // 896040 / 1008 = 888.9
    CHECK(std::abs(emu.audio_sample_rate() - 53267.0) < 1.0);
}

TEST(system_z80_plays_dac_square_wave) {
    md::Emulator emu;
    emu.load_rom(idle_rom());
    load_z80(emu, {
        0x3E, 0x2B, 0x32, 0x00, 0x40,  // YM addr = 0x2B
        0x3E, 0x80, 0x32, 0x01, 0x40,  // DAC enable
        0x3E, 0xB6, 0x32, 0x02, 0x40,  // YM part II addr = 0xB6
        0x3E, 0xC0, 0x32, 0x03, 0x40,  // channel 6 panning L+R
        0x3E, 0x2A, 0x32, 0x00, 0x40,  // YM addr = 0x2A (DAC data)
        // loop (0x19):
        0x3E, 0xFF, 0x32, 0x01, 0x40,  // DAC = 0xFF
        0x06, 0x40, 0x10, 0xFE,        // delay
        0x3E, 0x00, 0x32, 0x01, 0x40,  // DAC = 0x00
        0x06, 0x40, 0x10, 0xFE,        // delay
        0x18, 0xEC,                    // jr loop
    });
    emu.run_frame();
    emu.run_frame();
    // ~1.7k T-states per period -> ~2.1 kHz -> ~70 sign changes per frame.
    const int crossings = zero_crossings(emu.audio_samples());
    CHECK(crossings > 50 && crossings < 90);
    CHECK(rms(emu.audio_samples()) > 3000);
}
