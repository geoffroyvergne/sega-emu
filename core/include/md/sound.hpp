#pragma once

#include <functional>
#include <span>
#include <vector>

#include "md/memory.hpp"
#include "md/psg.hpp"
#include "md/types.hpp"
#include "md/ym2612.hpp"

namespace md {

// Owns the two sound chips and produces the mixed stereo output.
//
// Timing: everything is in master clock cycles. One output sample is produced
// every 1008 master cycles (one YM2612 sample = 144 YM clocks x 7), which is
// ~53.27 kHz NTSC / ~52.78 kHz PAL. The PSG runs at its native rate (one tick
// per 240 master cycles) and is averaged down to the output rate.
//
// Before any chip access, the chips are brought up to the current emulated
// time (via the time source), so writes take effect at the right sample —
// essential for DAC sample playback, where the Z80 writes ~8-20k values/s.
class Sound {
public:
    static constexpr int kMasterCyclesPerSample = 7 * Ym2612::kClocksPerSample;  // 1008

    using TimeSource = std::function<s64()>;

    Sound();

    void reset();

    // Returns the current emulated time in master cycles (provided by the scheduler).
    void set_time_source(TimeSource source) { time_source_ = std::move(source); }

    // Bus-facing adapters for Memory::attach_ym2612 / attach_psg.
    [[nodiscard]] ByteDevice& ym_device() noexcept  { return ym_port_; }
    [[nodiscard]] ByteDevice& psg_device() noexcept { return psg_port_; }

    // Generates all samples up to `master_time`. Calls with an earlier time
    // than already generated are ignored (time never goes backwards).
    void sync(s64 master_time);

    // Output: interleaved stereo (L, R, L, R, ...) since the last clear.
    [[nodiscard]] std::span<const s16> samples() const noexcept { return buffer_; }
    void clear_samples() { buffer_.clear(); }

    [[nodiscard]] static double sample_rate(Region region) noexcept;

    // Direct chip access (tests, debugger). Bypasses time sync.
    [[nodiscard]] Ym2612& ym() noexcept { return ym_; }
    [[nodiscard]] Psg&    psg() noexcept { return psg_; }

private:
    class YmPort final : public ByteDevice {
    public:
        explicit YmPort(Sound& sound) : sound_(sound) {}
        u8   read(u32 port) override;
        void write(u32 port, u8 value) override;
    private:
        Sound& sound_;
    };

    class PsgPort final : public ByteDevice {
    public:
        explicit PsgPort(Sound& sound) : sound_(sound) {}
        u8   read(u32) override { return 0xFF; }  // write-only
        void write(u32 port, u8 value) override;
    private:
        Sound& sound_;
    };

    void sync_now();
    void advance_psg(s64 master_time);
    [[nodiscard]] s16 dc_block(int channel, s32 input);

    Ym2612 ym_;
    Psg    psg_;
    YmPort  ym_port_{*this};
    PsgPort psg_port_{*this};

    TimeSource time_source_;
    s64 next_sample_time_ = kMasterCyclesPerSample;  // end of the sample being built
    s64 psg_time_ = 0;                               // master time of the PSG's next tick
    std::vector<s16> buffer_;

    // DC-blocking high-pass (~10 Hz), like the console's output coupling
    // capacitor. The YM2612's DAC "ladder effect" leaves a constant offset
    // (~+500) even when silent.
    double dc_prev_in_[2]  = {0, 0};
    double dc_prev_out_[2] = {0, 0};
    bool   dc_primed_ = false;
};

} // namespace md
