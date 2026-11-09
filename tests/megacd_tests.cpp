// Mega-CD foundation: gate array registers, PRG-RAM, Word RAM, sub CPU.

#include <initializer_list>
#include <vector>

#include "md/m68k.hpp"
#include "md/megacd/megacd.hpp"
#include "md/memory.hpp"
#include "test.hpp"

using md::u16;
using md::u32;
using md::u8;
using md::megacd::MegaCd;
using md::megacd::WordRam;

namespace {

// Cartridge-boot layout: expansion base 0x400000.
constexpr u32 kBios     = 0x400000;
constexpr u32 kPrgWin   = 0x420000;
constexpr u32 kWordRam  = 0x600000;

// Main CPU gate array registers.
constexpr u32 kMainCtrl  = 0xA12000;
constexpr u32 kMainMode  = 0xA12002;
constexpr u32 kMainFlags = 0xA1200E;
constexpr u32 kMainCmd   = 0xA12010;
constexpr u32 kMainStat  = 0xA12020;

// Sub CPU addresses.
constexpr u32 kSubWordRam2M = 0x080000;
constexpr u32 kSubWordRam1M = 0x0C0000;
constexpr u32 kSubMode   = 0xFF8002;
constexpr u32 kSubFlags  = 0xFF800E;
constexpr u32 kSubCmd    = 0xFF8010;
constexpr u32 kSubStat   = 0xFF8020;
constexpr u32 kSubIntMask = 0xFF8032;

// Main CPU control: SRES (bit 0) = run, SBRQ (bit 1) = bus request.
void sub_run(MegaCd& cd)     { cd.write8(kMainCtrl + 1, 0x01); }
void sub_halt(MegaCd& cd)    { cd.write8(kMainCtrl + 1, 0x03); }

} // namespace

// --- Communication registers ---------------------------------------------------

TEST(megacd_comm_flags_ownership) {
    MegaCd cd;
    auto& sub = cd.sub_bus();

    cd.write8(kMainFlags, 0xA5);          // main flags: main R/W
    cd.write8(kMainFlags + 1, 0x11);      // sub flags: read-only for main -> ignored
    CHECK(cd.read16(kMainFlags) == 0xA500);
    CHECK(sub.read_byte(kSubFlags) == 0xA5);

    sub.write_byte(kSubFlags + 1, 0x3C);  // sub flags: sub R/W
    sub.write_byte(kSubFlags, 0xFF);      // main flags: read-only for sub -> ignored
    CHECK(sub.read_word(kSubFlags) == 0xA53C);
    CHECK(cd.read8(kMainFlags + 1) == 0x3C);

    // Word writes only affect the writer's own byte.
    cd.write16(kMainFlags, 0x1234);
    sub.write_word(kSubFlags, 0x5678);
    CHECK(cd.read16(kMainFlags) == 0x1278);
}

TEST(megacd_command_and_status_registers) {
    MegaCd cd;
    auto& sub = cd.sub_bus();

    for (u16 i = 0; i < 8; ++i) cd.write16(kMainCmd + i * 2u, static_cast<u16>(0x1000 + i));
    for (u16 i = 0; i < 8; ++i) CHECK(sub.read_word(kSubCmd + i * 2u) == 0x1000 + i);
    sub.write_word(kSubCmd, 0xDEAD);                  // COMCMD is read-only for the sub CPU
    CHECK(cd.read16(kMainCmd) == 0x1000);

    sub.write_word(kSubStat + 14, 0xBEEF);
    sub.write_byte(kSubStat + 1, 0x42);               // byte write: low byte only
    CHECK(cd.read16(kMainStat + 14) == 0xBEEF);
    CHECK(cd.read16(kMainStat) == 0x0042);
    cd.write16(kMainStat, 0xFFFF);                    // COMSTAT is read-only for the main CPU
    CHECK(sub.read_word(kSubStat) == 0x0042);
}

// --- Reset / bus request --------------------------------------------------------

