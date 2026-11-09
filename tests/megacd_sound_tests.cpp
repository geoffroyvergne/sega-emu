// Mega-CD sound: the RF5C164 PCM chip, CD-DA playback, and their mix into
// the console's audio output.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "md/emulator.hpp"
#include "md/megacd/megacd.hpp"
#include "test.hpp"

using md::s16;
using md::s32;
using md::s64;
using md::u16;
using md::u32;
using md::u8;
using namespace md::megacd;

namespace {

constexpr u32 kPcm = 0xFF0000;

// PCM registers, as the sub CPU writes them (odd bytes).
void pcm_reg(MegaCd& cd, int reg, u8 value) { cd.sub_bus().write_byte(kPcm + 1 + 2u * static_cast<u32>(reg), value); }
u8 pcm_addr_byte(MegaCd& cd, int channel, bool high) {
    return cd.sub_bus().read_byte(kPcm + 0x21 + 4u * static_cast<u32>(channel) + (high ? 2u : 0u));
}

constexpr int kEnv = 0, kPan = 1, kFdl = 2, kFdh = 3, kLsl = 4, kLsh = 5, kSt = 6, kCtrl = 7, kOnOff = 8;
constexpr u8 kCtrlOn = 0x80, kCtrlChannel = 0x40;

// Writes `bytes` into wave RAM at `address` through the 4 KB window.
void pcm_poke(MegaCd& cd, u32 address, const std::vector<u8>& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const u32 a = address + static_cast<u32>(i);
        pcm_reg(cd, kCtrl, static_cast<u8>((a >> 12) & 0x0F));          // bank, sound off
        cd.sub_bus().write_byte(kPcm + 0x2001 + 2 * (a & 0x0FFF), bytes[i]);
    }
}

// Channel 0: full volume, both sides, 1 byte per sample, from `start`, loop at `loop`.
void setup_channel0(MegaCd& cd, u8 start_page, u16 loop) {
    pcm_reg(cd, kCtrl, kCtrlChannel | 0);
    pcm_reg(cd, kEnv, 0xFF);
    pcm_reg(cd, kPan, 0xFF);
    pcm_reg(cd, kFdl, 0x00);
    pcm_reg(cd, kFdh, 0x08);
    pcm_reg(cd, kLsl, static_cast<u8>(loop));
    pcm_reg(cd, kLsh, static_cast<u8>(loop >> 8));
    pcm_reg(cd, kSt, start_page);
}

struct TempFile {
    std::string path;
    TempFile(std::string p, const std::vector<u8>& data) : path(std::move(p)) {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    TempFile(std::string p, const std::string& text) : path(std::move(p)) { std::ofstream(path) << text; }
    ~TempFile() { std::remove(path.c_str()); }
};

} // namespace

// ==============================================================================
// PCM chip
// ==============================================================================

TEST(pcm_wave_ram_banks) {
    MegaCd cd;
    pcm_poke(cd, 0x0010, {0x11});
    pcm_poke(cd, 0x3010, {0x33});
    CHECK(cd.pcm().ram(0x0010) == 0x11);
    CHECK(cd.pcm().ram(0x3010) == 0x33);
    pcm_reg(cd, kCtrl, 0x03);                                        // window on bank 3
    CHECK(cd.sub_bus().read_byte(kPcm + 0x2001 + 0x20) == 0x33);
    CHECK(cd.sub_bus().read_byte(kPcm + 0x2000 + 0x20) == 0x00);     // even byte: nothing there
    CHECK(cd.sub_bus().read_byte(kPcm + 0x4000 + 0x2001 + 0x20) == 0x33);  // mirrored every 16 KB
}

