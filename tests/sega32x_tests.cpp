// 32X foundation: system registers, communication between the 68000 and the
// SH-2s, ROM banking, SDRAM and the SH-2 address map.

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "md/emulator.hpp"
#include "md/sega32x/sega32x.hpp"
#include "sh2_asm.hpp"
#include "test.hpp"

using md::s32;
using md::u16;
using md::u32;
using md::u8;
using namespace md::sega32x;
using md::sega32x::Pwm;

namespace {

// 68000 addresses.
constexpr u32 kAdapter = 0xA15100;
constexpr u32 kIntCtrl = 0xA15102;
constexpr u32 kBank    = 0xA15104;
constexpr u32 kDreqCtl = 0xA15106;
constexpr u32 kDreqLen = 0xA15110;
constexpr u32 kFifo    = 0xA15112;
constexpr u32 kComm68k = 0xA15120;

// SH-2 addresses (cache-through, as SH-2 code uses for shared registers).
constexpr u32 kIntMask = 0x20004000;
constexpr u32 kDreqSh2 = 0x20004006;
constexpr u32 kFifoSh2 = 0x20004012;
constexpr u32 kCmdClr  = 0x2000401A;
constexpr u32 kVClr    = 0x20004016;
constexpr u32 kCommSh2 = 0x20004020;

// A 4 MB cartridge where each 1 MB bank starts with its number.
std::vector<u8> banked_rom() {
    std::vector<u8> rom(0x400000, 0);
    for (u32 bank = 0; bank < 4; ++bank) {
        rom[bank * 0x100000] = static_cast<u8>(0xB0 + bank);
        rom[bank * 0x100000 + 1] = 0x5A;
    }
    return rom;
}

// 32X mode on, SH-2s running.
void enable(Sega32x& x) { x.write16(kAdapter, SystemRegisters::kAden | SystemRegisters::kRes); }

} // namespace

// ==============================================================================
// Adapter control
// ==============================================================================

TEST(sega32x_power_on_state) {
    Sega32x x;
    CHECK(x.read16(kAdapter) == SystemRegisters::kRen);   // ready, MD mode, SH-2s in reset
    CHECK(!x.registers().adapter_enabled());
    CHECK(x.registers().sh2_reset_asserted());
    CHECK(x.read16(0xA130EC) == 0x4D41 && x.read16(0xA130EE) == 0x5253);  // "MARS"
    CHECK(x.read8(0xA130EF) == 'S');
}

TEST(sega32x_adapter_control_and_sh2_reset) {
    Sega32x x;
    std::vector<bool> resets;
    x.registers().set_sh2_reset_callback([&](bool asserted) { resets.push_back(asserted); });

    x.write8(kAdapter + 1, SystemRegisters::kAden);                  // 32X mode, SH-2s still held
    CHECK(x.registers().adapter_enabled() && x.registers().sh2_reset_asserted());
    CHECK(resets.empty());
    x.write8(kAdapter + 1, SystemRegisters::kAden | SystemRegisters::kRes);
    CHECK(!x.registers().sh2_reset_asserted());
    CHECK(resets.size() == 1 && resets[0] == false);                 // released: SH-2s boot

    x.write8(kAdapter, 0x80);                                        // FM (high byte only)
    CHECK(x.read16(kAdapter) == (SystemRegisters::kFm | SystemRegisters::kRen | SystemRegisters::kRes | SystemRegisters::kAden));
    CHECK(x.registers().sh2_owns_vdp());

    // The SH-2s see FM and ADEN; FM is one bit shared by all three CPUs.
    auto& master = x.sh2_bus(Sh2::Master);
    CHECK((master.read16(kIntMask) & 0x8200) == 0x8200);
    master.write16(kIntMask, 0x0000);                                // SH-2 hands the VDP back
    CHECK((x.read16(kAdapter) & SystemRegisters::kFm) == 0);

    x.write16(kAdapter, SystemRegisters::kAden);                     // RES 1->0: back in reset
    CHECK(resets.size() == 2 && resets[1] == true);
}

TEST(sega32x_cart_bit) {
    Sega32x x;
    CHECK(x.sh2_bus(Sh2::Master).read16(kIntMask) & 0x0100);         // no cartridge
    const std::vector<u8> rom(0x1000, 0);
    x.set_cartridge(rom);
    CHECK((x.sh2_bus(Sh2::Master).read16(kIntMask) & 0x0100) == 0);  // 0 = inserted
}

// ==============================================================================
// Communication
// ==============================================================================

TEST(sega32x_comm_registers_shared_by_all_cpus) {
    Sega32x x;
    auto& master = x.sh2_bus(Sh2::Master);
    auto& slave = x.sh2_bus(Sh2::Slave);

    // 68000 -> SH-2s: a word in mailbox 0, seen by both, cached or not.
    x.write16(kComm68k, 0x1234);
    CHECK(master.read16(kCommSh2) == 0x1234);
    CHECK(slave.read16(kCommSh2 & 0x0FFFFFFF) == 0x1234);            // cached alias

    // SH-2 -> 68000: the slave answers in mailbox 7.
    slave.write16(kCommSh2 + 0x0E, 0xCAFE);
    CHECK(x.read16(kComm68k + 0x0E) == 0xCAFE);

    // Byte writes touch one byte; a long from the SH-2 fills two mailboxes.
    x.write8(kComm68k + 3, 0x77);
    CHECK(x.read16(kComm68k + 2) == 0x0077);
    master.write32(kCommSh2 + 4, 0xAAAABBBB);
    CHECK(x.read16(kComm68k + 4) == 0xAAAA && x.read16(kComm68k + 6) == 0xBBBB);
    CHECK(master.read8(kCommSh2 + 4) == 0xAA);
    for (int i = 0; i < 8; ++i) CHECK(x.registers().comm(i) == x.read16(kComm68k + 2u * static_cast<u32>(i)));
}

