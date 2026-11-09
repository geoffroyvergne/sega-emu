// End-to-end tests: Moira -> MoiraBridge -> Memory, using hand-assembled programs.

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "md/emulator.hpp"
#include "md/m68k.hpp"
#include "md/memory.hpp"
#include "test.hpp"

using md::u16;
using md::u32;
using md::u8;

namespace {

constexpr u32 kInitialSsp = 0x00FF'FE00;
constexpr u32 kEntry      = 0x0000'0200;

// Builds a ROM image, writing big-endian words/longs like an assembler would.
class RomBuilder {
public:
    RomBuilder() : rom_(0x1000, 0) {
        put_long(0x000, kInitialSsp);  // vector 0: initial SSP
        put_long(0x004, kEntry);       // vector 1: initial PC
    }

    RomBuilder& vector(int n, u32 handler) { put_long(static_cast<u32>(n) * 4, handler); return *this; }

    RomBuilder& code(u32 address, std::initializer_list<u16> words) {
        for (u16 w : words) { put_word(address, w); address += 2; }
        return *this;
    }

    [[nodiscard]] const std::vector<u8>& image() const { return rom_; }

private:
    void put_word(u32 a, u16 v) {
        rom_[a]     = static_cast<u8>(v >> 8);
        rom_[a + 1] = static_cast<u8>(v);
    }
    void put_long(u32 a, u32 v) {
        put_word(a,     static_cast<u16>(v >> 16));
        put_word(a + 2, static_cast<u16>(v));
    }

    std::vector<u8> rom_;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

TEST(cpu_reset_loads_vectors_from_rom) {
    md::Memory mem;
    mem.load_rom(RomBuilder{}.code(kEntry, {0x60FE}).image());
    md::M68k cpu{mem};
    cpu.reset();

    const auto r = cpu.registers();
    CHECK(r.pc == kEntry);
    CHECK(r.ssp == kInitialSsp);
    CHECK(r.a[7] == kInitialSsp);
    CHECK((r.sr & 0x2700) == 0x2700);  // supervisor mode, interrupts masked
}

TEST(cpu_reads_and_writes_through_memory) {
    md::Memory mem;
    mem.load_rom(RomBuilder{}.code(kEntry, {
        0x203C, 0x1234, 0x5678,          // move.l #$12345678,d0
        0x23C0, 0x00FF, 0x0000,          // move.l d0,($FF0000).l
        0x33FC, 0x0100, 0x00A1, 0x1100,  // move.w #$0100,($A11100).l   ; Z80 bus request
        0x13FC, 0x0042, 0x00A0, 0x0000,  // move.b #$42,($A00000).l     ; Z80 RAM
        0x60FE,                          // bra.s *
    }).image());
    md::M68k cpu{mem};
    cpu.reset();

    for (int i = 0; i < 4; ++i) CHECK(cpu.step() > 0);

    CHECK(cpu.registers().d[0] == 0x12345678);
    CHECK(mem.read_long(0xFF0000) == 0x12345678);
    CHECK(mem.main_ram()[0] == 0x12);               // stored big-endian
    CHECK(mem.z80_bus_requested());
    CHECK(mem.z80_ram()[0] == 0x42);
    CHECK(cpu.registers().pc == kEntry + 0x1C);     // at the bra.s
}

TEST(cpu_autovectored_interrupt_and_ack) {
    md::Memory mem;
    mem.load_rom(RomBuilder{}
        .vector(30, 0x300)                          // level 6 autovector (VINT)
        .code(kEntry, {
            0x46FC, 0x2000,                         // move #$2000,sr   ; unmask interrupts
            0x60FE,                                 // bra.s *
        })
        .code(0x300, {
            0x33FC, 0xBEEF, 0x00FF, 0x0010,         // move.w #$BEEF,($FF0010).l
            0x4E73,                                 // rte
        })
        .image());
    md::M68k cpu{mem};

    int acked_level = -1;
    cpu.set_interrupt_ack_handler([&](int level) {
        acked_level = level;
        cpu.set_irq_level(0);                       // what the VDP does on IACK
    });
    cpu.reset();
    cpu.step();                                     // move to sr
    cpu.step();                                     // spin once

    cpu.set_irq_level(6);
    cpu.run_until(cpu.cycles() + 200);

    CHECK(acked_level == 6);
    CHECK(mem.read_word(0xFF0010) == 0xBEEF);
    CHECK(cpu.irq_level() == 0);
    CHECK(cpu.registers().pc == kEntry + 4);        // returned to the loop
    CHECK(cpu.registers().a[7] == kInitialSsp);     // stack balanced after rte
}

TEST(cpu_raises_address_error_on_odd_word_access) {
    // Also verifies our MoiraConfig.h override is in effect
    // (MOIRA_EMULATE_ADDRESS_ERROR is false upstream).
    md::Memory mem;
    mem.load_rom(RomBuilder{}
        .vector(3, 0x400)                           // address error
        .code(kEntry, {
            0x3039, 0x00FF, 0x0001,                 // move.w ($FF0001).l,d0
            0x60FE,
        })
        .code(0x400, {
            0x33FC, 0xDEAD, 0x00FF, 0x0020,         // move.w #$DEAD,($FF0020).l
            0x60FE,
        })
        .image());
    md::M68k cpu{mem};
    cpu.reset();
    cpu.run_until(cpu.cycles() + 300);

    CHECK(mem.read_word(0xFF0020) == 0xDEAD);
    CHECK(cpu.registers().pc == 0x408);
}

TEST(cpu_disassembles_without_side_effects) {
    md::Memory mem;
    mem.load_rom(RomBuilder{}.code(kEntry, {0x203C, 0x1234, 0x5678}).image());
    md::M68k cpu{mem};

    int length = 0;
    const auto text = lower(cpu.disassemble(kEntry, &length));
    CHECK(text.find("move") != std::string::npos);
    CHECK(text.find("12345678") != std::string::npos);
    CHECK(length == 6);
}

TEST(emulator_runs_one_frame_of_cpu_cycles) {
    md::Emulator emu{md::Region::NTSC};
    emu.load_rom(RomBuilder{}.code(kEntry, {0x60FE}).image());

    const auto start = emu.cpu().cycles();
    emu.run_frame();
    const auto elapsed = emu.cpu().cycles() - start;

    // 3420 * 262 / 7 = 128005.7 CPU cycles per NTSC frame (bra.s = 10 cycles).
    CHECK(elapsed >= 128000);
    CHECK(elapsed <  128020);
}
