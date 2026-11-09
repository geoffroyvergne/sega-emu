// SDL3 frontend: owns the window, input and timing. All emulation lives in md::core.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "md/emulator.hpp"
#include "md/rom_loader.hpp"
#include "md/zip_archive.hpp"

#include "input.hpp"

namespace {

constexpr int kWindowScale = 3;

// RAII wrappers so every early return cleans up correctly.
struct SdlDeleter {
    void operator()(SDL_Window*   p) const noexcept { SDL_DestroyWindow(p); }
    void operator()(SDL_Renderer* p) const noexcept { SDL_DestroyRenderer(p); }
    void operator()(SDL_Texture*  p) const noexcept { SDL_DestroyTexture(p); }
    void operator()(SDL_AudioStream* p) const noexcept { SDL_DestroyAudioStream(p); }
};
using WindowPtr   = std::unique_ptr<SDL_Window,   SdlDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, SdlDeleter>;
using TexturePtr  = std::unique_ptr<SDL_Texture,  SdlDeleter>;
using AudioPtr    = std::unique_ptr<SDL_AudioStream, SdlDeleter>;

struct SdlSession {
    bool ok = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD);
    ~SdlSession() { SDL_Quit(); }
};

// Audio output. The core produces ~53.3 kHz stereo; SDL's audio stream
// resamples to the device rate. Video and audio are paced by different
// clocks, so the queue slowly drifts: we nudge the playback speed (by at most
// +/-0.5%, inaudible) to keep about kTargetLatency of audio queued.
class AudioOutput {
public:
    static constexpr double kTargetLatency = 0.060;  // seconds

    explicit AudioOutput(double sample_rate) : rate_(sample_rate) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(std::lround(sample_rate))};
        stream_.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr));
        if (!stream_) {
            std::fprintf(stderr, "Audio disabled: %s\n", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(stream_.get());
    }

    void push(std::span<const md::s16> samples) {
        if (!stream_ || samples.empty()) return;

        const double queued = queued_seconds();
        if (queued > 4 * kTargetLatency) {
            SDL_ClearAudioStream(stream_.get());  // way behind (e.g. after a stall): drop
        }

        if (!muted_) {
            SDL_PutAudioStreamData(stream_.get(), samples.data(),
                                   static_cast<int>(samples.size_bytes()));
        } else {
            silence_.assign(samples.size(), 0);
            SDL_PutAudioStreamData(stream_.get(), silence_.data(),
                                   static_cast<int>(samples.size_bytes()));
        }

        // Proportional rate control: too much queued -> play slightly faster.
        const double error = (queued - kTargetLatency) / kTargetLatency;
        const double ratio = 1.0 + std::clamp(error * 0.005, -0.005, 0.005);
        SDL_SetAudioStreamFrequencyRatio(stream_.get(), static_cast<float>(ratio));
    }

    void toggle_mute() { muted_ = !muted_; }
    [[nodiscard]] bool muted() const noexcept { return muted_; }

private:
    [[nodiscard]] double queued_seconds() const {
        const int bytes = SDL_GetAudioStreamQueued(stream_.get());
        return bytes / (rate_ * 2 * sizeof(md::s16));
    }

    AudioPtr stream_;
    double rate_;
    bool muted_ = false;
    std::vector<md::s16> silence_;
};

// Minimal WAV writer (16-bit stereo PCM) for --wav.
bool write_wav(const char* path, const std::vector<md::s16>& samples, int sample_rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto put32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * sizeof(md::s16));
    // Note: WAV is little-endian; this writer assumes a little-endian host.
    out.write("RIFF", 4); put32(36 + data_bytes); out.write("WAVE", 4);
    out.write("fmt ", 4); put32(16); put16(1); put16(2);
    put32(static_cast<std::uint32_t>(sample_rate));
    put32(static_cast<std::uint32_t>(sample_rate * 4)); put16(4); put16(16);
    out.write("data", 4); put32(data_bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return static_cast<bool>(out);
}

