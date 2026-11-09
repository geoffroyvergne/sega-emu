#include "md/m68k.hpp"

#include "cpu/moira_bridge.hpp"

namespace md {

class M68k::Impl {
public:
    explicit Impl(Bus68k& bus) : cpu(bus) {}
    MoiraBridge cpu;
};

M68k::M68k(Bus68k& bus) : impl_(std::make_unique<Impl>(bus)) {}

M68k::~M68k() = default;

void M68k::reset() {
    impl_->cpu.reset();
}

int M68k::step() {
    const auto before = impl_->cpu.getClock();
    impl_->cpu.execute();
    return static_cast<int>(impl_->cpu.getClock() - before);
}

void M68k::run_until(s64 target_cycle) {
    impl_->cpu.executeUntil(target_cycle);
}

s64 M68k::cycles() const noexcept {
    return impl_->cpu.getClock();
}

void M68k::set_irq_level(int level) {
    impl_->cpu.setIPL(static_cast<moira::u8>(level & 7));
}

int M68k::irq_level() const noexcept {
    return impl_->cpu.getIPL();
}

void M68k::set_interrupt_ack_handler(InterruptAckHandler handler) {
    impl_->cpu.on_interrupt_ack = std::move(handler);
}

bool M68k::is_halted() const noexcept {
    return impl_->cpu.isHalted();
}

M68k::Registers M68k::registers() const {
    const auto& cpu = impl_->cpu;
    Registers r;
    for (int i = 0; i < 8; ++i) {
        r.d[static_cast<std::size_t>(i)] = cpu.getD(i);
        r.a[static_cast<std::size_t>(i)] = cpu.getA(i);
    }
    r.pc  = cpu.getPC();
    r.sr  = cpu.getSR();
    r.usp = cpu.getUSP();
    r.ssp = cpu.getISP();  // 68000: interrupt SP == supervisor SP
    return r;
}

void M68k::set_pc(u32 pc) {
    impl_->cpu.setPC(pc);
}

std::string M68k::disassemble(u32 address, int* length) const {
    char buffer[128] = {};
    const int len = impl_->cpu.disassemble(buffer, address);
    if (length) *length = len;
    return buffer;
}

} // namespace md
