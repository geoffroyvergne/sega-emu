// SH-2 interpreter: instructions, delayed branches, exceptions, interrupts,
// and the on-chip peripherals (BCR1 master/slave ID, division unit, timer,
// cache data array). Programs are assembled with tests/sh2_asm.hpp and run
// from a flat 1 MB memory.

#include <vector>

#include "md/sh2.hpp"
#include "sh2_asm.hpp"
#include "test.hpp"

using md::Sh2;
using md::s32;
using md::u16;
using md::u32;
using md::u8;
using namespace sh2asm;

namespace {

struct FlatBus final : md::Sh2MemoryBus {
    std::vector<u8> mem = std::vector<u8>(0x10'0000, 0);
    static u32 at(u32 a) { return a & 0xF'FFFF; }
    u8 read8(u32 a) override { return mem[at(a)]; }
    u16 read16(u32 a) override { return static_cast<u16>((mem[at(a)] << 8) | mem[at(a + 1)]); }
    u32 read32(u32 a) override { return (u32{read16(a)} << 16) | read16(a + 2); }
    void write8(u32 a, u8 v) override { mem[at(a)] = v; }
    void write16(u32 a, u16 v) override { mem[at(a)] = static_cast<u8>(v >> 8); mem[at(a + 1)] = static_cast<u8>(v); }
    void write32(u32 a, u32 v) override { write16(a, static_cast<u16>(v >> 16)); write16(a + 2, static_cast<u16>(v)); }
};

constexpr u32 kCode = 0x1000;
constexpr u32 kPool = 0x1200;  // within MOV.L @(disp,PC) reach of the code
constexpr u32 kStack = 0xF0000;
constexpr u32 kData = 0x8000;    // inside the program image

// Loads `p` (vectors: PC = kCode, SP = kStack) and resets a CPU on it.
struct Rig {
    FlatBus bus;
    Sh2 cpu;
    explicit Rig(Program& p, bool slave = false) : cpu(bus, slave) {
        p.put32(0, kCode);
        p.put32(4, kStack);
        std::copy(p.image.begin(), p.image.end(), bus.mem.begin());
        cpu.reset();
    }
    // Runs until PC reaches `stop` (or a step limit).
    void run_to(u32 stop, int limit = 100000) {
        while (cpu.registers().pc != stop && limit-- > 0) cpu.step();
    }
    u32 r(int i) const { return cpu.registers().r[static_cast<std::size_t>(i)]; }
};

Program program() { return Program(0x10000, kCode, kPool); }

} // namespace

TEST(sh2_reset_vectors) {
    auto p = program();
    Rig rig(p);
    CHECK(rig.cpu.registers().pc == kCode);
    CHECK(rig.r(15) == kStack);
    CHECK((rig.cpu.registers().sr & Sh2::kImask) == Sh2::kImask);    // interrupts masked
    CHECK(rig.cpu.registers().vbr == 0);
}

TEST(sh2_loop_and_arithmetic) {
    // Sum 1..10 with a DT/BF loop.
    auto p = program();
    p << mov_imm(10, 1) << mov_imm(0, 0);
    const u32 loop = p.pc;
    p << add(1, 0) << dt(1);
    p.bf(loop);
    const u32 done = p.pc;
    p << nop();
    Rig rig(p);
    rig.run_to(done);
    CHECK(rig.r(0) == 55);
    CHECK(rig.r(1) == 0);
    CHECK(rig.cpu.registers().sr & Sh2::kT);

    // Sign extension of immediates and 8-bit adds.
    auto q = program();
    q << mov_imm(-2, 3) << add_imm(-3, 3) << mov_imm(0x7F, 4) << add_imm(1, 4);
    const u32 end = q.pc;
    q << nop();
    Rig rig2(q);
    rig2.run_to(end);
    CHECK(rig2.r(3) == static_cast<u32>(-5));
    CHECK(rig2.r(4) == 0x80);
}

TEST(sh2_delay_slots_and_subroutines) {
    // BSR to a subroutine; the instruction after BSR (delay slot) runs before
    // the call; RTS's slot runs before the return.
    auto p = program();
    p.bsr(kCode + 0x40);
    p << mov_imm(1, 1);                    // slot of BSR: runs
    const u32 after_call = p.pc;
    p << mov_imm(3, 3);
    const u32 end = p.pc;
    p << nop();
    p.pc = kCode + 0x40;
    p << mov_imm(2, 2) << rts() << mov_imm(4, 4);   // slot of RTS: runs
    Rig rig(p);
    rig.run_to(after_call);
    CHECK(rig.r(1) == 1 && rig.r(2) == 2 && rig.r(4) == 4);
    CHECK(rig.cpu.registers().pr == after_call);
    rig.run_to(end);
    CHECK(rig.r(3) == 3);

    // BF/S: the slot runs whether or not the branch is taken.
    auto q = program();
    q << mov_imm(0, 5) << tst(5, 5);       // T = 1: BF/S not taken
    q.bf_s(kCode + 0x80);
    q << add_imm(1, 6);                    // slot
    const u32 fall = q.pc;
    q << nop();
    Rig rig2(q);
    rig2.run_to(fall);
    CHECK(rig2.r(6) == 1);
}

TEST(sh2_pc_relative_loads) {
    auto p = program();
    p.mov_l(1, 0xDEADBEEF);
    p << nop();                            // the literal is 4-aligned whatever PC's alignment
    p.mov_l(2, 0x12345678);
    // MOVA: R0 = address of a literal.
    const u32 base = (p.pc + 4) & ~3u;
    p << static_cast<u16>(0xC700 | ((kPool - base) / 4));
    const u32 end = p.pc;
    p << nop();
    Rig rig(p);
    rig.run_to(end);
    CHECK(rig.r(1) == 0xDEADBEEF);
    CHECK(rig.r(2) == 0x12345678);
    CHECK(rig.r(0) == kPool);
}

TEST(sh2_pc_relative_loads_in_delay_slots) {
    // In a delay slot, PC is already the branch target: MOV.W @(disp,PC)
    // reads from target + 4 + disp * 2 - 2 (= target + 2 + disp * 2), and
    // MOV.L / MOVA from ((target + 2) & ~3) + disp * 4.
    auto p = program();
    const u32 target = kCode + 0x40;
    p.bra(target);
    p << static_cast<u16>(0x9600 | 3);          // slot: mov.w @(3,pc),r6
    p.pc = target;
    p.bra(target + 0x20);
    p << static_cast<u16>(0xD700 | 2);          // slot: mov.l @(2,pc),r7
    p.pc = target + 0x20;
    p.bra(target + 0x40);
    p << static_cast<u16>(0xC700 | 1);          // slot: mova @(1,pc),r0
    p.pc = target + 0x40;
    const u32 end = p.pc;
    p << nop();
    // Words around both candidate addresses, so the wrong rule shows.
    p.put16(target + 2 + 3 * 2, 0x4321);        // the slot rule
    p.put16(kCode + 2 + 4 + 3 * 2, 0x1111);     // its own address + 4 (wrong)
    p.put32(((target + 0x20 + 2) & ~3u) + 2 * 4, 0xCAFEF00D);
    Rig rig(p);
    rig.run_to(end);
    CHECK(rig.r(6) == 0x4321);
    CHECK(rig.r(7) == 0xCAFEF00D);
    CHECK(rig.r(0) == ((target + 0x40 + 2) & ~3u) + 1 * 4);
}

TEST(sh2_illegal_instruction_in_delay_slot) {
    // Slot illegal (vector 6): the saved PC is the delayed branch's address,
    // and the branch doesn't happen.
    auto p = program();
    p.put32(Sh2::kVectorSlotIllegal * 4, kCode + 0x100);
    const u32 branch = p.pc;
    p.bra(kCode + 0x80);
    p << 0xFFFF;
    Rig rig(p);
    rig.cpu.step();
    CHECK(rig.cpu.registers().pc == kCode + 0x100);
    CHECK(rig.bus.read32(kStack - 8) == branch);
}

TEST(sh2_division_step_sequences) {
    // The SH-2 manual's division idioms, built from DIV0U/DIV0S, DIV1,
    // ROTCL, SUBC and ADDC.
    {   // Unsigned 32 / 16: R1 / R0 -> R1.
        auto p = program();
        p.mov_l(1, 1000);
        p << mov_imm(7, 0) << shll16(0) << div0u();
        for (int i = 0; i < 16; ++i) p << div1(0, 1);
        p << rotcl(1) << extu_w(1, 1);
        const u32 end = p.pc;
        p << nop();
        Rig rig(p);
        rig.run_to(end);
        CHECK(rig.r(1) == 142);
    }
    {   // Unsigned 64 / 32: R1:R2 / R0 -> R2.
        auto p = program();
        p << mov_imm(0x12, 1);
        p.mov_l(2, 0x34567890);
        p.mov_l(0, 0x100);
        p << div0u();
        for (int i = 0; i < 32; ++i) p << rotcl(2) << div1(0, 1);
        p << rotcl(2);
        const u32 end = p.pc;
        p << nop();
        Rig rig(p);
        rig.run_to(end);
        CHECK(rig.r(2) == 0x12345678);
    }
    // Signed 16 / 16: R1 / R0 -> R1, every sign combination.
    for (const auto [a, b] : {std::pair{1000, 7}, {-1000, 7}, {1000, -7}, {-1000, -7}, {-7, 1000}}) {
        auto p = program();
        p.mov_l(1, static_cast<u32>(a));
        p.mov_l(0, static_cast<u32>(b));
        p << shll16(0) << exts_w(1, 1) << xor_(2, 2) << mov(1, 3) << rotcl(3) << subc(2, 1) << div0s(0, 1);
        for (int i = 0; i < 16; ++i) p << div1(0, 1);
        p << exts_w(1, 1) << rotcl(1) << addc(2, 1) << exts_w(1, 1);
        const u32 end = p.pc;
        p << nop();
        Rig rig(p);
        rig.run_to(end);
        CHECK(static_cast<s32>(rig.r(1)) == a / b);
    }
}

TEST(sh2_multiply_and_mac) {
    auto p = program();
    p.mov_l(1, static_cast<u32>(-123456789));
    p.mov_l(2, 1000);
    p << mull(1, 2) << sts_macl(3);                           // 32-bit product
    p << dmuls(1, 2) << sts_mach(4) << sts_macl(5);           // 64-bit signed
    // MAC.W over two pairs of words at kData: 3*4 + (-2)*5 = 2.
    p.mov_l(6, kData);
    p.mov_l(7, kData + 0x10);
    p << clrmac() << macw(7, 6) << macw(7, 6) << sts_macl(8);
    const u32 end = p.pc;
    p << nop();
    p.put16(kData, 3); p.put16(kData + 2, static_cast<u16>(-2));
    p.put16(kData + 0x10, 4); p.put16(kData + 0x12, 5);
    Rig rig(p);
    rig.run_to(end);
    CHECK(rig.r(3) == static_cast<u32>(-123456789 * 1000LL));
    CHECK(rig.r(4) == 0xFFFFFFE3 && rig.r(5) == 0x4166E5F8);
    CHECK(rig.r(8) == 2);
    CHECK(rig.r(6) == kData + 4 && rig.r(7) == kData + 0x14);  // both pointers advanced
}

TEST(sh2_trapa_and_rte) {
    auto p = program();
    p.put32(33 * 4, kCode + 0x100);        // vector 33
    p << mov_imm(0, 1) << trapa(33);
    const u32 back = p.pc;
    p << mov_imm(9, 9);
    const u32 end = p.pc;
    p << nop();
    p.pc = kCode + 0x100;
    p << mov_imm(1, 1) << rte() << nop();
    Rig rig(p);
    rig.run_to(kCode + 0x100);
    CHECK(rig.r(15) == kStack - 8);        // SR and PC pushed
    CHECK(rig.bus.read32(kStack - 8) == back);
    rig.run_to(end);
    CHECK(rig.r(1) == 1 && rig.r(9) == 9 && rig.r(15) == kStack);
}

TEST(sh2_irl_interrupt_and_sleep) {
    // Level 8 (the 32X's CMD) is auto-vectored to 68; the mask rises to 8
    // while it's handled, and SLEEP waits for it.
    auto p = program();
    p.put32(Sh2::irl_vector(8) * 4, kCode + 0x100);
    p << mov_imm(0, 0) << ldc_sr(0);       // unmask
    const u32 sleeping = p.pc;
    p << sleep();
    const u32 woke = p.pc;
    p << nop();
    p.pc = kCode + 0x100;
    p << stc_sr(2) << add_imm(1, 1) << rte() << nop();
    Rig rig(p);
    CHECK(Sh2::irl_vector(8) == 68 && Sh2::irl_vector(14) == 71 && Sh2::irl_vector(6) == 67);
    rig.run_to(sleeping + 2);
    rig.cpu.step();
    CHECK(rig.cpu.sleeping());
    rig.cpu.run_until(rig.cpu.cycles() + 1000);                // nothing happens
    CHECK(rig.cpu.sleeping() && rig.r(1) == 0);

    rig.cpu.set_irl(8);
    rig.cpu.step();                                            // takes it
    CHECK(rig.cpu.registers().pc == kCode + 0x100);
    CHECK(((rig.cpu.registers().sr >> 4) & 0xF) == 8);
    rig.cpu.set_irl(0);
    rig.run_to(woke);
    CHECK(rig.r(1) == 1);
    CHECK(((rig.r(2) >> 4) & 0xF) == 8);                       // SR inside the handler

    // A level at or below the mask waits.
    auto q = program();
    q.put32(Sh2::irl_vector(6) * 4, kCode + 0x100);
    q << mov_imm(0x70, 0) << ldc_sr(0) << nop() << nop();      // mask 7
    Rig rig2(q);
    rig2.cpu.set_irl(6);
    for (int i = 0; i < 4; ++i) rig2.cpu.step();
    CHECK(rig2.cpu.registers().pc != kCode + 0x100);
}

TEST(sh2_illegal_instruction) {
    auto p = program();
    p.put32(Sh2::kVectorIllegal * 4, kCode + 0x100);
    p << 0xFFFF;
    Rig rig(p);
    rig.cpu.step();
    CHECK(rig.cpu.registers().pc == kCode + 0x100);
    CHECK(rig.bus.read32(kStack - 8) == kCode);                // the faulting instruction
}

TEST(sh2_master_slave_identity_bcr1) {
    // Shared code tells the CPUs apart by BCR1's MASTER bit.
    FlatBus bus;
    Sh2 master(bus, false), slave(bus, true);
    CHECK((master.read32(Sh2::kBcr1) & Sh2::kBcr1Slave) == 0);
    CHECK((slave.read32(Sh2::kBcr1) & Sh2::kBcr1Slave) != 0);
    CHECK(slave.read16(Sh2::kBcr1 + 2) & Sh2::kBcr1Slave);
    // Keyed writes reach the register; the MASTER bit stays read-only.
    master.write32(Sh2::kBcr1, 0xA55A'8001);
    CHECK(master.read32(Sh2::kBcr1) == 0x0001);
    slave.write32(Sh2::kBcr1, 0x1234'0000);                    // no key: ignored
    CHECK(slave.read32(Sh2::kBcr1) & Sh2::kBcr1Slave);
}

TEST(sh2_division_unit) {
    FlatBus bus;
    Sh2 cpu(bus, false);
    cpu.write32(Sh2::kDvsr, 7);
    cpu.write32(Sh2::kDvdnt, static_cast<u32>(-1000));         // 32 / 32
    CHECK(static_cast<s32>(cpu.read32(Sh2::kDvdnt)) == -142);
    CHECK(static_cast<s32>(cpu.read32(Sh2::kDvdnth)) == -6);
    CHECK(cpu.read32(Sh2::kDvdntl) == cpu.read32(Sh2::kDvdnt));

    cpu.write32(Sh2::kDvsr, 0x100);                            // 64 / 32
    cpu.write32(Sh2::kDvdnth, 0x12);
    cpu.write32(Sh2::kDvdntl, 0x34567890);
    CHECK(cpu.read32(Sh2::kDvdntl) == 0x12345678);
    CHECK(cpu.read32(Sh2::kDvdnth) == 0x90);
    CHECK(cpu.read32(0xFFFFFF34) == 0x12345678);               // mirror (+0x20)
    CHECK((cpu.read32(Sh2::kDvcr) & 1) == 0);

    cpu.write32(Sh2::kDvsr, 0);                                // overflow
    cpu.write32(Sh2::kDvdnt, 5);
    CHECK(cpu.read32(Sh2::kDvcr) & 1);
    CHECK(cpu.read32(Sh2::kDvdnt) == 0x7FFFFFFF);
}

TEST(sh2_free_running_timer_and_data_array) {
    auto p = program();
    const u32 loop = p.pc;
    p.bra(loop);
    p << nop();
    Rig rig(p);
    rig.cpu.write8(Sh2::kTcr, 0);                              // clock / 8
    const u16 before = rig.cpu.read16(Sh2::kFrcH);
    rig.cpu.run_until(rig.cpu.cycles() + 800);
    const u16 after = rig.cpu.read16(Sh2::kFrcH);
    CHECK(after - before >= 99 && after - before <= 101);

    rig.cpu.write32(0xC0000010, 0xCAFEF00D);                   // cache data array as RAM
    CHECK(rig.cpu.read32(0xC0000010) == 0xCAFEF00D);
    CHECK(rig.cpu.read16(0xC0001012) == 0xF00D);               // 4 KB, mirrored
    rig.cpu.write32(0x40000010, 0x11111111);                   // purge area: ignored
    CHECK(rig.bus.read32(0x10) == 0);
}

// DMA controller ===============================================================

namespace {

// Sets up `channel`: source, destination, count, CHCR; DMAOR.DME on.
void dma_setup(Sh2& cpu, int channel, u32 src, u32 dst, u32 count, u32 chcr) {
    const u32 base = static_cast<u32>(channel) * Sh2::kDmaChannelStride;
    cpu.write32(Sh2::kSar0 + base, src);
    cpu.write32(Sh2::kDar0 + base, dst);
    cpu.write32(Sh2::kTcr0 + base, count);
    cpu.write32(Sh2::kDmaor, Sh2::kDmaorDme);
    cpu.write32(Sh2::kChcr0 + base, chcr);
}

// CHCR fields.
constexpr u32 kDstInc = 1u << 14, kDstDec = 2u << 14, kSrcInc = 1u << 12, kSrcDec = 2u << 12;
constexpr u32 kByte = 0u << 10, kWord = 1u << 10, kLong = 2u << 10, k16Byte = 3u << 10;

} // namespace

TEST(sh2_dma_auto_request) {
    auto p = program();
    for (u32 i = 0; i < 64; ++i) p.image[kData + i] = static_cast<u8>(i + 1);
    Rig rig(p);
    auto& cpu = rig.cpu;
    auto& mem = rig.bus.mem;

    // Words, both incrementing: runs to the end at once and sets TE.
    dma_setup(cpu, 0, kData, 0x9000, 4, kDstInc | kSrcInc | kWord | Sh2::kChcrAr | Sh2::kChcrDe);
    CHECK(mem[0x9000] == 1 && mem[0x9007] == 8 && mem[0x9008] == 0);
    CHECK(cpu.read32(Sh2::kTcr0) == 0);
    CHECK(cpu.read32(Sh2::kSar0) == kData + 8 && cpu.read32(Sh2::kDar0) == 0x9008);
    CHECK((cpu.read32(Sh2::kChcr0) & Sh2::kChcrTe) != 0);

    // TE can't be set by software, and while it's set the channel is idle.
    cpu.write32(Sh2::kTcr0, 2);
    cpu.write32(Sh2::kChcr0, kDstInc | kSrcInc | kWord | Sh2::kChcrAr | Sh2::kChcrDe | Sh2::kChcrTe);
    CHECK(cpu.read32(Sh2::kTcr0) == 2);                        // nothing moved
    cpu.write32(Sh2::kChcr0, kDstInc | kSrcInc | kWord | Sh2::kChcrAr | Sh2::kChcrDe);   // TE written 0
    CHECK(cpu.read32(Sh2::kTcr0) == 0 && mem[0x900B] == 12);

    // 16-byte units on channel 1 (TCR counts longs); fixed and decrementing.
    dma_setup(cpu, 1, kData, 0xA000, 8, kDstInc | kSrcInc | k16Byte | Sh2::kChcrAr | Sh2::kChcrDe);
    CHECK(mem[0xA000] == 1 && mem[0xA01F] == 32 && mem[0xA020] == 0);
    dma_setup(cpu, 1, kData + 3, 0xB000, 3, kDstInc | kSrcDec | kByte | Sh2::kChcrAr | Sh2::kChcrDe);
    CHECK(mem[0xB000] == 4 && mem[0xB001] == 3 && mem[0xB002] == 2);
    dma_setup(cpu, 1, kData, 0xC000, 3, kDstDec | kLong | Sh2::kChcrAr | Sh2::kChcrDe);   // source fixed
    CHECK(cpu.read32(0xC000) == 0x01020304 && cpu.read32(0xBFFC) == 0x01020304 && cpu.read32(0xBFF8) == 0x01020304);

    // DMAOR.DME off: nothing runs.
    cpu.write32(Sh2::kDmaor, 0);
    cpu.write32(Sh2::kTcr0, 1);
    cpu.write32(Sh2::kDar0, 0xD000);
    cpu.write32(Sh2::kChcr0, kDstInc | kSrcInc | kWord | Sh2::kChcrAr | Sh2::kChcrDe);
    CHECK(mem[0xD000] == 0 && cpu.read32(Sh2::kTcr0) == 1);
}

TEST(sh2_dma_external_requests) {
    auto p = program();
    for (u32 i = 0; i < 8; ++i) p.image[kData + i] = static_cast<u8>(0x10 + i);
    Rig rig(p);
    auto& cpu = rig.cpu;
    dma_setup(cpu, 0, kData, 0x9000, 2, kDstInc | kSrcInc | kWord | Sh2::kChcrDe);   // AR = 0
    CHECK(rig.bus.mem[0x9000] == 0);                           // waits for DREQ
    CHECK(cpu.dma_waiting_for_request(0) && !cpu.dma_waiting_for_request(1));
    CHECK(cpu.dma_request(0));                                 // one unit per request
    CHECK(rig.bus.mem[0x9001] == 0x11 && rig.bus.mem[0x9002] == 0);
    CHECK(cpu.dma_request(0));
    CHECK(rig.bus.mem[0x9003] == 0x13);
    CHECK((cpu.read32(Sh2::kChcr0) & Sh2::kChcrTe) != 0);
    CHECK(!cpu.dma_request(0));                                // done
    CHECK(!cpu.dma_request(1));                                // never set up
}

TEST(sh2_dma_end_interrupt) {
    // IE: the end of the transfer interrupts at IPRA's DMAC level through
    // VCRDMA0, until TE is cleared.
    auto p = program();
    p.put32(0x50 * 4, kCode + 0x100);                          // vector 0x50
    p << mov_imm(0, 0) << ldc_sr(0);                           // unmask
    const u32 loop = p.pc;
    p.bra(loop);
    p << nop();
    Rig rig(p);
    auto& cpu = rig.cpu;
    rig.run_to(loop);
    cpu.write16(Sh2::kIpra, 0x0900);                           // DMAC level 9
    cpu.write32(Sh2::kVcrDma0, 0x50);
    dma_setup(cpu, 0, kData, 0x9000, 1, kDstInc | kSrcInc | kWord | Sh2::kChcrIe | Sh2::kChcrAr | Sh2::kChcrDe);
    cpu.step();
    CHECK(cpu.registers().pc == kCode + 0x100);
    CHECK(((cpu.registers().sr >> 4) & 0xF) == 9);

    // An IRL at the same level wins over the on-chip source.
    auto q = program();
    q.put32(0x50 * 4, kCode + 0x100);
    q.put32(Sh2::irl_vector(9) * 4, kCode + 0x200);
    q << mov_imm(0, 0) << ldc_sr(0) << nop();
    Rig rig2(q);
    rig2.run_to(kCode + 4);
    rig2.cpu.write16(Sh2::kIpra, 0x0900);
    rig2.cpu.write32(Sh2::kVcrDma0, 0x50);
    dma_setup(rig2.cpu, 0, kData, 0x9000, 1, kDstInc | kSrcInc | kWord | Sh2::kChcrIe | Sh2::kChcrAr | Sh2::kChcrDe);
    rig2.cpu.set_irl(9);
    rig2.cpu.step();
    CHECK(rig2.cpu.registers().pc == kCode + 0x200);

    // Cleared TE: no request.
    auto r = program();
    r.put32(0x50 * 4, kCode + 0x100);
    r << mov_imm(0, 0) << ldc_sr(0) << nop() << nop();
    Rig rig3(r);
    rig3.run_to(kCode + 4);
    rig3.cpu.write16(Sh2::kIpra, 0x0900);
    rig3.cpu.write32(Sh2::kVcrDma0, 0x50);
    dma_setup(rig3.cpu, 0, kData, 0x9000, 1, kDstInc | kSrcInc | kWord | Sh2::kChcrIe | Sh2::kChcrAr | Sh2::kChcrDe);
    rig3.cpu.write32(Sh2::kChcr0, 0);
    rig3.cpu.step();
    CHECK(rig3.cpu.registers().pc == kCode + 6);
}

TEST(sh2_watchdog_interval_timer) {
    auto p = program();
    p.put32(0x60 * 4, kCode + 0x100);                          // vector 0x60
    p << mov_imm(0, 0) << ldc_sr(0);                           // unmask
    const u32 sleeping = p.pc;
    p << sleep();
    p << nop();
    Rig rig(p);
    auto& cpu = rig.cpu;
    CHECK(cpu.read8(Sh2::kWtcsr) == 0x18);                     // reset: stopped
    cpu.write8(Sh2::kWtcnt, 0x12);                             // byte writes: ignored
    CHECK(cpu.read8(Sh2::kWtcnt) == 0);
    cpu.write16(Sh2::kWtcsr, 0x12F0);                          // wrong key: ignored
    CHECK(cpu.read8(Sh2::kWtcnt) == 0);

    cpu.write16(Sh2::kIpra, 0x0050);                           // WDT level 5
    cpu.write16(Sh2::kVcrWdt, 0x6000);                         // vector 0x60
    cpu.write16(Sh2::kWtcsr, 0x5AF0);                          // WTCNT = 0xF0
    cpu.write16(Sh2::kWtcsr, 0xA521);                          // TME, interval, clock / 64
    CHECK(cpu.read8(Sh2::kWtcsr) == 0x39);
    rig.run_to(sleeping + 2);                                  // asleep
    CHECK(cpu.sleeping());
    // 16 counts of 64 cycles to overflow; the interrupt wakes the CPU.
    cpu.run_until(cpu.cycles() + 15 * 64);
    CHECK(cpu.sleeping() && cpu.read8(Sh2::kWtcnt) >= 0xFE);
    cpu.run_until(cpu.cycles() + 2 * 64);
    CHECK(!cpu.sleeping());
    CHECK(cpu.registers().pc >= kCode + 0x100 && cpu.registers().pc < kCode + 0x110);
    CHECK(((cpu.registers().sr >> 4) & 0xF) == 5);
    CHECK((cpu.read8(Sh2::kWtcsr) & Sh2::kWtcsrOvf) != 0);

    // OVF only clears by writing 0 (the key write keeps the other bits).
    cpu.write16(Sh2::kWtcsr, 0xA5A1);                          // writing 1: no change
    CHECK((cpu.read8(Sh2::kWtcsr) & Sh2::kWtcsrOvf) != 0);
    cpu.write16(Sh2::kWtcsr, 0xA521);
    CHECK(cpu.read8(Sh2::kWtcsr) == 0x39);

    // Watchdog mode (WT/IT = 1): OVF sets, but no interrupt is requested.
    auto q = program();
    q.put32(0x60 * 4, kCode + 0x100);
    q << mov_imm(0, 0) << ldc_sr(0);
    const u32 loop = q.pc;
    q.bra(loop);
    q << nop();
    Rig rig2(q);
    auto& wd = rig2.cpu;
    rig2.run_to(loop);
    wd.write16(Sh2::kIpra, 0x0050);
    wd.write16(Sh2::kVcrWdt, 0x6000);
    wd.write16(Sh2::kWtcsr, 0x5AFF);
    wd.write16(Sh2::kWtcsr, 0xA560);                           // TME, WT, clock / 2
    wd.run_until(wd.cycles() + 64);
    CHECK((wd.read8(Sh2::kWtcsr) & Sh2::kWtcsrOvf) != 0);
    CHECK(wd.registers().pc >= loop && wd.registers().pc <= loop + 2);   // still looping
}

TEST(sh2_disassembler) {
    CHECK(Sh2::disassemble(0x0009) == "nop");
    CHECK(Sh2::disassemble(mov_imm(-1, 3)) == "mov #-1,r3");
    CHECK(Sh2::disassemble(add(2, 5)) == "add r2,r5");
    CHECK(Sh2::disassemble(movl_pop(15, 1)) == "mov.l @r15+,r1");
    CHECK(Sh2::disassemble(0xFFFF) == ".word $ffff");
}