TEST(sega32x_cmd_interrupt_round_trip) {
    // The usual handshake: the 68000 posts a command and rings INTM; the
    // master SH-2 takes CMD interrupt (level 8), reads the mailbox, clears
    // the interrupt, and replies.
    Sega32x x;
    enable(x);
    auto& master = x.sh2_bus(Sh2::Master);
    auto& slave = x.sh2_bus(Sh2::Slave);
    std::vector<std::pair<Sh2, int>> irqs;
    x.registers().set_sh2_irq_callback([&](Sh2 cpu, int level) { irqs.emplace_back(cpu, level); });

    master.write16(kIntMask, 0x0002);                                // master: CMD enabled
    x.write16(kComm68k, 0x0042);
    x.write8(kIntCtrl + 1, 0x01);                                    // INTM
    CHECK(x.registers().sh2_irq_level(Sh2::Master) == 8);
    CHECK(x.registers().sh2_irq_level(Sh2::Slave) == 0);
    CHECK(irqs.size() == 1 && irqs[0].first == Sh2::Master && irqs[0].second == 8);

    CHECK(master.read16(kCommSh2) == 0x0042);
    master.write16(kCmdClr, 0);
    CHECK(x.registers().sh2_irq_level(Sh2::Master) == 0);
    CHECK(x.read16(kIntCtrl) == 0);                                  // the 68000 sees it was taken
    master.write16(kCommSh2, 0x8042);
    CHECK(x.read16(kComm68k) == 0x8042);

    // INTS: the slave's request waits for its mask, and the master's clear
    // doesn't touch it.
    x.write16(kIntCtrl, 0x0002);
    CHECK(x.registers().sh2_irq_level(Sh2::Slave) == 0);             // masked
    slave.write16(kIntMask, 0x0002);
    CHECK(x.registers().sh2_irq_level(Sh2::Slave) == 8);
    master.write16(kCmdClr, 0);
    CHECK(x.read16(kIntCtrl) == 0x0002);
    slave.write16(kCmdClr, 0);
    CHECK(x.read16(kIntCtrl) == 0);
}

TEST(sega32x_interrupt_priorities) {
    Sega32x x;
    auto& regs = x.registers();
    auto& master = x.sh2_bus(Sh2::Master);
    master.write16(kIntMask, 0x000F);                                // V, H, CMD, PWM
    regs.raise_interrupt(Sh2::Master, Sh2Interrupt::Pwm);
    CHECK(regs.sh2_irq_level(Sh2::Master) == 6);
    regs.raise_interrupt(Sh2::Master, Sh2Interrupt::V);
    CHECK(regs.sh2_irq_level(Sh2::Master) == 12);
    master.write16(kVClr, 0);
    CHECK(regs.sh2_irq_level(Sh2::Master) == 6);
    regs.raise_interrupt(Sh2::Master, Sh2Interrupt::Vres);           // unmaskable
    master.write16(kIntMask, 0x0000);
    CHECK(regs.sh2_irq_level(Sh2::Master) == 14);
    CHECK(regs.sh2_irq_level(Sh2::Slave) == 0);                      // per-CPU
}

TEST(sega32x_dreq_fifo) {
    Sega32x x;
    enable(x);
    auto& master = x.sh2_bus(Sh2::Master);
    x.write16(kFifo, 0x1111);                                        // 68S off: ignored
    CHECK(x.registers().fifo_size() == 0);

    x.write16(kDreqLen, 12);
    x.write16(kDreqCtl, SystemRegisters::k68s);
    CHECK(master.read16(kDreqSh2) == (0x4000 | SystemRegisters::k68s));  // empty
    for (u16 i = 0; i < 8; ++i) x.write16(kFifo, static_cast<u16>(0x100 + i));
    CHECK(x.read16(kDreqCtl) == (SystemRegisters::kFull | SystemRegisters::k68s));
    CHECK(master.read16(kFifoSh2) == 0x100);                         // first in, first out
    CHECK(master.read16(kFifoSh2) == 0x101);
    CHECK((x.read16(kDreqCtl) & SystemRegisters::kFull) == 0);
    x.write8(kFifo, 0xAB);                                           // byte pair
    x.write8(kFifo + 1, 0xCD);
    CHECK(x.registers().fifo_size() == 7);
    CHECK(x.registers().dreq_length() == 12 - 9);

    x.write16(kDreqCtl, 0);                                          // stop: FIFO emptied
    CHECK(x.registers().fifo_size() == 0);
    CHECK(master.read16(kDreqSh2) & 0x4000);
}

// ==============================================================================
// Memory map
// ==============================================================================

TEST(sega32x_rom_bank_switching) {
    Sega32x x;
    const auto rom = banked_rom();
    x.set_cartridge(rom);
    CHECK(x.read16(0x900000) == Sega32x::kOpenBusWord);              // MD mode: nothing there
    enable(x);
    CHECK(x.read16(0x880000) == 0xB05A);                             // fixed: ROM 0x000000
    CHECK(x.read16(0x900000) == 0xB05A);                             // bank 0
    x.write8(kBank + 1, 2);
    CHECK(x.read16(kBank) == 2);
    CHECK(x.read8(0x900000) == 0xB2);                                // ROM 0x200000
    x.write16(kBank, 3);
    CHECK(x.read16(0x900000) == 0xB35A);
    CHECK(x.read16(0x880000) == 0xB05A);                             // fixed window unaffected
    x.write16(0x900000, 0);                                          // ROM: read-only
    CHECK(x.read16(0x900000) == 0xB35A);

    // The SH-2s see the whole ROM at 0x02000000, unbanked.
    CHECK(x.sh2_bus(Sh2::Slave).read16(0x02300000) == 0xB35A);
    CHECK(x.sh2_bus(Sh2::Master).read8(0x22100000) == 0xB1);
}

