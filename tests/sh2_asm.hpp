#pragma once

// A minimal SH-2 assembler for tests: instruction encoders plus a buffer that
// places code and a literal pool (for MOV.L @(disp,PC),Rn).

#include <cassert>
#include <vector>

#include "md/types.hpp"

namespace sh2asm {

using md::u16;
using md::u32;
using md::u8;

constexpr u16 rr(u16 base, u32 m, u32 n) { return static_cast<u16>(base | (n << 8) | (m << 4)); }
constexpr u16 rn(u16 base, u32 n) { return static_cast<u16>(base | (n << 8)); }
constexpr u16 im(u16 base, u32 n, int imm) { return static_cast<u16>(base | (n << 8) | (static_cast<u32>(imm) & 0xFF)); }

// Data movement
constexpr u16 mov_imm(int imm, u32 n) { return im(0xE000, n, imm); }
constexpr u16 mov(u32 m, u32 n)      { return rr(0x6003, m, n); }
constexpr u16 movw_st(u32 m, u32 n)  { return rr(0x2001, m, n); }    // mov.w Rm,@Rn
constexpr u16 movl_st(u32 m, u32 n)  { return rr(0x2002, m, n); }    // mov.l Rm,@Rn
constexpr u16 movl_push(u32 m, u32 n){ return rr(0x2006, m, n); }    // mov.l Rm,@-Rn
constexpr u16 movw_ld(u32 m, u32 n)  { return rr(0x6001, m, n); }    // mov.w @Rm,Rn
constexpr u16 movl_ld(u32 m, u32 n)  { return rr(0x6002, m, n); }    // mov.l @Rm,Rn
constexpr u16 movl_pop(u32 m, u32 n) { return rr(0x6006, m, n); }    // mov.l @Rm+,Rn
constexpr u16 movw_st_disp(u32 disp, u32 n) { return static_cast<u16>(0x8100 | (n << 4) | disp); }  // mov.w r0,@(disp*2,Rn)
constexpr u16 movw_ld_disp(u32 disp, u32 m) { return static_cast<u16>(0x8500 | (m << 4) | disp); }  // mov.w @(disp*2,Rm),r0
constexpr u16 movl_st_disp(u32 m, u32 disp, u32 n) { return static_cast<u16>(0x1000 | (n << 8) | (m << 4) | disp); }
constexpr u16 movl_ld_disp(u32 disp, u32 m, u32 n) { return static_cast<u16>(0x5000 | (n << 8) | (m << 4) | disp); }
constexpr u16 swapw(u32 m, u32 n)    { return rr(0x6009, m, n); }
constexpr u16 extu_w(u32 m, u32 n)   { return rr(0x600D, m, n); }
constexpr u16 exts_w(u32 m, u32 n)   { return rr(0x600F, m, n); }

// Arithmetic / logic
constexpr u16 add(u32 m, u32 n)      { return rr(0x300C, m, n); }
constexpr u16 add_imm(int imm, u32 n){ return im(0x7000, n, imm); }
constexpr u16 sub(u32 m, u32 n)      { return rr(0x3008, m, n); }
constexpr u16 addc(u32 m, u32 n)     { return rr(0x300E, m, n); }
constexpr u16 subc(u32 m, u32 n)     { return rr(0x300A, m, n); }
constexpr u16 xor_(u32 m, u32 n)     { return rr(0x200A, m, n); }
constexpr u16 tst(u32 m, u32 n)      { return rr(0x2008, m, n); }
constexpr u16 cmp_eq(u32 m, u32 n)   { return rr(0x3000, m, n); }
constexpr u16 cmp_hs(u32 m, u32 n)   { return rr(0x3002, m, n); }
constexpr u16 dt(u32 n)              { return rn(0x4010, n); }
constexpr u16 cmp_pz(u32 n)          { return rn(0x4011, n); }
constexpr u16 shll16(u32 n)          { return rn(0x4028, n); }
constexpr u16 rotcl(u32 n)           { return rn(0x4024, n); }
constexpr u16 div0u()                { return 0x0019; }
constexpr u16 div0s(u32 m, u32 n)    { return rr(0x2007, m, n); }
constexpr u16 div1(u32 m, u32 n)     { return rr(0x3004, m, n); }
constexpr u16 mull(u32 m, u32 n)     { return rr(0x0007, m, n); }
constexpr u16 dmuls(u32 m, u32 n)    { return rr(0x300D, m, n); }
constexpr u16 macw(u32 m, u32 n)     { return rr(0x400F, m, n); }
constexpr u16 clrmac()               { return 0x0028; }
constexpr u16 sts_macl(u32 n)        { return rn(0x001A, n); }
constexpr u16 sts_mach(u32 n)        { return rn(0x000A, n); }
constexpr u16 sts_pr(u32 n)          { return rn(0x002A, n); }
constexpr u16 stc_sr(u32 n)          { return rn(0x0002, n); }
constexpr u16 ldc_sr(u32 m)          { return rn(0x400E, m); }
constexpr u16 ldc_vbr(u32 m)         { return rn(0x402E, m); }
constexpr u16 tas(u32 n)             { return rn(0x401B, n); }

// Control flow
constexpr u16 nop()   { return 0x0009; }
constexpr u16 rts()   { return 0x000B; }
constexpr u16 rte()   { return 0x002B; }
constexpr u16 sleep() { return 0x001B; }
constexpr u16 trapa(int imm) { return static_cast<u16>(0xC300 | (imm & 0xFF)); }
constexpr u16 jsr(u32 m) { return rn(0x400B, m); }

// A program image. Code goes at `pc`; mov_l(n, value) places `value` in the
// literal pool (which grows upward from `pool`) and loads it PC-relative.
struct Program {
    std::vector<u8> image;
    u32 pc;
    u32 pool;

