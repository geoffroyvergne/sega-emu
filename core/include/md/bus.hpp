#pragma once

#include "md/types.hpp"

namespace md {

// A 68000's view of memory (24-bit addresses). The main CPU's bus is Memory;
// the Mega-CD sub CPU has its own (megacd::SubBus). M68k talks only to this
// interface, so the same CPU wrapper serves both processors.
//
// Contract (same as Memory): accesses are big-endian; word/long accesses are
// even-aligned (the CPU raises address errors before calling in); long
// accesses may be split by the CPU into two word accesses.
class Bus68k {
public:
    virtual ~Bus68k() = default;

    virtual u8   read_byte(u32 address) = 0;
    virtual u16  read_word(u32 address) = 0;
    virtual u32  read_long(u32 address) {
        const u32 hi = read_word(address);
        return (hi << 16) | read_word(address + 2);
    }

    virtual void write_byte(u32 address, u8 value) = 0;
    virtual void write_word(u32 address, u16 value) = 0;
    virtual void write_long(u32 address, u32 value) {
        write_word(address, static_cast<u16>(value >> 16));
        write_word(address + 2, static_cast<u16>(value & 0xFFFF));
    }

    // Side-effect-free read for debuggers/disassemblers.
    [[nodiscard]] virtual u16 peek_word(u32 address) const noexcept = 0;
};

// A device occupying a range of a 68000 bus, addressed with full 24-bit
// addresses (so one device can serve several, non-contiguous ranges).
class BusDevice {
public:
    virtual ~BusDevice() = default;
    virtual u8   read8(u32 address) = 0;
    virtual u16  read16(u32 address) = 0;
    virtual void write8(u32 address, u8 value) = 0;
    virtual void write16(u32 address, u16 value) = 0;
};

} // namespace md
