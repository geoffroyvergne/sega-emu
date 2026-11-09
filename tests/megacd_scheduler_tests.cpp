// Mega-CD in the emulator's scheduler: clock ratio, lockstep, interrupt latency.
// Programs are hand-assembled; the main CPU's code is the "BIOS" (CD boot).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "md/clock.hpp"
#include "md/emulator.hpp"
#include "test.hpp"

using md::s64;
using md::u16;
using md::u32;
using md::u8;

namespace {

// Tiny assembler helper: emits words and resolves short branches to labels.
class Asm {
public:
    explicit Asm(u32 base) : base_(base) {}
    [[nodiscard]] u32 pc() const { return base_ + static_cast<u32>(words_.size() * 2); }
    Asm& w(std::initializer_list<u16> ws) { words_.insert(words_.end(), ws); return *this; }
    // Bcc.s to an earlier label (opcode: 0x6000 bra, 0x6600 bne, 0x6700 beq).
    Asm& branch(u16 opcode, u32 target) {
        const auto disp = static_cast<int>(target) - static_cast<int>(pc() + 2);
        words_.push_back(static_cast<u16>(opcode | static_cast<u8>(static_cast<signed char>(disp))));
        return *this;
    }
    [[nodiscard]] const std::vector<u16>& words() const { return words_; }
private:
    u32 base_;
    std::vector<u16> words_;
};

void put(std::vector<u8>& image, u32 at, const std::vector<u16>& words) {
    for (u16 v : words) {
        image[at++] = static_cast<u8>(v >> 8);
        image[at++] = static_cast<u8>(v);
    }
}

// BIOS image (CD boot: appears at 0x000000) with main-CPU vectors and code at 0x200.
std::vector<u8> bios_with(const Asm& main_code) {
    std::vector<u8> bios(md::megacd::MegaCd::kBiosSize, 0);
    put(bios, 0, {0x00FF, 0xFE00, 0x0000, 0x0200});   // SSP 0xFFFE00, PC 0x200
    put(bios, 0x200, main_code.words());
    return bios;
}

// Loads sub-CPU code into PRG-RAM (vectors + code at 0x200), as the BIOS would.
void load_sub(md::megacd::MegaCd& cd, const Asm& code, std::initializer_list<std::pair<int, u32>> vectors = {}) {
    std::vector<u8> prg(0x400, 0);
    put(prg, 0, {0x0007, 0xFFF0, 0x0000, 0x0200});    // SSP 0x7FFF0, PC 0x200
    for (auto [n, handler] : vectors) put(prg, static_cast<u32>(n) * 4, {static_cast<u16>(handler >> 16), static_cast<u16>(handler)});
    std::vector<u16> words = code.words();
    for (std::size_t i = 0; i < words.size(); ++i) {
        prg[0x200 + 2 * i] = static_cast<u8>(words[i] >> 8);
        prg[0x201 + 2 * i] = static_cast<u8>(words[i]);
    }
    for (u32 a = 0; a < prg.size(); a += 2) {
        cd.sub_bus().write_word(a, static_cast<u16>((prg[a] << 8) | prg[a + 1]));
    }
}

// Set MD_TEST_VERBOSE=1 to print the measured values.
bool verbose() { return std::getenv("MD_TEST_VERBOSE") != nullptr; }

// move.b #1,($A12001).l : release the sub CPU's reset.
constexpr std::initializer_list<u16> kReleaseSub = {0x13FC, 0x0001, 0x00A1, 0x2001};

} // namespace

TEST(clock_converter_is_exact) {
    md::ClockConverter c{12'500'000, 53'693'175};
    s64 master = 0;
    while (master + 224 <= 53'693'175) { c.advance(224); master += 224; }
    c.advance(53'693'175 - master);                 // complete exactly one second
    CHECK(c.total() == 12'500'000);
}

TEST(emulator_sub_cpu_ratio) {
    md::Emulator emu;
    CHECK(std::abs(emu.sub_cpu_ratio() - 1.6296) < 0.0001);
    md::Emulator pal{md::Region::PAL};
    CHECK(std::abs(pal.sub_cpu_ratio() - 1.6446) < 0.0001);
}

TEST(scheduler_runs_both_cpus_at_the_right_ratio) {
    Asm main_code{0x200};
    main_code.w(kReleaseSub);
    const u32 spin = main_code.pc();
    main_code.branch(0x6000, spin);                 // bra.s *

    md::Emulator emu;
    auto& cd = emu.attach_mega_cd(bios_with(main_code), true);
    Asm sub_code{0x200};
    sub_code.branch(0x6000, 0x200);                 // bra.s *
    load_sub(cd, sub_code);

    emu.run_frame();                                // sub CPU released during this frame
    const s64 main_start = emu.cpu().cycles();
    const s64 sub_start = cd.sub_cpu().cycles();
    for (int i = 0; i < 10; ++i) emu.run_frame();
    const double main_cycles = static_cast<double>(emu.cpu().cycles() - main_start);
    const double sub_cycles = static_cast<double>(cd.sub_cpu().cycles() - sub_start);

    if (verbose()) std::printf("    main %.0f cycles, sub %.0f cycles, ratio %.5f\n",
                               main_cycles, sub_cycles, sub_cycles / main_cycles);
    CHECK(std::abs(main_cycles - 1'280'057) < 20);  // 10 NTSC frames of 68000 time
    CHECK(std::abs(sub_cycles / main_cycles - emu.sub_cpu_ratio()) < 0.001);
}