TEST(megacd_reset_and_bus_request) {
    MegaCd cd;
    CHECK(cd.registers().sub_reset_asserted());       // power-on: held in reset
    CHECK((cd.read16(kMainCtrl) & 0x0003) == 0x0000);

    sub_run(cd);
    CHECK(!cd.registers().sub_reset_asserted());
    CHECK(!cd.registers().sub_halted());
    CHECK((cd.read16(kMainCtrl) & 0x0003) == 0x0001);

    sub_halt(cd);
    CHECK(cd.registers().sub_bus_requested());
    CHECK(cd.registers().sub_halted());
    CHECK((cd.read16(kMainCtrl) & 0x0003) == 0x0003);

    // Writing IFL2 (high byte) must not disturb SRES/SBRQ (low byte).
    cd.write8(kMainCtrl, 0x01);
    CHECK((cd.read16(kMainCtrl) & 0x0003) == 0x0003);
}

// --- PRG-RAM ----------------------------------------------------------------------

TEST(megacd_prg_ram_window_needs_halted_sub_cpu) {
    MegaCd cd;
    sub_run(cd);                                      // sub CPU running
    cd.write16(kPrgWin, 0x1234);
    CHECK(cd.program_ram().sub_read16(0) == 0x0000);  // write ignored
    CHECK(cd.read16(kPrgWin) == 0xFFFF);              // open bus

    sub_halt(cd);
    cd.write16(kPrgWin, 0x1234);
    CHECK(cd.read16(kPrgWin) == 0x1234);
    CHECK(cd.program_ram().sub_read16(0) == 0x1234);
}

TEST(megacd_prg_ram_bank_select) {
    MegaCd cd;                                        // sub in reset: window usable
    cd.write8(kMainMode + 1, 0x80);                   // BK = 2
    CHECK(cd.program_ram().main_bank() == 2);
    cd.write16(kPrgWin + 0x10, 0xCAFE);
    CHECK(cd.program_ram().sub_read16(2 * 0x20000 + 0x10) == 0xCAFE);
    CHECK((cd.read16(kMainMode) & 0x00C0) == 0x0080);
}

TEST(megacd_prg_ram_write_protect) {
    MegaCd cd;
    auto& sub = cd.sub_bus();
    cd.write8(kMainMode, 0x02);                       // WP = 2 -> first 0x400 bytes protected
    CHECK(sub.read_word(kSubMode) >> 8 == 0x02);      // sub CPU can read WP

    sub.write_word(0x0003FE, 0x1111);                 // protected
    sub.write_byte(0x000000, 0x22);                   // protected
    sub.write_word(0x000400, 0x3333);                 // first unprotected word
    CHECK(sub.read_word(0x0003FE) == 0x0000);
    CHECK(sub.read_byte(0x000000) == 0x00);
    CHECK(sub.read_word(0x000400) == 0x3333);

    sub.write_word(kSubMode, 0x0000);                 // WP is read-only for the sub CPU
    CHECK(cd.program_ram().write_protect() == 0x02);

    // The main CPU's window is not write-protected.
    cd.write16(kPrgWin, 0x4444);
    CHECK(sub.read_word(0x000000) == 0x4444);
}

// --- Word RAM: 2M mode ---------------------------------------------------------------

TEST(megacd_word_ram_2m_ownership_handover) {
    MegaCd cd;
    auto& sub = cd.sub_bus();
    CHECK(cd.word_ram().mode() == WordRam::Mode::TwoMeg);
    CHECK((cd.read16(kMainMode) & 0x0007) == 0x0001);  // 2M, DMNA=0, RET=1: main owns

    cd.write16(kWordRam + 0x100, 0xABCD);
    CHECK(cd.read16(kWordRam + 0x100) == 0xABCD);
    CHECK(sub.read_word(kSubWordRam2M + 0x100) == 0xFFFF);  // sub can't see it

    cd.write8(kMainMode + 1, 0x02);                   // main: DMNA = 1 -> give to sub
    CHECK((cd.read16(kMainMode) & 0x0003) == 0x0002); // DMNA=1, RET=0
    CHECK(sub.read_word(kSubWordRam2M + 0x100) == 0xABCD);
    CHECK(cd.read16(kWordRam + 0x100) == 0xFFFF);     // main lost access
    sub.write_word(kSubWordRam2M + 0x3FFFE, 0x5A5A);  // last word of 256 KB

    sub.write_word(kSubMode, 0x0000);                 // sub: RET = 0 -> no effect in 2M
    CHECK(cd.word_ram().sub_owns());

    sub.write_word(kSubMode, 0x0001);                 // sub: RET = 1 -> give back
    CHECK((cd.read16(kMainMode) & 0x0003) == 0x0001);
    CHECK(cd.read16(kWordRam + 0x3FFFE) == 0x5A5A);
}