TEST(sega32x_sdram_map) {
    Sega32x x;
    auto& master = x.sh2_bus(Sh2::Master);
    auto& slave = x.sh2_bus(Sh2::Slave);
    CHECK(Sdram::kSize == 256 * 1024);
    CHECK(Sega32x::decode_sh2(0x06000000) == Sega32x::Sh2Region::Sdram);
    CHECK(Sega32x::decode_sh2(0x2603FFFC) == Sega32x::Sh2Region::Sdram);
    CHECK(Sega32x::decode_sh2(0x20004000) == Sega32x::Sh2Region::Registers);
    CHECK(Sega32x::decode_sh2(0x00000100) == Sega32x::Sh2Region::BootRom);
    CHECK(Sega32x::decode_sh2(0x04000000) == Sega32x::Sh2Region::FrameBuffer);
    CHECK(Sega32x::decode_sh2(0xFFFFFE10) == Sega32x::Sh2Region::OnChip);

    master.write32(0x26000100, 0xDEADBEEF);                          // cache-through write
    CHECK(slave.read32(0x06000100) == 0xDEADBEEF);                   // shared, cached alias
    CHECK(slave.read16(0x06000102) == 0xBEEF);
    CHECK(slave.read8(0x06000101) == 0xAD);
    CHECK(x.sdram().read8(0x100) == 0xDE);                           // big-endian
    CHECK(master.read32(0x06040100) == 0xDEADBEEF);                  // mirrored every 256 KB
    slave.write8(0x0603FFFF, 0x99);
    CHECK(x.sdram().read8(Sdram::kSize - 1) == 0x99);
    slave.write16(0x06000200, 0x1234);
    CHECK(master.read8(0x26000200) == 0x12);

    x.reset();
    CHECK(master.read32(0x06000100) == 0);
}

TEST(sega32x_boot_roms_per_cpu) {
    Sega32x x;
    const std::vector<u8> m(Sega32x::kMasterBootRomSize, 0x11), s(Sega32x::kSlaveBootRomSize, 0x22);
    x.load_boot_roms(m, s);
    CHECK(x.sh2_bus(Sh2::Master).read16(0x00000000) == 0x1111);
    CHECK(x.sh2_bus(Sh2::Slave).read16(0x00000000) == 0x2222);
    x.sh2_bus(Sh2::Master).write16(0x00000000, 0);                   // read-only
    CHECK(x.sh2_bus(Sh2::Master).read16(0x00000000) == 0x1111);
}

TEST(sega32x_on_the_68000_bus) {
    md::Emulator emu;
    CHECK(emu.memory().read_word(0xA130EC) == md::Memory::kOpenBusWord);  // no 32X
    const auto rom = banked_rom();
    emu.load_rom(rom);
    auto& x = emu.attach_32x();
    auto& mem = emu.memory();
    CHECK(mem.read_word(0xA130EC) == 0x4D41);
    CHECK(mem.read_long(0xA130EC) == 0x4D415253);                    // "MARS"
    CHECK(mem.read_word(0x000000) == 0xB05A);                        // cartridge still at 0

    mem.write_word(kAdapter, SystemRegisters::kAden | SystemRegisters::kRes);
    CHECK(mem.read_word(0x880000) == 0xB05A);
    mem.write_byte(kBank + 1, 1);
    CHECK(mem.read_byte(0x900000) == 0xB1);
    mem.write_long(kComm68k, 0x12345678);
    CHECK(x.sh2_bus(Sh2::Master).read32(kCommSh2) == 0x12345678);
    CHECK(x.registers().comm(1) == 0x5678);

    emu.reset();                                                     // back to MD mode
    CHECK(!x.registers().adapter_enabled());
    CHECK(x.registers().comm(0) == 0);
    CHECK((x.sh2_bus(Sh2::Master).read16(kIntMask) & 0x0100) == 0);  // cartridge still seen
}

TEST(sega32x_68000_vector_page_in_32x_mode) {
    md::Emulator emu;
    const auto rom = banked_rom();
    emu.load_rom(rom);
    emu.attach_32x();
    auto& mem = emu.memory();
    CHECK(mem.read_word(0x000000) == 0xB05A);                        // MD mode: the cartridge

    mem.write_word(kAdapter, SystemRegisters::kAden | SystemRegisters::kRes);
    // Each vector points into the cartridge's jump table at 0x880200.
    CHECK(mem.read_long(0x000008) == 0x880206);                      // bus error: entry 1
    CHECK(mem.read_long(0x000078) == 0x880200 + 0x1D * 6);           // level 6 (V int)
    CHECK(mem.read_word(0x0000C8) == 0x1280);                        // RV helper routine
    CHECK(mem.read_word(0x000100) == 0x0000);                        // the page is 256 bytes
    mem.write_long(0x000008, 0);                                     // read-only...
    CHECK(mem.read_long(0x000008) == 0x880206);
    mem.write_long(0x000070, 0x00881234);                            // ...except the H vector
    CHECK(mem.read_long(0x000070) == 0x00881234);
    CHECK(mem.peek_word(0x000072) == 0x1234);                        // debuggers see it too

    mem.write_word(kDreqCtl, SystemRegisters::kRv);                  // RV: cartridge back at 0
    CHECK(mem.read_word(0x000000) == 0xB05A);
    mem.write_word(kDreqCtl, 0);
    CHECK(mem.read_word(0x000000) == 0x0000);                        // initial SSP: unused
}

// A cartridge with a 32X header (0x3C0): its SH-2 program (0x200 bytes at
// ROM 0x1000) goes to SDRAM 0x100. There, each CPU's entry point is a loop,
// and its vector table gives its stack pointer.
static std::vector<u8> cartridge_with_32x_header() {
    std::vector<u8> rom(0x2000, 0);
    const auto put32 = [&](u32 at, u32 v) {
        for (u32 i = 0; i < 4; ++i) rom[at + i] = static_cast<u8>(v >> (24 - 8 * i));
    };
    rom[0x18E] = 0xBE; rom[0x18F] = 0xEF;                            // header checksum
    const char* id = "MARS CHECK MODE ";
    for (u32 i = 0; i < 16; ++i) rom[0x3C0 + i] = static_cast<u8>(id[i]);
    put32(0x3D4, 0x1000);                                            // program in ROM
    put32(0x3D8, 0x0100);                                            // ... to SDRAM + 0x100
    put32(0x3DC, 0x0200);
    put32(0x3E0, 0x06000100);                                        // master entry
    put32(0x3E4, 0x06000104);                                        // slave entry
    put32(0x3E8, 0x06000180);                                        // master VBR
    put32(0x3EC, 0x06000200);                                        // slave VBR
    put32(0x1000, 0xAFFE0009);                                       // 0x06000100: bra $; nop
    put32(0x1004, 0xAFFE0009);                                       // 0x06000104: bra $; nop
    put32(0x1084, 0x0603FF00);                                       // master SP vector
    put32(0x1104, 0x0603F000);                                       // slave SP vector
    return rom;
}

