#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "md/clock.hpp"
#include "md/megacd/cdc.hpp"
#include "md/megacd/disc.hpp"
#include "md/types.hpp"

namespace md::megacd {

// The CD drive ("CDD"): the transport that spins the disc and streams sectors
// into the CDC, and the drive's own microcontroller that the sub CPU talks to
// through the gate array.
//
// Command interface (sub CPU 0xFF8000 + offset):
//   34  CD-DA fader: volume in bits 14-4 (0x400 = full); bit 15 reads 1
//         while the volume is still ramping to a new value
//   36  R: DM (bit 8, 1 = no CD-DA playing) | HOCK (bit 2)   W: HOCK (bit 2)
//         HOCK = 1 enables communication: the drive then raises INT4 every
//         1/75 s, and the BIOS answers each one by reading the status and
//         sending the next command.
//   38-41  status, 10 nibbles (one per byte, read-only):
//          [0] drive status (kStop, kPlay, ...)  [1] report type
//          [2-7] data (BCD digits)  [8] flags  [9] checksum
//   42-4B  command, 10 nibbles: [0] command  [1..8] parameters  [9] checksum
//          A word write to 0x4A (or a byte write to 0x4B) sends it.
//   Checksum: ~(sum of nibbles 0-8) & 0xF.
//
// Commands: 0 get status, 1 stop, 2 report (TOC and position; sub-code in
// nibble 3: 0 absolute time, 1 track time, 2 track number, 3 disc length,
// 4 first/last track, 5 track start (track in nibbles 4-5), 6 error),
// 3 play from MSF (nibbles 2-7), 4 seek to MSF, 6 pause, 7 resume,
// 8 fast forward, 9 rewind, A track jump, C close tray, D open tray.
//
// Timing follows Genesis Plus GX's model of the drive (which many games rely
// on): a play/seek reports "seeking" for a latency that grows with the seek
// distance, then data sectors reach the CDC one per tick at 1x (two at 2x).
// Audio tracks play as CD-DA: each tick's 588 stereo samples (44.1 kHz) go
// into a FIFO that render_cdda() drains at exactly 44.1 kHz of sub-CPU time.
// TODO: subcode (INT6).
class CdDrive {
public:
    static constexpr u64 kTicksPerSecond = 75;
    static constexpr u64 kClockHz = 12'500'000;  // timed on the sub CPU clock

    // Drive status codes (status nibble 0).
    static constexpr u8 kStop   = 0x0;
    static constexpr u8 kPlay   = 0x1;
    static constexpr u8 kSeek   = 0x2;
    static constexpr u8 kScan   = 0x3;
    static constexpr u8 kPause  = 0x4;
    static constexpr u8 kOpen   = 0x5;
    static constexpr u8 kToc    = 0x9;  // stopped, TOC read (disc ready)
    static constexpr u8 kNoDisc = 0xB;
    static constexpr u8 kEnd    = 0xC;  // played past the last track

    static constexpr u32 kRegFader   = 0x34;
    static constexpr u32 kRegControl = 0x36;
    static constexpr u32 kRegStatus  = 0x38;
    static constexpr u32 kRegCommand = 0x42;
    static constexpr u32 kRegEnd     = 0x4C;

    explicit CdDrive(Cdc& cdc) : cdc_(cdc) { reset(); }

    void reset();  // power-on: registers cleared, head at the start of the disc

    // Puts a disc in a closed drive (power-on, or tests).
    void insert(std::shared_ptr<const Disc> disc);
    void eject();
    [[nodiscard]] bool has_disc() const noexcept { return disc_ != nullptr; }

    // --- Tray ------------------------------------------------------------------
    // While the tray is open its disc is out of reach: the drive reports
    // kOpen and has no disc (no TOC, nothing to read). Closing it loads
    // whatever disc is in the tray, and the drive is ready again (kToc).
    // The CDD commands D/C open and close it too.
    static constexpr u32 kSwapTicks = 75;  // a disc change keeps the tray open 1 s
    void open_tray();
    void close_tray();
    // Changes the disc as a player would: opens the tray, swaps the disc, and
    // closes it again after kSwapTicks. Multi-disc games wait for exactly
    // this (tray open, then a new disc ready) when they ask for the next disc.
    void swap_disc(std::shared_ptr<const Disc> disc);
    [[nodiscard]] bool tray_open() const noexcept { return tray_open_; }
    [[nodiscard]] const Disc* disc() const noexcept { return disc_.get(); }

    // 1 or 2 (the Mega-CD drive is 1x; 2x is offered for faster loading).
    void set_speed(int speed) noexcept { speed_ = speed >= 2 ? 2 : 1; }
    [[nodiscard]] int speed() const noexcept { return speed_; }