TEST(megacd_word_ram_2m_interleaves_banks) {
    MegaCd cd;
    cd.write16(kWordRam + 0, 0x1111);                 // word 0 -> bank 0, offset 0
    cd.write16(kWordRam + 2, 0x2222);                 // word 1 -> bank 1, offset 0
    cd.write16(kWordRam + 4, 0x3333);                 // word 2 -> bank 0, offset 2
    CHECK(cd.word_ram().bank_byte(0, 0) == 0x11);
    CHECK(cd.word_ram().bank_byte(1, 0) == 0x22);
    CHECK(cd.word_ram().bank_byte(0, 2) == 0x33);
}

// --- Word RAM: 1M mode ---------------------------------------------------------------

TEST(megacd_word_ram_1m_banks_and_swap) {
    MegaCd cd;
    auto& sub = cd.sub_bus();

    sub.write_word(kSubMode, 0x0005);                 // MODE = 1M, RET = 1
    CHECK(cd.word_ram().mode() == WordRam::Mode::OneMeg);
    CHECK((cd.read16(kMainMode) & 0x0004) == 0x0004); // main sees MODE
    CHECK(cd.word_ram().main_bank() == 1);

    // Both CPUs work at the same time, each in its own bank.
    cd.write16(kWordRam + 0x10, 0xAAAA);              // main -> bank 1
    sub.write_word(kSubWordRam1M + 0x10, 0xBBBB);     // sub  -> bank 0
    CHECK(cd.word_ram().bank_byte(1, 0x10) == 0xAA);
    CHECK(cd.word_ram().bank_byte(0, 0x10) == 0xBB);
    cd.write16(kWordRam + 0x1FFFE, 0x7777);           // last word of the main's 128 KB bank
    CHECK(cd.word_ram().bank_byte(1, 0x1FFFE) == 0x77);
    CHECK(cd.read16(kWordRam + 0x20000 + 4) == cd.read16(kWordRam + 0x100));  // cell view: line 1 of cell 0

    // Ping-pong: the sub performs a swap by writing RET, which clears DMNA.
    // 1M quirk (as in Genesis Plus GX): a main write with DMNA = 0 sets the
    // visible DMNA bit; a write with DMNA = 1 leaves it unchanged.
    cd.write8(kMainMode + 1, 0x02);
    CHECK(!cd.word_ram().dmna());
    cd.write8(kMainMode + 1, 0x00);
    CHECK(cd.word_ram().dmna());
    sub.write_word(kSubMode, 0x0004);                 // 1M, RET = 0
    CHECK(!cd.word_ram().dmna());                     // swap done
    CHECK(cd.read16(kWordRam + 0x10) == 0xBBBB);      // main now sees the sub's data
    CHECK(sub.read_word(kSubWordRam1M + 0x10) == 0xAAAA);
}

namespace {

// Independent description of the cell image: the 128 KB bank is a bitmap
// 256 bytes (64 cells of 4 bytes) wide; the view lists 8x8 cells (32 bytes
// each: 8 lines of 4 bytes) column by column, in five zones of 32, 16, 8, 4
// and 4 cells of height.
u32 cell_reference(u32 view) {
    struct Zone { u32 start, height_cells; };
    constexpr Zone zones[] = {{0x00000, 32}, {0x10000, 16}, {0x18000, 8}, {0x1C000, 4}, {0x1E000, 4}};
    Zone z = zones[0];
    for (const auto& each : zones) if (view >= each.start) z = each;
    const u32 rel = view - z.start;
    const u32 cell = rel / 32, line = (rel / 4) % 8, byte = rel % 4;
    const u32 row = cell % z.height_cells, column = cell / z.height_cells;
    return z.start + (row * 8 + line) * 256 + column * 4 + byte;
}

} // namespace