TEST(sega32x_built_in_boot_without_bios) {
    const auto rom = cartridge_with_32x_header();
    Sega32x x;                                                       // no boot ROMs loaded
    x.set_cartridge(rom);
    enable(x);
    x.run_until(200);
    CHECK(x.sdram().read32(0x100) == 0xAFFE0009);                    // program loaded
    CHECK(x.sdram().read32(0x204) == 0x0603F000);
    CHECK(x.sdram().read32(0x000) == 0);                             // nothing outside it

    const auto& m = x.sh2(Sh2::Master).registers();
    const auto& s = x.sh2(Sh2::Slave).registers();
    CHECK(m.pc == 0x06000100 && s.pc == 0x06000104);                 // looping at their entries
    CHECK(m.vbr == 0x06000180 && s.vbr == 0x06000200);
    CHECK(m.r[15] == 0x0603FF00 && s.r[15] == 0x0603F000);
    CHECK(m.gbr == 0x20004000 && s.gbr == 0x20004000);               // system registers

    // What the 68000 start-up code waits for, as the BIOS leaves it.
    CHECK(x.read16(kComm68k) == 0x4D5F && x.read16(kComm68k + 2) == 0x4F4B);      // "M_OK"
    CHECK(x.read16(kComm68k + 4) == 0x535F && x.read16(kComm68k + 6) == 0x4F4B);  // "S_OK"
    CHECK(x.read16(kComm68k + 8) == 0xBEEF);                         // checksum in COMM4
}

// ==============================================================================
// Frame buffer and palette
// ==============================================================================

TEST(sega32x_frame_buffer_and_palette_follow_fm) {
    Sega32x x;
    enable(x);                                                       // FM = 0: the 68000's
    auto& sh2 = x.sh2_bus(Sh2::Master);
    x.write16(0x840000, 0x1234);
    CHECK(x.read16(0x840000) == 0x1234);
    CHECK(sh2.read16(0x24000000) == 0);                              // not the SH-2s' turn
    x.write16(0xA1520A, 0x7C00);                                     // palette entry 5
    CHECK(x.vdp().palette(5) == 0x7C00);

    x.write8(kAdapter, 0x80);                                        // FM = 1: the SH-2s'
    CHECK(sh2.read16(0x24000000) == 0x1234);                         // cache-through
    CHECK(sh2.read16(0x04000000) == 0x1234);                         // cached alias
    CHECK(x.read16(0x840000) == 0);
    x.write16(0x840000, 0xFFFF);                                     // ignored
    CHECK(sh2.read16(0x24000000) == 0x1234);
    CHECK(sh2.read16(0x20004200 + 10) == 0x7C00);
    sh2.write16(0x20004200 + 10, 0x001F);
    CHECK(x.vdp().palette(5) == 0x001F);

    // Overwrite image: zero bytes leave the frame buffer alone.
    sh2.write16(0x24020000, 0x00AB);
    CHECK(sh2.read16(0x24000000) == 0x12AB);
    sh2.write32(0x24020000, 0x56000000);
    CHECK(sh2.read32(0x24000000) == 0x56AB0000);
}

TEST(sega32x_frame_buffer_swap_at_vblank) {
    Sega32x x;
    enable(x);
    x.write8(kAdapter, 0x80);
    auto& sh2 = x.sh2_bus(Sh2::Master);
    sh2.write16(0x20004100, 1);                                      // packed pixel: displaying
    sh2.write16(0x24000100, 0xAAAA);                                 // into frame buffer 0
    sh2.write16(0x2000410A, 1);                                      // request FS = 1
    CHECK((sh2.read16(0x2000410A) & 1) == 0);                        // not before VBlank
    x.set_vblank(true);
    CHECK(sh2.read16(0x2000410A) == (Vdp32x::kVblk | Vdp32x::kPen | 1));
    sh2.write16(0x24000100, 0xBBBB);                                 // now into frame buffer 1
    CHECK(x.vdp().frame_buffer(0)[0x100] == 0xAA);
    CHECK(x.vdp().frame_buffer(1)[0x100] == 0xBB);
    CHECK(&x.vdp().displayed_frame_buffer() == &x.vdp().frame_buffer(0));
    sh2.write16(0x2000410A, 0);                                      // during VBlank: at once
    CHECK((sh2.read16(0x2000410A) & 1) == 0);

    // Blank mode (nothing displayed): at once too, so a program can set up
    // both buffers in a row while the screen is off.
    x.set_vblank(false);
    sh2.write16(0x20004100, 0);
    sh2.write16(0x2000410A, 1);
    CHECK((sh2.read16(0x2000410A) & 1) == 1);
    sh2.write16(0x24000000, 0x1234);                                 // line table of buffer 1
    sh2.write16(0x2000410A, 0);
    sh2.write16(0x24000000, 0x5678);                                 // and of buffer 0
    CHECK(x.vdp().frame_buffer(1)[0] == 0x12 && x.vdp().frame_buffer(0)[0] == 0x56);

    // Autofill: 4 words of 0x5555 from word 0x0FE, wrapping inside its line.
    sh2.write16(0x20004104, 3);
    sh2.write16(0x20004106, 0x00FE);
    sh2.write16(0x20004108, 0x5555);
    const auto& fb = x.vdp().frame_buffer(0);
    CHECK(fb[0x1FC] == 0x55 && fb[0x1FE] == 0x55 && fb[0x000] == 0x55 && fb[0x002] == 0x55);
    CHECK(fb[0x004] == 0x00);
    CHECK(sh2.read16(0x20004106) == 0x0002);
}