    Program(u32 size, u32 code_start, u32 pool_start) : image(size, 0), pc(code_start), pool(pool_start) {}

    void put16(u32 at, u16 v) {
        image[at] = static_cast<u8>(v >> 8);
        image[at + 1] = static_cast<u8>(v);
    }
    void put32(u32 at, u32 v) {
        put16(at, static_cast<u16>(v >> 16));
        put16(at + 2, static_cast<u16>(v));
    }
    Program& operator<<(u16 op) { put16(pc, op); pc += 2; return *this; }

    // mov.l @(disp,pc),Rn with the literal at the next pool slot.
    Program& mov_l(u32 n, u32 value) {
        put32(pool, value);
        const u32 base = (pc + 4) & ~3u;
        assert(pool >= base && (pool - base) / 4 < 256);
        *this << static_cast<u16>(0xD000 | (n << 8) | ((pool - base) / 4));
        pool += 4;
        return *this;
    }
    // Branches to an absolute target.
    [[nodiscard]] u16 disp8(u16 base, u32 target) const {
        const int d = (static_cast<int>(target) - static_cast<int>(pc + 4)) / 2;
        assert(d >= -128 && d < 128);
        return static_cast<u16>(base | (static_cast<u32>(d) & 0xFF));
    }
    [[nodiscard]] u16 disp12(u16 base, u32 target) const {
        const int d = (static_cast<int>(target) - static_cast<int>(pc + 4)) / 2;
        assert(d >= -2048 && d < 2048);
        return static_cast<u16>(base | (static_cast<u32>(d) & 0xFFF));
    }
    Program& bt(u32 target)  { return *this << disp8(0x8900, target); }
    Program& bf(u32 target)  { return *this << disp8(0x8B00, target); }
    Program& bf_s(u32 target){ return *this << disp8(0x8F00, target); }
    Program& bra(u32 target) { return *this << disp12(0xA000, target); }
    Program& bsr(u32 target) { return *this << disp12(0xB000, target); }
};

} // namespace sh2asm
