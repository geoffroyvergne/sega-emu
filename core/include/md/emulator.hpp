#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "md/framebuffer.hpp"
#include "md/input.hpp"
#include "md/m68k.hpp"
#include "md/memory.hpp"
#include "md/sound.hpp"
#include "md/vdp.hpp"
#include "md/z80.hpp"

namespace md {

// Top-level facade over the whole machine (68000, Z80, VDP, YM2612, PSG, bus).
// A frontend only talks to this class: feed it input, call run_frame() once per
// host frame, and read back the framebuffer and the frame's audio samples.
class Emulator {
public:
    static constexpr int kNumPorts = 2;

    explicit Emulator(Region region = Region::NTSC);

    // Components hold references to each other.
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    void reset();

    void load_rom(std::span<const u8> image);

    // Emulate exactly one video frame (all scanlines, including VBlank).
    void run_frame();

    void set_controller(int port, ControllerState state) noexcept;

    // Pad type on both ports (6-button by default; a few old games misread
    // 6-button pads and need 3-button).
    void set_pad_type(Gamepad::Type type) noexcept;

    [[nodiscard]] const Framebuffer& framebuffer() const noexcept { return framebuffer_; }

    // Audio produced by the last run_frame(): interleaved stereo s16 at
    // audio_sample_rate() (~888 sample frames per NTSC video frame).
    [[nodiscard]] std::span<const s16> audio_samples() const noexcept { return sound_.samples(); }
    [[nodiscard]] double audio_sample_rate() const noexcept { return Sound::sample_rate(region_); }
    [[nodiscard]] M68k&         cpu() noexcept { return cpu_; }
    [[nodiscard]] const M68k&   cpu() const noexcept { return cpu_; }
    [[nodiscard]] Z80&          z80() noexcept { return z80_; }
    [[nodiscard]] Sound&        sound() noexcept { return sound_; }
    [[nodiscard]] Vdp&          vdp() noexcept { return vdp_; }
    [[nodiscard]] const Vdp&    vdp() const noexcept { return vdp_; }
    [[nodiscard]] Memory&       memory() noexcept { return memory_; }
    [[nodiscard]] const Memory& memory() const noexcept { return memory_; }
    [[nodiscard]] Region   region()      const noexcept { return region_; }
    [[nodiscard]] std::uint64_t frame_count() const noexcept { return frame_count_; }

    // Native frame rate of the emulated console in Hz
    // (NTSC ~59.92, PAL ~49.70). Frontends should pace their loop with this.
    [[nodiscard]] double refresh_rate() const noexcept;

private:
    // Which CPU is executing, so chip accesses can be timestamped.
    enum class Active { None, M68k, Z80 };

    void render_test_pattern();
    void run_z80_until(s64 master_target);
    [[nodiscard]] s64 current_master_time() const noexcept;

    // Declaration order matters: later members reference earlier ones.
    Region      region_;
    Framebuffer framebuffer_;
    Memory      memory_;
    Vdp         vdp_{region_, framebuffer_};
    Sound       sound_;
    M68k        cpu_{memory_};
    Z80         z80_{memory_};

    s64 master_clock_     = 0;  // master cycles since the last reset (end of last slice)
    s64 cpu_cycle_origin_ = 0;  // 68000 cycle count at the last reset
    s64 z80_time_         = 0;  // master time the Z80 has run up to
    bool z80_held_in_reset_ = true;
    Active active_ = Active::None;
    std::array<ControllerState, kNumPorts> controllers_{};
    std::array<Gamepad, kNumPorts> pads_{};
    std::uint64_t frame_count_ = 0;
};

} // namespace md