// ==============================================================================
// SH-2s running in the emulator
// ==============================================================================

namespace {

using namespace sh2asm;

constexpr u32 kSh2Code = 0x140;
constexpr u32 kSh2Pool = 0x300;

// A boot ROM image with its reset vectors.
Program boot_rom(u32 size, u32 stack) {
    Program p(size, kSh2Code, kSh2Pool);
    p.put32(0, kSh2Code);
    p.put32(4, stack);
    return p;
}

// Ends the program with an idle loop: sleep until an interrupt, again.
void idle(Program& p) {
    const u32 loop = p.pc;
    p << sleep();
    p.bra(loop);
    p << nop();
}

// A 68000 cartridge: the given instructions at 0x200, then a branch to self.
std::vector<u8> cartridge(const std::vector<u16>& code) {
    std::vector<u8> rom(0x1000, 0);
    const auto put16 = [&](u32 at, u16 v) { rom[at] = static_cast<u8>(v >> 8); rom[at + 1] = static_cast<u8>(v); };
    put16(0, 0x00FF); put16(2, 0x0000);                              // SSP
    put16(4, 0x0000); put16(6, 0x0200);                              // PC
    u32 at = 0x200;
    for (const u16 w : code) { put16(at, w); at += 2; }
    put16(at, 0x60FE);                                               // bra.s *
    return rom;
}

// move.w #value,(address).l
std::vector<u16> move_w(u16 value, u32 address) {
    return {0x33FC, value, static_cast<u16>(address >> 16), static_cast<u16>(address)};
}

std::vector<u16> concat(std::initializer_list<std::vector<u16>> parts) {
    std::vector<u16> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct System32x {
    md::Emulator emu;
    Sega32x* x = nullptr;
    std::vector<u8> rom;
    System32x(const Program& master, const Program& slave, std::vector<u8> cart) : rom(std::move(cart)) {
        emu.load_rom(rom);
        x = &emu.attach_32x();
        x->load_boot_roms(master.image, slave.image);
    }
};

} // namespace

TEST(sega32x_sh2s_boot_and_tell_master_from_slave) {
    // The same program in both boot ROMs: it reads BCR1's MASTER bit and
    // answers in mailbox 0 (master) or 2 (slave).
    const auto shared = [](u32 size, u32 stack) {
        Program p = boot_rom(size, stack);
        p.mov_l(1, md::Sh2::kBcr1);
        p << movl_ld(1, 0);
        p.mov_l(2, md::Sh2::kBcr1Slave);
        p << tst(2, 0);                                              // T = master
        const u32 branch = p.pc;
        p << nop();                                                  // patched below
        p.mov_l(3, kCommSh2);
        p.mov_l(4, 0x4D53);                                          // "MS"
        p << movw_st(4, 3);
        idle(p);
        const u32 slave_path = p.pc;
        p.mov_l(3, kCommSh2 + 4);
        p.mov_l(4, 0x534C);                                          // "SL"
        p << movw_st(4, 3);
        idle(p);
        const u32 end = p.pc;
        p.pc = branch;
        p.bf(slave_path);
        p.pc = end;
        return p;
    };
    System32x sys(shared(Sega32x::kMasterBootRomSize, 0x0603FF00), shared(Sega32x::kSlaveBootRomSize, 0x0603F000),
                  cartridge(move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter)));
    CHECK(sys.x->sh2(Sh2::Master).registers().pc == 0);              // still in reset
    sys.emu.run_frame();
    CHECK(sys.x->registers().comm(0) == 0x4D53);
    CHECK(sys.x->registers().comm(2) == 0x534C);
    CHECK(!sys.x->sh2(Sh2::Master).is_slave() && sys.x->sh2(Sh2::Slave).is_slave());
    CHECK(sys.x->sh2(Sh2::Master).registers().r[15] == 0x0603FF00); // each from its own vectors
    CHECK(sys.x->sh2(Sh2::Slave).registers().r[15] == 0x0603F000);
    CHECK(sys.x->sh2(Sh2::Master).sleeping() && sys.x->sh2(Sh2::Slave).sleeping());
}

TEST(sega32x_68000_commands_the_master_sh2) {
    // The 68000 posts 41 in mailbox 0 and raises INTM. The master SH-2 sleeps
    // with CMD enabled; its handler answers 42 in mailbox 1 and clears CMD.
    Program master = boot_rom(Sega32x::kMasterBootRomSize, 0x0603FF00);
    master.put32(md::Sh2::irl_vector(8) * 4, 0x200);
    master.mov_l(1, kIntMask);
    master << mov_imm(2, 0) << movw_st(0, 1);                        // CMD mask
    master << mov_imm(0, 0) << ldc_sr(0);                            // SR mask 0
    idle(master);
    master.pc = 0x200;
    master.mov_l(1, kCommSh2);
    master << movw_ld(1, 0) << add_imm(1, 0) << movw_st_disp(1, 1);
    master.mov_l(1, kCmdClr);
    master << movw_st(0, 1) << rte() << nop();
    Program slave = boot_rom(Sega32x::kSlaveBootRomSize, 0x0603F000);
    idle(slave);

    System32x sys(master, slave, cartridge(concat({
        move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter),
        move_w(41, kComm68k),
        move_w(1, kIntCtrl),
    })));
    sys.emu.run_frame();
    sys.emu.run_frame();
    CHECK(sys.x->registers().comm(1) == 42);
    CHECK(sys.emu.memory().read_word(kIntCtrl) == 0);                // taken and cleared
    CHECK(sys.x->registers().sh2_irq_level(Sh2::Master) == 0);
}

