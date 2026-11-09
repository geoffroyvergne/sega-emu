#include "md/emulator.hpp"

#include <algorithm>

namespace md {

namespace {

// Master clock: NTSC 53.693175 MHz, PAL 53.203424 MHz.
// One scanline = 3420 master cycles; 262 lines (NTSC) / 313 lines (PAL).
constexpr double kNtscMasterClock = 53'693'175.0;
constexpr double kPalMasterClock  = 53'203'424.0;
constexpr double kCyclesPerLine   = 3420.0;
constexpr double kNtscLines       = 262.0;
constexpr double kPalLines        = 313.0;

// The 68000 runs at master clock / 7 (the Z80 at / 15, see Z80::kClockDivider).
constexpr int kM68kClockDivider = 7;

// Upper bound on a scheduler slice: one audio sample. The CPUs run one after
// the other within a slice, so this bounds how far apart in time two chip
// writes from different CPUs can be reordered.
constexpr int kMaxSlice = Sound::kMasterCyclesPerSample;

} // namespace

Emulator::Emulator(Region region) : region_(region) {
    // Overseas console, no expansion unit, no TMSS; bit 6 selects PAL.
    memory_.set_version_register(region == Region::PAL ? 0xE0 : 0xA0);

    // Wiring: VDP and sound chips on the bus, VDP drives the 68000's IPL pins
    // and the Z80's /INT, VDP DMA reads through the 68000 bus, and the 68000's
    // interrupt-acknowledge cycle clears the VDP's pending flag.
    memory_.attach_vdp(&vdp_);
    memory_.attach_ym2612(&sound_.ym_device());
    memory_.attach_psg(&sound_.psg_device());
    sound_.set_time_source([this] { return current_master_time(); });
    for (int port = 0; port < kNumPorts; ++port) {
        auto& pad = pads_[static_cast<std::size_t>(port)];
        pad.set_time_source([this] { return current_master_time(); });
        memory_.attach_pad(port, &pad);
    }
    vdp_.set_z80_int_callback([this](bool asserted) { z80_.set_int_line(asserted); });
    vdp_.set_irq_callback([this](int level) { cpu_.set_irq_level(level); });
    vdp_.set_dma_read_callback([this](u32 address) { return memory_.read_word(address); });
    cpu_.set_interrupt_ack_handler([this](int level) { vdp_.acknowledge_interrupt(level); });

    reset();
}

void Emulator::load_rom(std::span<const u8> image) {
    memory_.load_rom(image);
    reset();
}

void Emulator::reset() {
    memory_.reset();
    vdp_.reset();
    sound_.reset();
    z80_.reset();
    z80_held_in_reset_ = true;
    z80_time_ = 0;
    cpu_.reset();  // fetches SSP/PC from the ROM vector table
    cpu_.set_irq_level(vdp_.irq_level());
    master_clock_ = 0;
    cpu_cycle_origin_ = cpu_.cycles();
    frame_count_ = 0;
    controllers_ = {};
    for (auto& pad : pads_) {
        pad.reset();
        pad.set_state({});
    }
}

void Emulator::set_controller(int port, ControllerState state) noexcept {
    if (port >= 0 && port < kNumPorts) {
        controllers_[static_cast<std::size_t>(port)] = state;
        pads_[static_cast<std::size_t>(port)].set_state(state);
    }
}

void Emulator::set_pad_type(Gamepad::Type type) noexcept {
    for (auto& pad : pads_) pad.set_type(type);
}

double Emulator::refresh_rate() const noexcept {
    return region_ == Region::PAL
        ? kPalMasterClock  / (kCyclesPerLine * kPalLines)
        : kNtscMasterClock / (kCyclesPerLine * kNtscLines);
}

void Emulator::run_frame() {
    sound_.clear_samples();

    if (memory_.rom().empty()) {
        // Nothing to run: show the test pattern so the frontend has output.
        render_test_pattern();
        ++frame_count_;
        return;
    }

    // Event-driven scheduler: the VDP defines the timeline. Each slice ends at
    // the VDP's next event (HBlank start / end of line) or after one audio
    // sample, whichever comes first. Within a slice the Z80 runs first, then
    // the 68000; then the VDP processes its event, which may raise or drop
    // interrupts for the next slice.
    //
    // CPU targets are absolute, so instruction overshoot in one slice is
    // automatically absorbed by the next one.
    // TODO: CPU reads of VDP state see it as of the slice start; sync lazily
    // on port access if a game needs sub-slice HV counter accuracy.
    const u64 start_frame = vdp_.frame();
    while (vdp_.frame() == start_frame) {
        const int slice = std::min(vdp_.cycles_to_next_event(), kMaxSlice);
        const s64 target = master_clock_ + slice;

        run_z80_until(target);

        active_ = Active::M68k;
        cpu_.run_until(cpu_cycle_origin_ + target / kM68kClockDivider);
        active_ = Active::None;

        master_clock_ = target;
        vdp_.advance(slice);
    }

    sound_.sync(master_clock_);  // complete the frame's audio
    ++frame_count_;
}

void Emulator::run_z80_until(s64 master_target) {
    // /RESET held: the Z80 is stopped. The YM2612's /IC pin shares this line,
    // so asserting it also resets the FM chip.
    if (memory_.z80_reset_asserted()) {
        if (!z80_held_in_reset_) {
            z80_held_in_reset_ = true;
            z80_.reset();
            sound_.sync(current_master_time());
            sound_.ym().reset();
        }
        z80_time_ = master_target;
        return;
    }
    z80_held_in_reset_ = false;

    // Bus granted to the 68000: the Z80 is paused.
    if (memory_.z80_bus_requested()) {
        z80_time_ = std::max(z80_time_, master_target);
        return;
    }

    active_ = Active::Z80;
    while (z80_time_ < master_target) {
        z80_time_ += static_cast<s64>(z80_.step()) * Z80::kClockDivider;
    }
    active_ = Active::None;
}

s64 Emulator::current_master_time() const noexcept {
    switch (active_) {
    case Active::M68k: return (cpu_.cycles() - cpu_cycle_origin_) * kM68kClockDivider;
    case Active::Z80:  return z80_time_;
    case Active::None: break;
    }
    return master_clock_;
}

// Placeholder output when no ROM is loaded: scrolling colour bars, inverted
// while Start is held on pad 1.
void Emulator::render_test_pattern() {
    static constexpr std::array<Framebuffer::Pixel, 8> kBars = {
        0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00,
        0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000,
    };

    framebuffer_.set_resolution(kMaxScreenWidth, region_ == Region::PAL ? 240 : 224);
    const int w = framebuffer_.width();
    const int h = framebuffer_.height();
    const int bar_width = w / static_cast<int>(kBars.size());
    const int offset = static_cast<int>(frame_count_ % static_cast<std::uint64_t>(w));
    const bool invert = controllers_[0].is_pressed(Button::Start);

    for (int y = 0; y < h; ++y) {
        auto row = framebuffer_.row(y);
        for (int x = 0; x < w; ++x) {
            const int bar = ((x + offset) % w) / bar_width;
            auto pixel = kBars[static_cast<std::size_t>(bar) % kBars.size()];
            if (invert) {
                pixel ^= 0x00FFFFFFu;
            }
            row[static_cast<std::size_t>(x)] = pixel;
        }
    }
}

} // namespace md