TEST(megacd_word_ram_cell_image_layout) {
    // Every byte of the view maps to a distinct byte of the bank, as described.
    std::vector<bool> seen(WordRam::kBankSize, false);
    bool matches = true, bijective = true;
    for (u32 v = 0; v < WordRam::kBankSize; ++v) {
        const u32 o = WordRam::cell_offset(v);
        matches &= o == cell_reference(v);
        bijective &= o < WordRam::kBankSize && !seen[o];
        if (o < WordRam::kBankSize) seen[o] = true;
    }
    CHECK(matches);
    CHECK(bijective);
}

TEST(megacd_word_ram_cell_image_access) {
    MegaCd cd;
    cd.sub_bus().write_word(kSubMode, 0x0005);                // 1M, RET = 1: main has bank 1
    // The main CPU draws a bitmap line through the linear view...
    cd.write16(kWordRam + 3 * 256 + 2 * 4, 0x1234);           // bitmap line 3, cell column 2
    cd.write16(kWordRam + 3 * 256 + 2 * 4 + 2, 0x5678);
    // ...and reads it back as cell (column 2, row 0), line 3 (column 2 = cells 64-95).
    const u32 cell_view = 0x20000 + (2 * 32) * 32 + 3 * 4;
    CHECK(cd.read16(kWordRam + cell_view) == 0x1234);
    CHECK(cd.read16(kWordRam + cell_view + 2) == 0x5678);
    CHECK(cd.read8(kWordRam + cell_view + 1) == 0x34);
    cd.write8(kWordRam + cell_view + 3, 0x99);                // writes go through too
    CHECK(cd.word_ram().bank_byte(1, 3 * 256 + 2 * 4 + 3) == 0x99);
}

TEST(megacd_word_ram_dot_image) {
    MegaCd cd;
    auto& sub = cd.sub_bus();
    constexpr u32 kDot = 0x080000;
    sub.write_word(kSubMode, 0x0005);                         // 1M, RET = 1: sub has bank 0

    // One pixel per byte address: even = high nibble, odd = low nibble.
    sub.write_byte(kDot + 0x10, 0x0A);
    sub.write_byte(kDot + 0x11, 0x0B);
    CHECK(cd.word_ram().bank_byte(0, 0x08) == 0xAB);
    CHECK(sub.read_byte(kDot + 0x10) == 0x0A && sub.read_byte(kDot + 0x11) == 0x0B);
    CHECK(sub.read_word(kDot + 0x10) == 0x0A0B);              // both pixels of the byte
    CHECK(sub.read_byte(kSubWordRam1M + 0x08) == 0xAB);        // same bank, linear view
    sub.write_word(kDot + 0x20, 0x0C0D);                      // word: one byte, two pixels
    CHECK(cd.word_ram().bank_byte(0, 0x10) == 0xCD);

    // Priority modes (PM, bits 4-3), with 0 as the empty pixel.
    sub.write_word(kDot + 0x30, 0x0300);                      // byte 0x18 = 0x30
    sub.write_word(kSubMode, 0x0005 | (1 << 3));              // underwrite
    sub.write_word(kDot + 0x30, 0x0909);
    CHECK(cd.word_ram().bank_byte(0, 0x18) == 0x39);          // only the empty pixel filled
    sub.write_word(kSubMode, 0x0005 | (2 << 3));              // overwrite
    sub.write_word(kDot + 0x30, 0x0700);
    CHECK(cd.word_ram().bank_byte(0, 0x18) == 0x79);          // new 0 is transparent
    sub.write_word(kSubMode, 0x0005 | (3 << 3));              // invalid: no effect
    sub.write_word(kDot + 0x30, 0x0101);
    CHECK(cd.word_ram().bank_byte(0, 0x18) == 0x79);
    sub.write_word(kSubMode, 0x0005);                         // normal
    sub.write_byte(kDot + 0x31, 0x00);
    CHECK(cd.word_ram().bank_byte(0, 0x18) == 0x70);
}