TEST(sega32x_vblank_interrupts_both_sh2s) {
    // Each SH-2 counts V interrupts in SDRAM (master at 0x26000000, slave
    // at +4), acknowledging each one.
    const auto counter = [](u32 size, u32 stack, u32 slot) {
        Program p = boot_rom(size, stack);
        p.put32(md::Sh2::irl_vector(12) * 4, 0x200);
        p.mov_l(1, kIntMask);
        p << mov_imm(8, 0) << movw_st(0, 1);                         // V mask
        p << mov_imm(0, 0) << ldc_sr(0);
        idle(p);
        p.pc = 0x200;
        p.mov_l(1, 0x26000000 + slot);
        p << movl_ld(1, 0) << add_imm(1, 0) << movl_st(0, 1);
        p.mov_l(1, kVClr);
        p << movw_st(0, 1) << rte() << nop();
        return p;
    };
    System32x sys(counter(Sega32x::kMasterBootRomSize, 0x0603FF00, 0), counter(Sega32x::kSlaveBootRomSize, 0x0603F000, 4),
                  cartridge(move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter)));
    for (int i = 0; i < 10; ++i) sys.emu.run_frame();
    const u32 master_count = sys.x->sdram().read32(0);
    const u32 slave_count = sys.x->sdram().read32(4);
    CHECK(master_count >= 9 && master_count <= 10);
    CHECK(slave_count == master_count);
}

TEST(sega32x_sh2s_run_at_three_times_the_68000) {
    Program master = boot_rom(Sega32x::kMasterBootRomSize, 0x0603FF00);
    const u32 loop = master.pc;
    master << add_imm(1, 5);
    master.bra(loop);
    master << nop();
    Program slave = boot_rom(Sega32x::kSlaveBootRomSize, 0x0603F000);
    idle(slave);
    System32x sys(master, slave, cartridge(move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter)));
    sys.emu.run_frame();
    // One NTSC frame: 262 lines x 3420 master cycles x 3/7.
    CHECK(sys.x->sh2_time() == 262LL * 3420 * 3 / 7);
    // The master's loop (ADD, then BRA and its slot: 4 cycles) ran for the
    // whole frame: the SH-2s start with the slice in which the 68000
    // releases them, and the last iteration may overshoot by one.
    const u32 iterations = sys.x->sh2(Sh2::Master).registers().r[5];
    CHECK(iterations >= 262 * 3420 * 3 / 7 / 4 - 10 && iterations <= 262 * 3420 * 3 / 7 / 4 + 1);
}

// ==============================================================================
// Interrupt controller and PWM
// ==============================================================================

TEST(sega32x_interrupt_levels_and_vectors) {
    // Source -> IRL level -> SH-2 auto-vector.
    CHECK(SystemRegisters::level(Sh2Interrupt::Vres) == 14 && md::Sh2::irl_vector(14) == 71);
    CHECK(SystemRegisters::level(Sh2Interrupt::V) == 12 && md::Sh2::irl_vector(12) == 70);
    CHECK(SystemRegisters::level(Sh2Interrupt::H) == 10 && md::Sh2::irl_vector(10) == 69);
    CHECK(SystemRegisters::level(Sh2Interrupt::Cmd) == 8 && md::Sh2::irl_vector(8) == 68);
    CHECK(SystemRegisters::level(Sh2Interrupt::Pwm) == 6 && md::Sh2::irl_vector(6) == 67);

    // The levels reach the CPUs' IRL pins as sources come and go.
    Sega32x x;
    auto& master = x.sh2_bus(Sh2::Master);
    master.write16(kIntMask, 0x000F);
    x.registers().raise_interrupt(Sh2::Master, Sh2Interrupt::H);
    CHECK(x.sh2(Sh2::Master).irl() == 10);
    x.registers().raise_interrupt(Sh2::Master, Sh2Interrupt::V);
    CHECK(x.sh2(Sh2::Master).irl() == 12);
    master.write16(kVClr, 0);
    CHECK(x.sh2(Sh2::Master).irl() == 10);
    master.write16(0x20004018, 0);                                   // H clear
    CHECK(x.sh2(Sh2::Master).irl() == 0);
    CHECK(x.sh2(Sh2::Slave).irl() == 0);
}

TEST(sega32x_pwm_fifos_and_flags) {
    Sega32x x;
    auto& sh2 = x.sh2_bus(Sh2::Master);
    auto& pwm = x.registers().pwm();
    CHECK(x.read16(0xA15134) == Pwm::kEmpty);                        // L: empty
    CHECK(x.read16(0xA15138) == Pwm::kEmpty);                        // mono: both empty

    x.write16(0xA15130, 0x0F05);                                     // 68000: routing only
    CHECK(x.read16(0xA15130) == 0x0005);
    sh2.write16(0x20004030, 0x0305);                                 // SH-2: TM = 3
    CHECK(pwm.timer_interval() == 3);
    sh2.write16(0x20004032, 1047);                                   // 23.01 MHz / 1046 = 22 kHz
    CHECK(pwm.sample_period() == 1046 && pwm.running());

    for (u16 i = 1; i <= 3; ++i) sh2.write16(0x20004038, static_cast<u16>(0x100 * i));   // mono
    CHECK(x.read16(0xA15134) == Pwm::kFull && x.read16(0xA15136) == Pwm::kFull);
    CHECK(sh2.read16(0x20004038) == Pwm::kFull);
    sh2.write16(0x20004034, 0x777);                                  // full: replaces the newest
    CHECK(pwm.fifo_size(0) == 3);

    pwm.advance(1045);
    CHECK(pwm.fifo_size(0) == 3);                                    // not yet a sample
    pwm.advance(1);
    CHECK(pwm.fifo_size(0) == 2 && pwm.output(0) == 0x100);
    CHECK(x.read16(0xA15134) == 0);                                  // neither full nor empty
    pwm.advance(2 * 1046);
    CHECK(pwm.output(0) == 0x777 && pwm.output(1) == 0x300);
    CHECK(x.read16(0xA15138) == Pwm::kEmpty);
    CHECK(pwm.samples() == 3);

    // Stopped (both channels off): nothing drains.
    sh2.write16(0x20004034, 1);
    sh2.write16(0x20004030, 0x0300);
    pwm.advance(10 * 1046);
    CHECK(pwm.fifo_size(0) == 1);
}