TEST(pcm_channel_plays_loops_and_reports_its_address) {
    MegaCd cd;
    cd.pcm().set_lowpass(0);                                         // exact chip output
    // +100 for 4 samples, then a loop marker; the loop is -50 then a marker.
    pcm_poke(cd, 0x0100, {0x80 | 100, 0x80 | 100, 0x80 | 100, 0x80 | 100, 0xFF});
    pcm_poke(cd, 0x0200, {50, 0xFF});                                // sign bit clear: -50
    setup_channel0(cd, 0x01, 0x0200);
    pcm_reg(cd, kOnOff, 0xFE);                                       // channel 0 on
    pcm_reg(cd, kCtrl, kCtrlOn | kCtrlChannel);                      // sound on

    // level x env x pan >> 5, per side.
    constexpr s32 kHigh = (100 * 255 * 15) >> 5;
    constexpr s32 kLow  = (-50 * 255 * 15) >> 5;                     // arithmetic shift: rounds down
    std::vector<s32> out;
    s32 l = 0, r = 0;
    for (int n = 1; n <= 8; ++n) {
        // Interpolation lags one chip sample: at sample n+1's time, the output
        // is exactly sample n.
        cd.pcm().output_at((n + 1) * Pcm::kCyclesPerSample, l, r);
        CHECK(l == r);
        out.push_back(l);
    }
    // Samples: 4 x high, marker -> jumps to the loop (-50), marker -> -50 again...
    CHECK(out[0] == kHigh && out[3] == kHigh);
    CHECK(out[4] == kLow && out[5] == kLow && out[7] == kLow);
    CHECK(pcm_addr_byte(cd, 0, true) == 0x02);                       // playing around 0x0200

    // Off: held at its start address, silent.
    pcm_reg(cd, kOnOff, 0xFF);
    cd.pcm().output_at(11 * Pcm::kCyclesPerSample, l, r);            // the first sample made while off
    CHECK(l == 0);
    CHECK(pcm_addr_byte(cd, 0, true) == 0x01 && pcm_addr_byte(cd, 0, false) == 0x00);
}

TEST(pcm_runs_at_32552_hz_and_pans) {
    MegaCd cd;
    setup_channel0(cd, 0x00, 0x0000);
    pcm_reg(cd, kPan, 0x0F);                                         // left only
    pcm_poke(cd, 0, std::vector<u8>(16, 0x80 | 64));
    pcm_reg(cd, kOnOff, 0xFE);
    pcm_reg(cd, kCtrl, kCtrlOn | kCtrlChannel);

    s32 l = 0, r = 0;
    cd.pcm().output_at(2 * Pcm::kCyclesPerSample, l, r);
    CHECK(l > 0 && r == 0);

    cd.pcm().run_until(12'500'000);                                  // one second
    const u32 played = cd.pcm().channel_address(0);
    CHECK(played == 12'500'000 / 384);                               // 32,552 bytes at 1 byte/sample
    if (std::getenv("MD_TEST_VERBOSE")) std::printf("\n  PCM bytes played in 1 s: %u\n", played);
}

TEST(pcm_sound_off_is_silent) {
    MegaCd cd;
    setup_channel0(cd, 0x00, 0x0000);
    pcm_poke(cd, 0, std::vector<u8>(16, 0x80 | 64));
    pcm_reg(cd, kOnOff, 0xFE);
    pcm_reg(cd, kCtrl, kCtrlChannel);                                // ON bit clear
    s32 l = 1, r = 1;
    cd.pcm().output_at(5 * Pcm::kCyclesPerSample, l, r);
    CHECK(l == 0 && r == 0);
}

TEST(pcm_keeps_samples_made_ahead_of_the_reader) {
    // A register write runs the chip up to the sub CPU's time, but the mixer
    // reads the output later (at the end of the frame), for earlier times:
    // those samples must still be there, not replaced by the latest one.
    const auto play = [](bool run_ahead) {
        MegaCd cd;
        std::vector<u8> wave;
        for (int i = 0; i < 400; ++i) wave.push_back(static_cast<u8>((i % 2 ? 0x80 : 0) | (i % 97)));
        wave.push_back(0xFF);
        pcm_poke(cd, 0, wave);
        setup_channel0(cd, 0x00, 0x0000);
        pcm_reg(cd, kOnOff, 0xFE);
        pcm_reg(cd, kCtrl, kCtrlOn | kCtrlChannel);
        if (run_ahead) cd.pcm().run_until(300 * Pcm::kCyclesPerSample);
        std::vector<s32> out;
        s32 l = 0, r = 0;
        for (s64 t = 0; t < 300 * Pcm::kCyclesPerSample; t += 237) {  // the mixer's rate, roughly
            cd.pcm().output_at(t, l, r);
            out.push_back(l);
        }
        CHECK(cd.pcm().buffered() <= 2);                             // what was read is released
        return out;
    };
    const auto reference = play(false);
    CHECK(play(true) == reference);
    CHECK(std::count(reference.begin(), reference.end(), reference.back()) < 10);  // not a held value
}