TEST(megacd_word_ram_mode_switch_keeps_data) {
    MegaCd cd;
    auto& sub = cd.sub_bus();
    cd.write16(kWordRam + 0, 0x1111);                 // 2M: bank 0
    cd.write16(kWordRam + 2, 0x2222);                 // 2M: bank 1
    sub.write_word(kSubMode, 0x0005);                 // 1M, RET = 1: main = bank 1
    CHECK(cd.read16(kWordRam) == 0x2222);
    CHECK(sub.read_word(kSubWordRam1M) == 0x1111);
}

// --- Interrupts ------------------------------------------------------------------------

TEST(megacd_int2_doorbell) {
    MegaCd cd;
    auto& regs = cd.registers();
    cd.write8(kMainCtrl, 0x01);                       // IFL2 while INT2 is masked
    CHECK(regs.int2_pending());
    CHECK((cd.read16(kMainCtrl) & 0x0100) != 0);
    CHECK(regs.sub_irq_level() == 0);

    cd.sub_bus().write_word(kSubIntMask, 0x0004);     // enable INT2: raises it now
    CHECK(regs.sub_irq_level() == 2);
    CHECK(cd.sub_cpu().irq_level() == 2);

    regs.acknowledge_sub_interrupt(2);
    CHECK(!regs.int2_pending());
    CHECK(cd.sub_cpu().irq_level() == 0);
    CHECK((cd.read16(kMainCtrl) & 0x0100) == 0);
}

// --- Sub CPU end to end ------------------------------------------------------------------

namespace {

void main_load_prg(MegaCd& cd, u32 offset, std::initializer_list<u16> words) {
    for (u16 w : words) { cd.write16(kPrgWin + offset, w); offset += 2; }
}

} // namespace

TEST(megacd_sub_cpu_boots_from_prg_ram_and_reports) {
    MegaCd cd;
    // Sub CPU held in reset: the main CPU loads its program through the window.
    main_load_prg(cd, 0x000, {0x0007, 0xFFF0, 0x0000, 0x0200});  // SSP, PC = 0x200
    main_load_prg(cd, 0x200, {
        0x3039, 0x00FF, 0x8010,              // move.w ($FF8010).l,d0   ; COMCMD0
        0x5240,                              // addq.w #1,d0
        0x33C0, 0x00FF, 0x8020,              // move.w d0,($FF8020).l   ; COMSTAT0
        0x60FE,                              // bra.s *
    });
    cd.write16(kMainCmd, 0x0041);            // command for the sub CPU

    sub_run(cd);                             // release reset
    cd.run_sub_until(1000);
    CHECK(cd.sub_cpu().registers().pc == 0x20E);
    CHECK(cd.read16(kMainStat) == 0x0042);   // the sub CPU answered

    // Bus request pauses it.
    sub_halt(cd);
    const auto cycles = cd.sub_cpu().cycles();
    cd.run_sub_until(5000);
    CHECK(cd.sub_cpu().cycles() == cycles);
}

TEST(megacd_sub_cpu_takes_int2) {
    MegaCd cd;
    main_load_prg(cd, 0x000, {0x0007, 0xFFF0, 0x0000, 0x0200});
    main_load_prg(cd, 26 * 4, {0x0000, 0x0300});   // level 2 autovector -> 0x300
    main_load_prg(cd, 0x200, {
        0x33FC, 0x0004, 0x00FF, 0x8032,      // move.w #4,($FF8032).l   ; enable INT2
        0x46FC, 0x2000,                      // move #$2000,sr
        0x60FE,                              // bra.s *
    });
    main_load_prg(cd, 0x300, {
        0x33FC, 0x00AA, 0x00FF, 0x8020,      // move.w #$AA,($FF8020).l
        0x4E73,                              // rte
    });
    sub_run(cd);
    cd.run_sub_until(500);
    cd.write8(kMainCtrl, 0x01);              // main CPU rings INT2
    cd.run_sub_until(1500);
    CHECK(cd.read16(kMainStat) == 0x00AA);
    CHECK(!cd.registers().int2_pending());   // acknowledged by the sub CPU
}