TEST(scheduler_flag_handshake_like_the_bios) {
    // Main: increments its flag, then waits until the sub CPU echoes it.
    // Sub:  copies the main flag into its own flag, forever.
    Asm m{0x200};
    m.w(kReleaseSub);
    m.w({0x7200});                                  // moveq #0,d1
    const u32 loop = m.pc();
    m.w({0x5201});                                  // addq.b #1,d1
    m.w({0x13C1, 0x00A1, 0x200E});                  // move.b d1,($A1200E).l   main flag
    const u32 wait = m.pc();
    m.w({0xB239, 0x00A1, 0x200F});                  // cmp.b ($A1200F).l,d1    sub flag
    m.branch(0x6600, wait);                         // bne.s wait
    m.w({0x5279, 0x00FF, 0x0000});                  // addq.w #1,($FF0000).l   round trips
    m.branch(0x6000, loop);

    Asm s{0x200};
    s.w({0x1039, 0x00FF, 0x800E});                  // move.b ($FF800E).l,d0
    s.w({0x13C0, 0x00FF, 0x800F});                  // move.b d0,($FF800F).l
    s.branch(0x6000, 0x200);

    md::Emulator emu;
    auto& cd = emu.attach_mega_cd(bios_with(m), true);
    load_sub(cd, s);
    emu.run_frame();
    const u16 first = emu.memory().read_word(0xFF0000);
    emu.run_frame();
    const u16 per_frame = static_cast<u16>(emu.memory().read_word(0xFF0000) - first);

    // Each round trip costs about two lockstep slices (~64 main cycles plus
    // the polling instructions); a frame is ~128k main cycles.
    if (verbose()) std::printf("    %u flag round trips per frame\n", per_frame);
    CHECK(per_frame > 900);
    CHECK(per_frame < 3000);
    CHECK(cd.registers().sub_flags() == cd.registers().main_flags());
}

TEST(scheduler_int2_latency_main_to_sub) {
    // Main: wait for "ready" (COMSTAT1), set IFL2, then count polls until the
    // sub CPU's INT2 handler has written COMSTAT0.
    Asm m{0x200};
    m.w(kReleaseSub);
    const u32 ready = m.pc();
    m.w({0x4A79, 0x00A1, 0x2022});                  // tst.w ($A12022).l
    m.branch(0x6700, ready);                        // beq.s ready
    m.w({0x7400});                                  // moveq #0,d2
    m.w({0x13FC, 0x0001, 0x00A1, 0x2000});          // move.b #1,($A12000).l   IFL2
    const u32 poll = m.pc();
    m.w({0x5242});                                  // addq.w #1,d2
    m.w({0x4A79, 0x00A1, 0x2020});                  // tst.w ($A12020).l
    m.branch(0x6700, poll);                         // beq.s poll
    m.w({0x33C2, 0x00FF, 0x0000});                  // move.w d2,($FF0000).l
    const u32 done = m.pc();
    m.branch(0x6000, done);

    // Sub: enable INT2, unmask, signal ready, idle. Handler writes COMSTAT0.
    Asm s{0x200};
    s.w({0x33FC, 0x0004, 0x00FF, 0x8032});          // move.w #4,($FF8032).l
    s.w({0x46FC, 0x2000});                          // move #$2000,sr
    s.w({0x33FC, 0x0001, 0x00FF, 0x8022});          // move.w #1,($FF8022).l   ready
    const u32 idle = s.pc();
    s.branch(0x6000, idle);
    const u32 handler = s.pc();
    s.w({0x33FC, 0x0001, 0x00FF, 0x8020});          // move.w #1,($FF8020).l
    s.w({0x4E73});                                  // rte

    md::Emulator emu;
    auto& cd = emu.attach_mega_cd(bios_with(m), true);
    load_sub(cd, s, {{26, handler}});               // level 2 autovector
    emu.run_frame();

    const u16 polls = emu.memory().read_word(0xFF0000);
    // The sub CPU takes INT2 in the same slice as the IFL2 write; its reply
    // is visible to the main CPU from the next slice: a few 30-cycle polls.
    if (verbose()) std::printf("    INT2 reply seen after %u polls (~%u main cycles)\n", polls, polls * 30u);
    CHECK(polls >= 1);
    CHECK(polls <= 4);
    CHECK(!cd.registers().int2_pending());          // acknowledged by the sub CPU
}

TEST(scheduler_expansion_interrupt_reaches_main_cpu) {
    // The expansion port shares the main CPU's IPL pins with the VDP.
    Asm m{0x200};
    m.w({0x46FC, 0x2000});                          // move #$2000,sr
    const u32 idle = m.pc();
    m.branch(0x6000, idle);
    const u32 handler = m.pc();
    m.w({0x5279, 0x00FF, 0x0010});                  // addq.w #1,($FF0010).l
    m.w({0x4E73});                                  // rte

    auto bios = bios_with(m);
    put(bios, 26 * 4, {static_cast<u16>(handler >> 16), static_cast<u16>(handler)});

    md::Emulator emu;
    auto& cd = emu.attach_mega_cd(bios, true);
    emu.run_frame();
    cd.request_main_interrupt(2);
    CHECK(emu.cpu().irq_level() == 2);              // asserted immediately
    emu.run_frame();
    CHECK(emu.memory().read_word(0xFF0010) == 1);   // taken once, then acknowledged
    CHECK(cd.main_irq_level() == 0);
}