// Writes the active area of the framebuffer as a binary PPM (P6).
bool write_ppm(const md::Framebuffer& fb, const char* path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << fb.width() << ' ' << fb.height() << "\n255\n";
    for (int y = 0; y < fb.height(); ++y) {
        for (int x = 0; x < fb.width(); ++x) {
            const auto p = fb.pixels()[static_cast<std::size_t>(y * md::Framebuffer::kStride + x)];
            const char rgb[3] = {static_cast<char>((p >> 16) & 0xFF),
                                 static_cast<char>((p >> 8) & 0xFF),
                                 static_cast<char>(p & 0xFF)};
            out.write(rgb, 3);
        }
    }
    return static_cast<bool>(out);
}

// Fullscreen hotkeys: F11, Alt/Option+Enter, Cmd+F (macOS convention).
bool is_fullscreen_toggle(const SDL_KeyboardEvent& key) {
    if (key.repeat) return false;
    if (key.scancode == SDL_SCANCODE_F11) return true;
    if (key.scancode == SDL_SCANCODE_RETURN && (key.mod & SDL_KMOD_ALT)) return true;
    if (key.scancode == SDL_SCANCODE_F && (key.mod & SDL_KMOD_GUI)) return true;
    return false;
}

// How the game image is scaled into the window (cycled with F2).
enum class ScaleMode { Fit, Stretch, Integer };

const char* to_string(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::Fit:     return "Fit";
    case ScaleMode::Stretch: return "Stretch";
    case ScaleMode::Integer: return "Integer";
    }
    return "?";
}

ScaleMode next(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::Fit:     return ScaleMode::Stretch;
    case ScaleMode::Stretch: return ScaleMode::Integer;
    case ScaleMode::Integer: return ScaleMode::Fit;
    }
    return ScaleMode::Fit;
}

//   Fit:     as large as possible with the game's aspect ratio. In a window,
//            the window's shape is locked to that ratio, so it is filled
//            edge to edge. Fullscreen gets bars on the sides.
//   Stretch: fills the window/screen completely; distorts the image.
//   Integer: whole-number scale only; sharpest, may leave borders.
void apply_presentation(SDL_Window* window, SDL_Renderer* renderer,
                        int w, int h, bool fullscreen, ScaleMode mode) {
    if (w <= 0 || h <= 0) return;  // no frame yet

    SDL_RendererLogicalPresentation presentation = SDL_LOGICAL_PRESENTATION_LETTERBOX;
    if (mode == ScaleMode::Stretch) presentation = SDL_LOGICAL_PRESENTATION_STRETCH;
    if (mode == ScaleMode::Integer) presentation = SDL_LOGICAL_PRESENTATION_INTEGER_SCALE;
    SDL_SetRenderLogicalPresentation(renderer, w, h, presentation);

    if (!fullscreen && mode == ScaleMode::Fit) {
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        SDL_SetWindowAspectRatio(window, aspect, aspect);
        // Snap the current window to that shape, keeping its height.
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        SDL_SetWindowSize(window, static_cast<int>(static_cast<float>(win_h) * aspect + 0.5f), win_h);
    } else {
        SDL_SetWindowAspectRatio(window, 0.0f, 0.0f);  // free resizing
    }

    char title[64];
    std::snprintf(title, sizeof title, "Mega Drive  [%s]", to_string(mode));
    SDL_SetWindowTitle(window, title);
}

void print_usage() {
    std::fprintf(stderr,
        "usage: megadrive [--fullscreen] [--3button] [--region pal|ntsc|jp] [--bios megacd_bios.bin] [rom.bin]\n"
        "         --region: console region (default: read from the cartridge or BIOS header)\n"
        "         --pcm-lowpass <Hz>: Mega-CD PCM output filter cutoff (default 7000, 0 = off)\n"
        "         --bios: attach a Mega-CD with this BIOS; without a ROM, boot it (CD boot)\n"
        "         --disc: insert a CD image (.cue, .iso or raw .bin) into the Mega-CD;\n"
        "                 to change discs while playing, drop the next one on the window\n"
        "       megadrive --frames <n> [--screenshot <out.ppm>] [--wav <out.wav>] <rom.bin>\n"
        "         headless: runs <n> frames without a window, then saves the last\n"
        "         frame and/or all the audio produced\n");
}