TEST(sega32x_pwm_timer_interrupt) {
    Sega32x x;
    auto& master = x.sh2_bus(Sh2::Master);
    master.write16(kIntMask, 0x0001);                                // master: PWM enabled
    master.write16(0x20004030, 0x0401);                              // TM = 4, left on
    master.write16(0x20004032, 101);                                 // 100 cycles per sample
    x.registers().pwm().advance(399);
    CHECK(x.sh2(Sh2::Master).irl() == 0);
    x.registers().pwm().advance(1);                                  // 4th sample
    CHECK(x.sh2(Sh2::Master).irl() == 6);
    CHECK(x.registers().interrupt_pending(Sh2::Slave, Sh2Interrupt::Pwm));  // raised for both
    CHECK(x.sh2(Sh2::Slave).irl() == 0);                             // masked on the slave
    master.write16(0x2000401C, 0);
    CHECK(x.sh2(Sh2::Master).irl() == 0);
}

TEST(sega32x_pwm_levels_and_routing) {
    Pwm pwm;
    pwm.write(0x02, 101, 0xFFFF, true);                              // 100 cycles per sample
    pwm.write(0x00, 0x0005, 0xFFFF, true);                           // L = left, R = right
    pwm.advance(100);
    CHECK(pwm.level()[0] == 0 && pwm.level()[1] == 0);               // never fed: silent
    pwm.write(0x04, 100, 0xFFFF, true);                              // L: full width
    pwm.write(0x06, 0, 0xFFFF, true);                                // R: zero width
    pwm.advance(100);
    CHECK(pwm.level()[0] == Pwm::kFullScale && pwm.level()[1] == -Pwm::kFullScale);
    pwm.write(0x08, 50, 0xFFFF, true);                               // mono: half = silence
    pwm.advance(100);
    CHECK(pwm.level()[0] == 0 && pwm.level()[1] == 0);
    pwm.write(0x04, 75, 0xFFFF, true);
    pwm.advance(100);
    CHECK(pwm.level()[0] == Pwm::kFullScale / 2 && pwm.level()[1] == 0);

    pwm.write(0x00, 0x000A, 0xFFFF, true);                           // swapped: L <- R, R <- L
    pwm.write(0x04, 100, 0xFFFF, true);
    pwm.write(0x06, 25, 0xFFFF, true);
    pwm.advance(100);
    CHECK(pwm.level()[0] == -Pwm::kFullScale / 2 && pwm.level()[1] == Pwm::kFullScale);
    pwm.write(0x00, 0x0004, 0xFFFF, true);                           // left off, R <- R (25)
    pwm.advance(100);
    CHECK(pwm.level()[0] == 0 && pwm.level()[1] == -Pwm::kFullScale / 2);
    pwm.write(0x00, 0x0000, 0xFFFF, true);                           // both off: silence
    pwm.advance(1);
    CHECK(pwm.level()[0] == 0 && pwm.level()[1] == 0);
}

TEST(sega32x_pwm_output_over_time) {
    Pwm pwm;
    s32 l = 0, r = 0;
    pwm.write(0x02, 101, 0xFFFF, true);
    pwm.write(0x00, 0x0005, 0xFFFF, true);
    pwm.write(0x08, 100, 0xFFFF, true);                              // sample at t = 100: full
    pwm.write(0x08, 0, 0xFFFF, true);                                // t = 200: zero
    pwm.advance(250);
    pwm.output_at(50, l, r);
    CHECK(l == 0 && r == 0);                                         // before the first sample
    pwm.output_at(100, l, r);
    CHECK(l == Pwm::kFullScale);
    pwm.output_at(150, l, r);
    CHECK(l == 0);                                                   // halfway: interpolated
    pwm.output_at(175, l, r);
    CHECK(l == -Pwm::kFullScale / 2);
    pwm.output_at(240, l, r);
    CHECK(l == -Pwm::kFullScale && r == -Pwm::kFullScale);           // past the newest: held
    pwm.advance(100);                                                // t = 300: FIFO empty, repeats
    pwm.output_at(320, l, r);
    CHECK(l == -Pwm::kFullScale);
}

TEST(sega32x_pwm_reaches_the_mixer) {
    // The 68000 can drive PWM by itself: rate, routing and a full-width
    // sample (a step up). The step reaches the mixed output.
    md::Emulator emu;
    const auto rom = cartridge(concat({
        move_w(SystemRegisters::kAden, kAdapter),                    // 32X mode, SH-2s in reset
        move_w(1047, 0xA15132),
        move_w(0x0005, 0xA15130),
        move_w(1046, 0xA15138),
    }));
    emu.load_rom(rom);
    auto& x = emu.attach_32x();
    emu.run_frame();
    CHECK(x.registers().pwm().level()[0] == Pwm::kFullScale);
    const auto out = emu.audio_samples();
    s32 peak = 0;
    for (std::size_t i = 0; i < out.size(); i += 2) peak = std::max<s32>(peak, out[i]);
    CHECK(peak > Pwm::kFullScale * 9 / 10);                          // before DC blocking pulls it back
    CHECK(std::abs(out[0]) < 100);                                   // silent before the write
}

TEST(sega32x_fifo_feeds_sh2_dma) {
    // DREQ0: the 68000 writes words into the FIFO; the master's DMA channel
    // 0, waiting for requests, moves them from 0x20004012 into SDRAM.
    Sega32x x;
    enable(x);
    x.run_until(100);                                                // SH-2s out of reset
    auto& m = x.sh2(Sh2::Master);
    m.write32(md::Sh2::kSar0, kFifoSh2);
    m.write32(md::Sh2::kDar0, 0x26001000);
    m.write32(md::Sh2::kTcr0, 12);
    m.write32(md::Sh2::kDmaor, md::Sh2::kDmaorDme);
    m.write32(md::Sh2::kChcr0, (1u << 14) | (1u << 10) | md::Sh2::kChcrDe);   // dest +, words, DREQ
    x.write16(kDreqLen, 12);
    x.write16(kDreqCtl, SystemRegisters::k68s);
    for (u16 i = 0; i < 12; ++i) x.write16(kFifo, static_cast<u16>(0x100 + i));  // more than 8: drained as written
    CHECK(x.registers().fifo_size() == 0);
    CHECK(x.sdram().read16(0x1000) == 0x100 && x.sdram().read16(0x1016) == 0x10B);
    CHECK((m.read32(md::Sh2::kChcr0) & md::Sh2::kChcrTe) != 0);
    CHECK((x.read16(kDreqCtl) & SystemRegisters::k68s) == 0);        // length reached: done
}

