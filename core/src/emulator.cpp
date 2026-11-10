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

Emulator::Emulator(Region region, bool overseas) : region_(region) {
    // Version register: bit 7 = overseas, bit 6 = PAL, bit 5 = no expansion
    // unit; revision 0 (no TMSS).
    memory_.set_version_register(static_cast<u8>((overseas ? 0x80 : 0x00) | (region == Region::PAL ? 0x40 : 0x00) | 0x20));

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
    vdp_.set_irq_callback([this](int level) {
        vdp_irq_level_ = level;
        update_main_irq();
    });
    vdp_.set_dma_read_callback([this](u32 address) { return memory_.read_word(address); });
    cpu_.set_interrupt_ack_handler([this](int level) {
        // Route the acknowledge to the source that requested this level.
        if (level == expansion_irq_level_ && level != vdp_irq_level_ && mega_cd_) {
            mega_cd_->acknowledge_main_interrupt(level);
        } else {
            vdp_.acknowledge_interrupt(level);
        }
    });

    reset();
}

void Emulator::load_rom(std::span<const u8> image) {
    memory_.load_rom(image);
    if (sega32x_) sega32x_->set_cartridge(memory_.rom());
    reset();
}

void Emulator::reset() {
    memory_.reset();
    vdp_.reset();
    sound_.reset();
    z80_.reset();
    z80_held_in_reset_ = true;
    z80_time_ = 0;
    if (mega_cd_) mega_cd_->reset();
    if (sega32x_) sega32x_->reset();
    sh2_clock_.reset();
    vblank_ = false;
    hblank_ = false;
    sub_clock_.reset();
    cpu_.reset();  // fetches SSP/PC from the ROM vector table (or the Mega-CD BIOS)
    vdp_irq_level_ = vdp_.irq_level();
    expansion_irq_level_ = mega_cd_ ? mega_cd_->main_irq_level() : 0;
    update_main_irq();
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

megacd::MegaCd& Emulator::attach_mega_cd(std::span<const u8> bios, bool cd_boot) {
    mega_cd_ = std::make_unique<megacd::MegaCd>();
    mega_cd_->load_bios(bios);
    mega_cd_->set_main_irq_callback([this](int level) {
        expansion_irq_level_ = level;
        update_main_irq();
    });
    install_expansion_audio();
    // Word RAM: expansion base + 0x200000-0x3FFFFF (0x200000 when booting
    // from CD, 0x600000 with a cartridge).
    vdp_.set_dma_delay_predicate([cd_boot](u32 address) {
        const u32 base = cd_boot ? 0x20'0000 : 0x60'0000;
        return (address & 0xE0'0000) == base;
    });
    cd_boot_ = cd_boot;
    memory_.attach_expansion(mega_cd_.get(), cd_boot);
    reset();
    return *mega_cd_;
}

sega32x::Sega32x& Emulator::attach_32x() {
    sega32x_ = std::make_unique<sega32x::Sega32x>();
    sega32x_->set_cartridge(memory_.rom());
    sega32x_->set_ntsc(region_ == Region::NTSC);
    // The 32X layer goes over each Mega Drive line as soon as it is drawn,
    // so mid-frame changes (palette, frame buffer flip) land on the right
    // lines. The frontend then shows the composed framebuffer as before.
    vdp_.set_line_hook([this](int line, std::span<Framebuffer::Pixel> row, std::span<const u8> backdrop) {
        sega32x_->compose_line(line, row, backdrop);
    });
    memory_.attach_32x(sega32x_.get());
    install_expansion_audio();
    reset();
    return *sega32x_;
}

void Emulator::install_expansion_audio() {
    // The mixer pulls each device's audio at each output sample's time,
    // converted to that device's timeline: sub_clock_ / sh2_clock_ hold its
    // time at master_clock_, and the sample is at most a slice or so away.
    sound_.set_expansion_audio([this](s64 master_time, s32& left, s32& right) {
        const s64 ahead = master_time - master_clock_;
        if (mega_cd_) {
            const s64 sub_time = sub_clock_.total() +
                ahead * static_cast<s64>(megacd::MegaCd::kSubCpuClockHz) / master_clock_hz();
            mega_cd_->render_audio(sub_time, left, right);
        }
        if (sega32x_) {
            const s64 sh2_time = sh2_clock_.total() + ahead * static_cast<s64>(sega32x::Sega32x::kSh2ClockNumerator) /
                                                          static_cast<s64>(sega32x::Sega32x::kSh2ClockDenominator);
            s32 l = 0, r = 0;
            sega32x_->render_audio(sh2_time, l, r);
            left += l;
            right += r;
        }
    });
}

double Emulator::sub_cpu_ratio() const noexcept {
    return megacd::MegaCd::kSubCpuClockHz * kM68kClockDivider / static_cast<double>(master_clock_hz());
}

s64 Emulator::master_clock_hz() const noexcept {
    return region_ == Region::PAL ? 53'203'424 : 53'693'175;
}

void Emulator::update_main_irq() {
    // Takes effect at the CPU's next instruction boundary: if this happens
    // while the 68000 is executing, Moira checks the IPL pins before the
    // next instruction of the same slice.
    cpu_.set_irq_level(std::max(vdp_irq_level_, expansion_irq_level_));
}

double Emulator::refresh_rate() const noexcept {
    return region_ == Region::PAL
        ? kPalMasterClock  / (kCyclesPerLine * kPalLines)
        : kNtscMasterClock / (kCyclesPerLine * kNtscLines);
}

void Emulator::run_frame() {
    sound_.clear_samples();

    if (memory_.rom().empty() && !(mega_cd_ && cd_boot_)) {
        // Nothing to run: show the test pattern so the frontend has output.
        render_test_pattern();
        ++frame_count_;
        return;
    }

    // Event-driven scheduler: the VDP defines the timeline. Each slice ends at
    // the VDP's next event (HBlank start / end of line) or after one audio
    // sample, whichever comes first. Within a slice the Z80 runs first, then
    // the main 68000, then the Mega-CD sub 68000 (if attached); then the VDP
    // processes its event, which may raise or drop interrupts for the next
    // slice.
    //
    // Mega-CD lockstep: the two 68000s communicate by polling each other's
    // gate array registers, so slices are shortened to kMegaCdSliceMainCycles
    // main-CPU cycles. The sub CPU's share of each slice comes from an exact
    // integer clock conversion (12.5 MHz vs the master clock), so the ~1.63
    // ratio holds over any duration without drift. Interrupts between the
    // CPUs are raised the moment the gate array register is written, and the
    // receiving CPU takes them at its next instruction: in the same slice for
    // main -> sub (the sub CPU runs after the main CPU), the next slice for
    // sub -> main.
    //
    // 32X lockstep: the same idea with the two SH-2s, which run after the
    // 68000 in each slice (master, then slave) at exactly 3 SH-2 cycles per
    // 68000 cycle (master clock x 3/7). The Mega Drive VDP's VBlank drives
    // the 32X's frame-buffer flip and V interrupt.
    //
    // CPU targets are absolute, so instruction overshoot in one slice is
    // automatically absorbed by the next one.
    // TODO: CPU reads of VDP state see it as of the slice start; sync lazily
    // on port access if a game needs sub-slice HV counter accuracy.
    const u64 start_frame = vdp_.frame();
    while (vdp_.frame() == start_frame) {
        int max_slice = kMaxSlice;
        if (mega_cd_) max_slice = std::min(max_slice, kMegaCdSliceMainCycles * kM68kClockDivider);
        if (sega32x_) max_slice = std::min(max_slice, k32xSliceMainCycles * kM68kClockDivider);
        const int slice = std::min({vdp_.cycles_to_next_event(), kMaxSlice, max_slice});
        const s64 target = master_clock_ + slice;

        run_z80_until(target);

        active_ = Active::M68k;
        cpu_.run_until(cpu_cycle_origin_ + target / kM68kClockDivider);
        active_ = Active::None;

        if (mega_cd_) {
            sub_clock_.advance(slice);
            active_ = Active::SubCpu;
            mega_cd_->run_sub_until(sub_clock_.total());
            active_ = Active::None;
        }

        if (sega32x_) {
            sh2_clock_.advance(slice);
            active_ = Active::Sh2;
            sega32x_->run_until(sh2_clock_.total());
            active_ = Active::None;
        }

        master_clock_ = target;
        vdp_.advance(slice);

        if (sega32x_) {
            // The vertical blanking period itself (not the status bit, which
            // also reads 1 whenever the display is off).
            const bool vblank = vdp_.line() >= vdp_.active_height();
            if (vblank != vblank_) {
                vblank_ = vblank;
                sega32x_->set_vblank(vblank);
            }
            // Slices end exactly at HBlank start (a VDP event), so the edge
            // is seen on the line it belongs to.
            const bool hblank = vdp_.line_cycle() >= Vdp::kHBlankStart;
            if (hblank != hblank_) {
                hblank_ = hblank;
                sega32x_->set_hblank(hblank);
            }
        }
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
    case Active::SubCpu: break;  // no timed devices on the sub CPU's bus yet
    case Active::Sh2: break;     // nor on the SH-2s' (PWM is to come)
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