TEST(pcm_output_lowpass_filter) {
    // A tone at half the chip rate (16.3 kHz, alternating +/-) against a
    // 1 kHz one: the default filter keeps the low tone and cuts the high one.
    const auto amplitude = [](double cutoff, u32 period) {
        MegaCd cd;
        cd.pcm().set_lowpass(cutoff);
        std::vector<u8> wave(period * 64);
        for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = (i % period) < period / 2 ? (0x80 | 100) : 100;
        wave.push_back(0xFF);
        pcm_poke(cd, 0, wave);
        setup_channel0(cd, 0x00, 0x0000);
        pcm_reg(cd, kOnOff, 0xFE);
        pcm_reg(cd, kCtrl, kCtrlOn | kCtrlChannel);
        s32 peak = 0, l = 0, r = 0;
        for (s64 n = 2; n < static_cast<s64>(wave.size()); ++n) {
            cd.pcm().output_at(n * Pcm::kCyclesPerSample, l, r);
            if (n > static_cast<s64>(period) * 8) peak = std::max(peak, std::abs(l));  // after settling
        }
        return peak;
    };
    const s32 raw_high = amplitude(0, 2);
    const s32 high = amplitude(Pcm::kDefaultLowpassHz, 2);
    const s32 low = amplitude(Pcm::kDefaultLowpassHz, 32);
    const s32 raw_low = amplitude(0, 32);
    if (std::getenv("MD_TEST_VERBOSE")) {
        std::printf("\n  PCM lowpass: 16.3 kHz %d -> %d, 1 kHz %d -> %d\n", raw_high, high, raw_low, low);
    }
    CHECK(raw_high == -((-100 * 255 * 15) >> 5));                    // off: untouched (negative peak)
    CHECK(high < raw_high / 10);                                     // cut
    CHECK(low > raw_low * 9 / 10);                                   // kept (square wave: may overshoot)
}

// ==============================================================================
// CD-DA
// ==============================================================================

namespace {

// An audio-only disc: `sectors` sectors of a constant stereo sample.
struct AudioDisc {
    std::vector<u8> pcm;
    TempFile bin;
    TempFile cue;
    AudioDisc(const char* stem, u32 sectors, s16 left, s16 right)
        : pcm(make(sectors, left, right)),
          bin(std::string(stem) + ".bin", pcm),
          cue(std::string(stem) + ".cue",
              "FILE \"" + std::string(stem) + ".bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n") {}
    static std::vector<u8> make(u32 sectors, s16 left, s16 right) {
        std::vector<u8> data;
        for (u32 i = 0; i < sectors * 588; ++i) {
            for (s16 v : {left, right}) {
                data.push_back(static_cast<u8>(v));
                data.push_back(static_cast<u8>(static_cast<u16>(v) >> 8));
            }
        }
        return data;
    }
};

constexpr s64 kTick = 12'500'000 / 75 + 1;

} // namespace