    // Raised every tick while HOCK is set (the gate array's INT4).
    void set_irq_callback(std::function<void()> fn) { irq_ = std::move(fn); }

    // --- Gate array registers (offsets 0x36-0x4B) ----------------------------
    [[nodiscard]] static constexpr bool owns(u32 offset) noexcept {
        return offset >= kRegFader && offset < kRegEnd;
    }
    [[nodiscard]] u16 read_register(u32 offset) const noexcept;
    void write_register(u32 offset, u16 value, u16 lanes);

    // --- Direct control (tests, debugger): no seek latency --------------------
    void read(u32 lba);           // play from `lba`
    void pause() noexcept { status_code_ = kPause; }
    void stop() noexcept;

    [[nodiscard]] bool reading() const noexcept { return status_code_ == kPlay && latency_ == 0; }
    // Head position; negative in the 2-second pregap before sector 0.
    [[nodiscard]] s32 lba() const noexcept { return lba_; }
    [[nodiscard]] u8 status() const noexcept { return status_code_; }
    [[nodiscard]] bool hock() const noexcept { return hock_; }
    [[nodiscard]] u32 latency() const noexcept { return latency_; }
    [[nodiscard]] std::array<u8, 10> status_nibbles() const noexcept { return status_regs_; }

    // Advances the drive by `sub_cycles` of the sub CPU clock (1 tick = 1/75 s).
    void advance(s64 sub_cycles);

    // One 1/75 s step: moves the head, delivers sectors, raises INT4.
    void tick();

    // --- CD-DA output ----------------------------------------------------------
    static constexpr u32 kCddaRate = 44'100;
    static constexpr u32 kFramesPerSector = 588;    // 2352 bytes of 16-bit stereo
    static constexpr u32 kFullVolume = 0x400;

    // The CD-DA output at `sub_time` (sub-CPU cycles, non-decreasing), fader
    // applied. Consumes the samples whose time has passed.
    void render_cdda(s64 sub_time, s32& left, s32& right);
    [[nodiscard]] std::size_t cdda_buffered() const noexcept { return fifo_count_; }
    [[nodiscard]] bool audio_playing() const noexcept { return audio_playing_; }

private:
    void process_command();
    void report(u8 type);  // fills status nibbles 1-8 for a report type
    void set_time(s32 msf_frames);           // status nibbles 2-7 = MM SS FF (BCD digits)
    void set_bcd(std::size_t at, u32 value);
    void finish_status();  // nibble 9 = checksum
    [[nodiscard]] u32 track_count() const noexcept;
    [[nodiscard]] u32 lead_out() const noexcept;
    [[nodiscard]] u32 track_index_of(s32 lba) const noexcept;
    [[nodiscard]] bool data_track(u32 index) const noexcept;
    [[nodiscard]] s32 command_msf_lba() const noexcept;
    [[nodiscard]] u8 command(std::size_t i) const noexcept { return command_regs_[i] & 0x0F; }

    Cdc& cdc_;
    std::shared_ptr<const Disc> disc_;
    std::function<void()> irq_;
    int speed_ = 1;
    ClockConverter clock_{kTicksPerSecond, kClockHz};

    u8   status_code_ = kNoDisc;
    u8   pending_ = 0;      // status to enter once a play/seek starts (kPlay/kPause), 0 = none
    s32  pending_lba_ = 0;
    u32  latency_ = 0;      // ticks before the head reaches its target
    s32  lba_ = 0;
    u32  index_ = 0;        // current track (0-based)
    int  scan_offset_ = 0;
    bool hock_ = false;
    bool tray_open_ = false;
    std::shared_ptr<const Disc> tray_disc_;  // the disc in the open tray
    u32  swap_ticks_ = 0;                    // ticks until a disc change closes the tray
    bool audio_playing_ = false;  // DM = !audio_playing_

    std::array<u8, 10> status_regs_{};
    std::array<u8, 10> command_regs_{};
    RawSector sector_{};

    // CD-DA: ring buffer of stereo frames, filled per tick, drained by render_cdda.
    void push_audio(const RawSector& sector);
    static constexpr std::size_t kFifoFrames = 4096;
    std::vector<std::array<s16, 2>> fifo_ = std::vector<std::array<s16, 2>>(kFifoFrames);
    std::size_t fifo_read_ = 0;
    std::size_t fifo_count_ = 0;
    bool  priming_ = true;        // wait for 1.5 sectors before playing (absorbs jitter)
    ClockConverter cdda_clock_{kCddaRate, kClockHz};
    s64   render_time_ = 0;
    std::array<s32, 2> cdda_out_{};
    u16   fader_reg_ = 0;
    u32   volume_ = kFullVolume;  // current (ramps one step per sample towards target)
    u32   volume_target_ = kFullVolume;
};

} // namespace md::megacd