TEST(sega32x_pwm_timer_requests_dma) {
    // RTP: each timer event requests one DMA unit (DREQ1); the slave's
    // channel 1 copies samples from SDRAM into the mono register.
    Sega32x x;
    enable(x);
    x.run_until(100);
    for (u32 i = 0; i < 4; ++i) x.sdram().write16(0x2000 + i * 2, static_cast<u16>(0x200 + i));
    auto& s = x.sh2(Sh2::Slave);
    s.write32(md::Sh2::kSar1, 0x26002000);
    s.write32(md::Sh2::kDar1, 0x20004038);                           // mono
    s.write32(md::Sh2::kTcr1, 4);
    s.write32(md::Sh2::kDmaor, md::Sh2::kDmaorDme);
    s.write32(md::Sh2::kChcr1, (1u << 12) | (1u << 10) | md::Sh2::kChcrDe);   // source +, words, DREQ
    auto& sh2 = x.sh2_bus(Sh2::Slave);
    sh2.write16(0x20004032, 101);                                    // 100 cycles per sample
    sh2.write16(0x20004030, 0x0185);                                 // TM = 1, RTP, both channels
    auto& pwm = x.registers().pwm();
    pwm.advance(100);                                                // a timer event: one word in
    CHECK(pwm.fifo_size(0) == 1);
    pwm.advance(300);
    CHECK(pwm.output(0) == 0x202);                                   // played in order
    CHECK(s.read32(md::Sh2::kTcr1) == 0);
    sh2.write16(0x20004030, 0x0105);                                 // RTP off: no more requests
    pwm.advance(500);
    CHECK(pwm.output(0) == 0x203);
}

TEST(sega32x_hblank_status_bit) {
    Sega32x x;
    enable(x);
    x.write8(kAdapter, 0x80);                                        // FM = 1
    auto& sh2 = x.sh2_bus(Sh2::Master);
    CHECK((sh2.read16(0x2000410A) & Vdp32x::kHblk) == 0);
    x.set_hblank(true);
    CHECK(sh2.read16(0x2000410A) & Vdp32x::kHblk);
    x.set_hblank(false);
    CHECK((sh2.read16(0x2000410A) & Vdp32x::kHblk) == 0);
}

namespace {

// Master program: sets the H count and HEN, enables H, and counts H
// interrupts in SDRAM.
Program h_counter(u16 h_count, bool hen) {
    Program p = boot_rom(Sega32x::kMasterBootRomSize, 0x0603FF00);
    p.put32(md::Sh2::irl_vector(10) * 4, 0x200);
    p.mov_l(1, 0x20004004);
    p.mov_l(0, h_count);
    p << movw_st(0, 1);
    p.mov_l(1, kIntMask);
    p.mov_l(0, hen ? 0x0084u : 0x0004u);                             // HEN + H mask
    p << movw_st(0, 1);
    p << mov_imm(0, 0) << ldc_sr(0);
    idle(p);
    p.pc = 0x200;
    p.mov_l(1, 0x26000000);
    p << movl_ld(1, 0) << add_imm(1, 0) << movl_st(0, 1);
    p.mov_l(1, 0x20004018);
    p << movw_st(0, 1) << rte() << nop();
    return p;
}

u32 h_interrupts_per_frame(u16 h_count, bool hen) {
    Program slave = boot_rom(Sega32x::kSlaveBootRomSize, 0x0603F000);
    idle(slave);
    System32x sys(h_counter(h_count, hen), slave, cartridge(move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter)));
    sys.emu.run_frame();                                             // boot and set up
    sys.emu.run_frame();
    const u32 before = sys.x->sdram().read32(0);
    sys.emu.run_frame();
    return sys.x->sdram().read32(0) - before;
}

} // namespace

TEST(sega32x_h_interrupt_every_n_lines) {
    // H count 3: every 4th line. The counter reloads at VBlank.
    CHECK(h_interrupts_per_frame(3, false) == 224 / 4);              // displayed lines only
    CHECK(h_interrupts_per_frame(0, false) == 224);                  // every line
    CHECK(h_interrupts_per_frame(3, true) == 262 / 4);               // HEN: VBlank lines too
}

TEST(sega32x_sh2_feeding_pwm_does_not_stall) {
    // The usual loop: wait while the FIFO is FULL, write a sample, ten times.
    // It only finishes if the FIFO drains at the sample rate.
    Program master = boot_rom(Sega32x::kMasterBootRomSize, 0x0603FF00);
    master.mov_l(1, 0x20004030);
    master << mov_imm(0x05, 0) << movw_st(0, 1);                     // both channels on
    master.mov_l(0, 1047);
    master << movw_st_disp(1, 1);                                    // cycle (offset 2)
    master.mov_l(2, 0x20004038);
    master << mov_imm(10, 3);
    const u32 wait = master.pc;
    master << movw_ld_disp(4, 1) << cmp_pz(0);                       // mono status; T = not FULL
    master.bf(wait);
    master << movw_st(3, 2) << dt(3);
    master.bf(wait);
    master.mov_l(1, kCommSh2);
    master.mov_l(0, 0x600D);
    master << movw_st(0, 1);
    idle(master);
    Program slave = boot_rom(Sega32x::kSlaveBootRomSize, 0x0603F000);
    idle(slave);
    System32x sys(master, slave, cartridge(move_w(SystemRegisters::kAden | SystemRegisters::kRes, kAdapter)));
    sys.emu.run_frame();
    CHECK(sys.x->registers().comm(0) == 0x600D);
    CHECK(sys.x->registers().pwm().samples() >= 7);                  // 10 written, 3 fit at once
}
