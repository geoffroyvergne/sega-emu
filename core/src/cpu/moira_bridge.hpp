#pragma once

// Private header: the only place in the project that includes Moira.

#include <Moira.h>

#include "md/m68k.hpp"
#include "md/bus.hpp"

namespace md {

// Glue between Moira's bus callbacks and a 68000 bus (Memory, or the Mega-CD sub bus).
//
// Moira calls these hooks for every bus cycle. With the 68000 model and the
// default 16-bit DSACK, Moira splits long accesses into two 16-bit cycles in
// the correct order itself (including the -(An) low-word-first case), and with
// MOIRA_EMULATE_ADDRESS_ERROR enabled it raises address errors before calling
// us — matching the contract documented on Memory.
//
// Moira declares the hooks `const`; Memory is held by reference, so the
// (stateful) bus accesses are still allowed.
class MoiraBridge final : public moira::Moira {
public:
    explicit MoiraBridge(Bus68k& memory) : memory_(memory) {
        setModel(moira::Model::M68000);
        irqMode = moira::IrqMode::AUTO;  // Mega Drive peripherals use autovectors
    }

    M68k::InterruptAckHandler on_interrupt_ack;

private:
    // --- Bus reads ---------------------------------------------------------
    moira::u8  read8(moira::u32 addr)  const override { return memory_.read_byte(addr); }
    moira::u16 read16(moira::u32 addr) const override { return memory_.read_word(addr); }
    moira::u32 read32(moira::u32 addr) const override { return memory_.read_long(addr); }

    // Disassembler fetches must not trigger device side effects.
    moira::u16 read16Dasm(moira::u32 addr) const override { return memory_.peek_word(addr); }

    // --- Bus writes --------------------------------------------------------
    void write8(moira::u32 addr, moira::u8 val)   const override { memory_.write_byte(addr, val); }
    void write16(moira::u32 addr, moira::u16 val) const override { memory_.write_word(addr, val); }
    void write32(moira::u32 addr, moira::u32 val) const override { memory_.write_long(addr, val); }

    // --- Interrupts --------------------------------------------------------
    // Invoked at the start of interrupt processing (the IACK cycle).
    void willInterrupt(moira::u8 level) override {
        if (on_interrupt_ack) on_interrupt_ack(level);
    }

    // TODO: TAS on the Mega Drive does not perform its write-back cycle (the
    // bus arbiter ignores the locked RMW). Some games (e.g. Gargoyles) rely on
    // this. Hook willExecute(..., Instr::TAS, ...) once a game needs it.

    Bus68k& memory_;
};

} // namespace md