TEST(cdda_plays_audio_tracks_through_the_fader) {
    AudioDisc disc{"cdda_test", 300, 8000, -4000};
    std::string error;
    MegaCd cd;
    cd.insert_disc(Disc::open(disc.cue.path, error));
    CHECK(error.empty());

    CHECK(cd.sub_bus().read_word(0xFF8036) & 0x0100);                // DM: no CD-DA
    cd.drive().read(0);
    s64 t = 0;
    s32 l = 0, r = 0;
    for (int i = 0; i < 4; ++i) {                                    // fill the FIFO
        t += kTick;
        cd.run_sub_until(t);
        cd.render_audio(t, l, r);
    }
    CHECK(cd.drive().audio_playing());
    CHECK((cd.sub_bus().read_word(0xFF8036) & 0x0100) == 0);         // DM: CD-DA playing
    t += 1000;
    cd.run_sub_until(t);
    cd.render_audio(t, l, r);
    CHECK(l == 8000 / 2 && r == -4000 / 2);                          // mixed at half level

    // Fader to half volume (0x200 in bits 14-4): ramps one step per sample.
    cd.sub_bus().write_word(0xFF8034, 0x200 << 4);
    CHECK(cd.sub_bus().read_word(0xFF8034) & 0x8000);                // still ramping
    for (int i = 0; i < 3; ++i) {
        t += kTick;
        cd.run_sub_until(t);
        cd.render_audio(t, l, r);
    }
    CHECK((cd.sub_bus().read_word(0xFF8034) & 0x8000) == 0);
    CHECK(l == 8000 / 2 / 2 && r == -4000 / 2 / 2);

    // Paused: the FIFO drains, then silence.
    cd.drive().pause();
    for (int i = 0; i < 4; ++i) {
        t += kTick;
        cd.run_sub_until(t);
        cd.render_audio(t, l, r);
    }
    CHECK(l == 0 && r == 0);
}

TEST(cdda_rate_is_exactly_44100_hz) {
    AudioDisc disc{"cdda_rate", 300, 100, 100};
    std::string error;
    MegaCd cd;
    cd.insert_disc(Disc::open(disc.cue.path, error));
    cd.drive().read(0);
    // Render at the mixer's ~53 kHz for 2 seconds; production (75 sectors/s)
    // and consumption must balance, so the FIFO stays bounded.
    std::size_t lowest = 1u << 30, highest = 0;
    s32 l = 0, r = 0;
    for (s64 t = 0; t < 25'000'000; t += 235) {
        cd.run_sub_until(t);
        cd.render_audio(t, l, r);
        if (t > 2'000'000) {
            lowest = std::min(lowest, cd.drive().cdda_buffered());
            highest = std::max(highest, cd.drive().cdda_buffered());
        }
    }
    CHECK(lowest > 0);                                               // never ran dry
    CHECK(highest < 2 * 588);
    CHECK(l == 50);
}

// ==============================================================================
// Mixed into the console's output
// ==============================================================================

TEST(megacd_pcm_reaches_the_audio_output) {
    std::vector<u8> bios(MegaCd::kBiosSize, 0);
    const u16 boot[] = {0x00FF, 0xFE00, 0x0000, 0x0200};             // SSP, PC = 0x200
    for (std::size_t i = 0; i < 4; ++i) {
        bios[2 * i] = static_cast<u8>(boot[i] >> 8);
        bios[2 * i + 1] = static_cast<u8>(boot[i]);
    }
    bios[0x200] = 0x60; bios[0x201] = 0xFE;                          // bra.s *

    md::Emulator emu;
    auto& cd = emu.attach_mega_cd(bios, true);
    // A 325 Hz square wave: 50 samples high, 50 low, looping.
    std::vector<u8> wave(100);
    for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = i < 50 ? (0x80 | 100) : 100;
    wave.push_back(0xFF);
    pcm_poke(cd, 0, wave);
    setup_channel0(cd, 0x00, 0x0000);
    pcm_reg(cd, kOnOff, 0xFE);
    pcm_reg(cd, kCtrl, kCtrlOn | kCtrlChannel);

    std::vector<s16> left;
    for (int f = 0; f < 10; ++f) {
        emu.run_frame();
        const auto s = emu.audio_samples();
        for (std::size_t i = 0; i < s.size(); i += 2) left.push_back(s[i]);
    }
    double sum = 0;
    int crossings = 0;
    for (std::size_t i = 1; i < left.size(); ++i) {
        sum += double(left[i]) * left[i];
        if ((left[i - 1] < 0) != (left[i] < 0)) ++crossings;
    }
    const double rms = std::sqrt(sum / static_cast<double>(left.size()));
    const double seconds = static_cast<double>(left.size()) / emu.audio_sample_rate();
    const double freq = crossings / 2.0 / seconds;
    if (std::getenv("MD_TEST_VERBOSE")) std::printf("\n  PCM through the mixer: rms %.0f, %.1f Hz\n", rms, freq);
    CHECK(rms > 3000);                                               // ~ (100*255*15>>5) / 2
    CHECK(std::abs(freq - 32552.0 / 100) < 5);
}