// --- Integration with the main bus ----------------------------------------------------------

TEST(megacd_through_main_memory_map) {
    md::Memory mem;
    MegaCd cd;
    std::vector<u8> bios(MegaCd::kBiosSize, 0);
    bios[0x100] = 'S'; bios[0x101] = 'E'; bios[0x102] = 'G'; bios[0x103] = 'A';
    cd.load_bios(bios);

    // Cartridge boot: Mega-CD at 0x400000, cartridge ROM stays at 0.
    mem.load_rom(std::vector<u8>(0x200, 0x77));
    mem.attach_expansion(&cd);
    CHECK(mem.read_byte(0x000000) == 0x77);
    CHECK(mem.read_long(kBios + 0x100) == 0x53454741);  // "SEGA" from the BIOS
    mem.write_word(0xA12010, 0x1234);
    CHECK(cd.registers().command(0) == 0x1234);
    mem.write_word(0x600000, 0x4321);
    CHECK(mem.read_word(0x600000) == 0x4321);
    CHECK((mem.read_byte(0xA11100) & 1) == 1);           // Z80 bus register: still Memory's

    // CD boot: no cartridge, the BIOS appears at 0.
    mem.attach_expansion(&cd, true);
    CHECK(mem.read_long(0x000100) == 0x53454741);
    CHECK(mem.read_word(0x200000) == 0x4321);            // Word RAM at 0x200000
}

TEST(megacd_main_68k_talks_to_sub_68k) {
    // Both CPUs running real code: the main CPU sends a command, the sub CPU
    // doubles it and replies.
    md::Memory mem;
    MegaCd cd;
    mem.attach_expansion(&cd);

    std::vector<u8> rom(0x1000, 0);
    rom[0x01] = 0xFF; rom[0x02] = 0xFE; rom[0x06] = 0x02;   // SSP 0xFFFE00, PC 0x200
    const std::vector<u16> main_code = {
        0x33FC, 0x0015, 0x00A1, 0x2010,      // move.w #$15,($A12010).l  ; COMCMD0
        0x13FC, 0x0001, 0x00A1, 0x2001,      // move.b #1,($A12001).l    ; release sub reset
        // wait:
        0x3039, 0x00A1, 0x2020,              // move.w ($A12020).l,d0     ; COMSTAT0
        0x67F8,                              // beq.s wait
        0x33C0, 0x00FF, 0x0000,              // move.w d0,($FF0000).l
        0x60FE,                              // bra.s *
    };
    std::size_t pc = 0x200;
    for (auto w : main_code) { rom[pc++] = static_cast<u8>(w >> 8); rom[pc++] = static_cast<u8>(w); }
    mem.load_rom(rom);

    main_load_prg(cd, 0x000, {0x0007, 0xFFF0, 0x0000, 0x0200});
    main_load_prg(cd, 0x200, {
        0x3039, 0x00FF, 0x8010,              // move.w ($FF8010).l,d0
        0xD040,                              // add.w d0,d0
        0x33C0, 0x00FF, 0x8020,              // move.w d0,($FF8020).l
        0x60FE,
    });

    md::M68k main_cpu{mem};
    main_cpu.reset();
    // Interleave the two CPUs in small slices, like the scheduler will.
    for (int slice = 1; slice <= 50; ++slice) {
        main_cpu.run_until(main_cpu.cycles() + 100);
        cd.run_sub_until(cd.sub_time() + 160);
    }
    CHECK(mem.read_word(0xFF0000) == 0x002A);   // 0x15 * 2, computed by the sub CPU
}
