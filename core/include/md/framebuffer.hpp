#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace md {

// The VDP can output 256 (H32) or 320 (H40) pixels per line and
// 224 (V28) or 240 (V30, PAL only) lines. Storage is sized for the maximum;
// width()/height() report the currently active resolution.
inline constexpr int kMaxScreenWidth  = 320;
inline constexpr int kMaxScreenHeight = 240;

// Pixels are 0xAARRGGBB (ARGB8888), row-major, with a fixed stride of
// kMaxScreenWidth pixels per row regardless of the active width.
class Framebuffer {
public:
    using Pixel = std::uint32_t;

    static constexpr int kStride = kMaxScreenWidth;

    [[nodiscard]] int width()  const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    // Bytes per row, for APIs that want a pitch.
    [[nodiscard]] static constexpr int pitch_bytes() noexcept {
        return kStride * static_cast<int>(sizeof(Pixel));
    }

    void set_resolution(int width, int height) noexcept {
        width_  = width;
        height_ = height;
    }

    [[nodiscard]] std::span<const Pixel> pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::span<Pixel>       pixels()       noexcept { return pixels_; }

    [[nodiscard]] std::span<Pixel> row(int y) noexcept {
        return std::span<Pixel>(pixels_).subspan(static_cast<std::size_t>(y * kStride),
                                                 static_cast<std::size_t>(kStride));
    }

private:
    int width_  = kMaxScreenWidth;
    int height_ = 224;
    std::array<Pixel, static_cast<std::size_t>(kMaxScreenWidth * kMaxScreenHeight)> pixels_{};
};

} // namespace md