int fail(const char* what) {
    std::fprintf(stderr, "%s failed: %s\n", what, SDL_GetError());
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    // --- Command line --------------------------------------------------------
    const char* rom_path = nullptr;
    const char* screenshot_path = nullptr;
    const char* wav_path = nullptr;
    long headless_frames = 0;
    bool start_fullscreen = false;
    bool three_button = false;
    double pcm_lowpass = md::megacd::Pcm::kDefaultLowpassHz;
    const char* bios_path = nullptr;
    const char* disc_path = nullptr;
    const char* region_arg = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            headless_frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        } else if (std::strcmp(argv[i], "--region") == 0 && i + 1 < argc) {
            region_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--pcm-lowpass") == 0 && i + 1 < argc) {
            pcm_lowpass = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--3button") == 0) {
            three_button = true;
        } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
            start_fullscreen = true;
        } else if (argv[i][0] == '-') {
            print_usage();
            return 1;
        } else {
            rom_path = argv[i];
        }
    }
    const bool headless = headless_frames > 0 || screenshot_path || wav_path;
    if (headless && ((!rom_path && !bios_path) || headless_frames <= 0)) {
        print_usage();
        return 1;
    }

    // --- Load files (before creating the console: they decide its region) ---
    md::RomImage rom;
    if (rom_path) {
        rom = md::load_rom_file(rom_path);  // raw image or ZIP archive
        if (!rom.ok()) {
            std::fprintf(stderr, "Cannot load ROM: %s\n", rom.error.c_str());
            return 1;
        }
    }
    md::RomImage bios;
    if (bios_path) {
        bios = md::load_rom_file(bios_path);
        if (!bios.ok()) {
            std::fprintf(stderr, "Cannot load Mega-CD BIOS: %s\n", bios.error.c_str());
            return 1;
        }
    }

    // Region: --region, else the header of what boots (cartridge, or the
    // Mega-CD BIOS when booting a CD), else NTSC overseas.
    md::ConsoleRegion region;
    const char* region_source = "default";
    if (region_arg) {
        const std::string r = region_arg;
        if (r == "pal" || r == "eu")       region = {md::Region::PAL, true};
        else if (r == "ntsc" || r == "us") region = {md::Region::NTSC, true};
        else if (r == "jp")                region = {md::Region::NTSC, false};
        else { print_usage(); return 1; }
        region_source = "--region";
    } else if (const auto& boot = rom_path ? rom : bios; !boot.data.empty()) {
        if (const auto detected = md::detect_region(boot.data)) {
            region = *detected;
            region_source = rom_path ? "cartridge header" : "BIOS header";
        }
    }
    std::printf("Console: %s %s (%s)\n", region.video == md::Region::PAL ? "PAL 50 Hz" : "NTSC 60 Hz",
                region.overseas ? "overseas" : "Japan", region_source);

    md::Emulator emu{region.video, region.overseas};
    if (three_button) emu.set_pad_type(md::Gamepad::Type::ThreeButton);

    if (bios_path) {
        const bool cd_boot = rom_path == nullptr;
        emu.attach_mega_cd(bios.data, cd_boot).pcm().set_lowpass(pcm_lowpass);
        std::printf("Mega-CD attached (%s boot), BIOS %s\n", cd_boot ? "CD" : "cartridge", bios.name.c_str());
    }
    if (disc_path) {
        if (!emu.mega_cd()) {
            std::fprintf(stderr, "--disc needs a Mega-CD: add --bios <file>\n");
            return 1;
        }
        std::string error;
        if (md::file_is_zip(disc_path)) {
            std::printf("Unpacking disc archive into %s (first run only)...\n",
                        md::megacd::Disc::default_cache_dir().c_str());
        }
        auto disc = md::megacd::Disc::open(disc_path, error);
        if (!disc) {
            std::fprintf(stderr, "Cannot load disc: %s\n", error.c_str());
            return 1;
        }
        std::printf("Disc %s: %zu track(s), %u sectors\n", disc_path, disc->tracks().size(), disc->sector_count());
        emu.mega_cd()->insert_disc(std::move(disc));
    }

    if (rom_path) {
        emu.load_rom(rom.data);
        const auto regs = emu.cpu().registers();
        std::printf("Loaded %s (%zu bytes): SSP=%06X PC=%06X  %s\n", rom.name.c_str(), rom.data.size(),
                    regs.ssp, regs.pc, emu.cpu().disassemble(regs.pc).c_str());
    }
    const auto& fb = emu.framebuffer();

    // --- Headless mode: no SDL at all ----------------------------------------
    if (headless) {
        std::vector<md::s16> audio;
        for (long n = 0; n < headless_frames; ++n) {
            emu.run_frame();
            if (wav_path) {
                const auto s = emu.audio_samples();
                audio.insert(audio.end(), s.begin(), s.end());
            }
        }
        const auto regs = emu.cpu().registers();
        std::printf("Ran %ld frames, PC=%06X  %s\n", headless_frames, regs.pc,
                    emu.cpu().disassemble(regs.pc).c_str());
        if (auto* cd = emu.mega_cd()) {
            const auto sub = cd->sub_cpu().registers();
            std::printf("Sub CPU PC=%06X  %s\n", sub.pc, cd->sub_cpu().disassemble(sub.pc).c_str());
        }

        if (screenshot_path) {
            if (!write_ppm(fb, screenshot_path)) {
                std::fprintf(stderr, "Cannot write %s\n", screenshot_path);
                return 1;
            }
            std::printf("Wrote %s (%dx%d)\n", screenshot_path, fb.width(), fb.height());
        }
        if (wav_path) {
            const int rate = static_cast<int>(std::lround(emu.audio_sample_rate()));
            if (!write_wav(wav_path, audio, rate)) {
                std::fprintf(stderr, "Cannot write %s\n", wav_path);
                return 1;
            }
            std::printf("Wrote %s (%.1f s at %d Hz)\n", wav_path,
                        static_cast<double>(audio.size() / 2) / rate, rate);
        }
        return 0;
    }

    SdlSession sdl;
    if (!sdl.ok) return fail("SDL_Init");

    // Sized for the common 320x224 mode, not the VDP's 256-wide reset state:
    // games switch to H40 within the first frames.
    WindowPtr window{SDL_CreateWindow("Mega Drive",
                                      md::kMaxScreenWidth * kWindowScale, 224 * kWindowScale,
                                      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    if (!window) return fail("SDL_CreateWindow");
    if (start_fullscreen) SDL_SetWindowFullscreen(window.get(), true);

    RendererPtr renderer{SDL_CreateRenderer(window.get(), nullptr)};
    if (!renderer) return fail("SDL_CreateRenderer");

    // We pace frames ourselves at the console's rate; vsync would double-throttle.
    SDL_SetRenderVSync(renderer.get(), 0);

    TexturePtr texture{SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         md::kMaxScreenWidth, md::kMaxScreenHeight)};
    if (!texture) return fail("SDL_CreateTexture");
#if SDL_VERSION_ATLEAST(3, 4, 0)
    // Sharp pixels at non-integer scales, without nearest-neighbour's uneven rows.
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_PIXELART);
#else
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST);
#endif

    AudioOutput audio{emu.audio_sample_rate()};
    frontend::InputManager input;  // gamepads already plugged in arrive as ADDED events

    // Fixed timestep: one emulated frame per tick (~59.92 Hz NTSC, ~49.70 Hz PAL).
    using Clock = std::chrono::steady_clock;
    const auto frame_duration = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / emu.refresh_rate()));
    constexpr auto kMaxLag = std::chrono::milliseconds(250);

    int logical_w = 0;
    int logical_h = 0;
    ScaleMode scale_mode = ScaleMode::Fit;
    bool fullscreen = false;  // updated from window events, so it also tracks
                              // the OS's own controls (e.g. macOS green button)
    auto next_frame = Clock::now();
    bool running = true;

    // Disc changes by drag and drop (multi-disc games). Opening a disc may
    // unpack a ZIP (seconds), so it runs on a worker thread while the game
    // keeps going; the swap happens when it's ready.
    struct LoadedDisc {
        std::shared_ptr<md::megacd::Disc> disc;
        std::string path;
        std::string error;
    };
    std::future<LoadedDisc> disc_loading;
    const auto on_drop = [&](const char* path) {
        if (!emu.mega_cd()) {
            std::printf("Dropped %s: disc changes need a Mega-CD (start with --bios)\n", path);
            return;
        }
        if (disc_loading.valid()) {
            std::printf("Dropped %s: still loading the previous disc, try again in a moment\n", path);
            return;
        }
        std::printf("Loading disc %s%s...\n", path, md::file_is_zip(path) ? " (unpacking, first time only)" : "");
        disc_loading = std::async(std::launch::async, [p = std::string(path)] {
            LoadedDisc loaded;
            loaded.path = p;
            loaded.disc = md::megacd::Disc::open(p, loaded.error);
            return loaded;
        });
    };

    while (running) {
        // --- Events ---------------------------------------------------------
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (const auto message = input.handle_event(event); !message.empty()) {
                std::printf("%s\n", message.c_str());
            }
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.scancode == SDL_SCANCODE_F2 && !event.key.repeat) {
                    scale_mode = next(scale_mode);
                    apply_presentation(window.get(), renderer.get(), logical_w, logical_h,
                                       fullscreen, scale_mode);
                } else if (event.key.scancode == SDL_SCANCODE_M && !event.key.repeat) {
                    audio.toggle_mute();
                    std::printf("Sound %s\n", audio.muted() ? "muted" : "on");
                } else if (is_fullscreen_toggle(event.key)) {
                    SDL_SetWindowFullscreen(window.get(), !fullscreen);
                } else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    // Esc leaves fullscreen first; from a window it quits.
                    if (fullscreen) SDL_SetWindowFullscreen(window.get(), false);
                    else            running = false;
                }
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data) on_drop(event.drop.data);
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                fullscreen = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                if (fullscreen) SDL_HideCursor(); else SDL_ShowCursor();
                apply_presentation(window.get(), renderer.get(), logical_w, logical_h,
                                   fullscreen, scale_mode);
                break;
            default:
                break;
            }
        }
        if (!running) break;

        if (disc_loading.valid() &&
            disc_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            LoadedDisc loaded = disc_loading.get();
            if (loaded.disc) {
                std::printf("Disc changed: %s (%zu track(s), %u sectors). Tray open, closing in 1 s.\n",
                            loaded.path.c_str(), loaded.disc->tracks().size(), loaded.disc->sector_count());
                emu.mega_cd()->swap_disc(std::move(loaded.disc));
            } else {
                std::printf("Cannot load disc %s: %s\n", loaded.path.c_str(), loaded.error.c_str());
            }
        }

        // --- Emulate one frame ----------------------------------------------
        for (int player = 0; player < frontend::InputManager::kPlayers; ++player) {
            emu.set_controller(player, input.state(player));
        }
        emu.run_frame();
        audio.push(emu.audio_samples());

        // --- Present --------------------------------------------------------
        if (fb.width() != logical_w || fb.height() != logical_h) {
            logical_w = fb.width();
            logical_h = fb.height();
            apply_presentation(window.get(), renderer.get(), logical_w, logical_h,
                               fullscreen, scale_mode);
        }

        const SDL_Rect active{0, 0, fb.width(), fb.height()};
        SDL_UpdateTexture(texture.get(), &active, fb.pixels().data(), md::Framebuffer::pitch_bytes());

        const SDL_FRect src{0.0f, 0.0f, static_cast<float>(fb.width()), static_cast<float>(fb.height())};
        SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
        SDL_RenderClear(renderer.get());
        SDL_RenderTexture(renderer.get(), texture.get(), &src, nullptr);
        SDL_RenderPresent(renderer.get());

        // --- Frame pacing ---------------------------------------------------
        next_frame += frame_duration;
        const auto now = Clock::now();
        if (next_frame > now) {
            const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(next_frame - now);
            SDL_DelayPrecise(static_cast<Uint64>(wait.count()));
        } else if (now - next_frame > kMaxLag) {
            // Fell far behind (debugger, window drag, ...): resync instead of fast-forwarding.
            next_frame = now;
        }
    }

    return 0;
}
