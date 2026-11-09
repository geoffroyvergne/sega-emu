#include "md/z80.hpp"

#include "md/memory.hpp"

extern "C" {
#include <z80.h>
}

namespace md {

namespace {

uint8_t read_byte(void* userdata, uint16_t address) {
    return static_cast<Memory*>(userdata)->z80_read(address);
}

void write_byte(void* userdata, uint16_t address, uint8_t value) {
    static_cast<Memory*>(userdata)->z80_write(address, value);
}

// The Mega Drive leaves the Z80's I/O port space unconnected.
uint8_t port_in(z80*, uint8_t) { return 0xFF; }
void port_out(z80*, uint8_t, uint8_t) {}

} // namespace

struct Z80::Impl {
    explicit Impl(Memory& m) : memory(m) {}
    Memory& memory;
    z80 cpu{};
};

Z80::Z80(Memory& memory) : impl_(std::make_unique<Impl>(memory)) {
    reset();
}

Z80::~Z80() = default;

void Z80::reset() {
    auto& z = impl_->cpu;
    const auto cycles = z.cyc;  // keep the cycle counter monotonic
    z80_init(&z);
    z.read_byte  = read_byte;
    z.write_byte = write_byte;
    z.port_in    = port_in;
    z.port_out   = port_out;
    z.userdata   = &impl_->memory;
    z.cyc = cycles;
}

int Z80::step() {
    auto& z = impl_->cpu;
    const auto before = z.cyc;
    z80_step(&z);
    return static_cast<int>(z.cyc - before);
}

void Z80::set_int_line(bool asserted) {
    // superzazu/z80 latches a pending interrupt until it is accepted; clearing
    // it when the line drops models a level-triggered /INT that the CPU can
    // miss if interrupts stay disabled for the whole pulse (as on hardware).
    auto& z = impl_->cpu;
    if (asserted) {
        z80_gen_int(&z, 0xFF);
    } else {
        z.int_pending = false;
    }
}

u16 Z80::pc() const noexcept { return impl_->cpu.pc; }
u64 Z80::cycles() const noexcept { return impl_->cpu.cyc; }

} // namespace md
