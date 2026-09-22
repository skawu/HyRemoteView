#pragma once

// POC_ONLY deterministic content definition.
//
// This header is the *single* definition of what the remote frame must contain and which input the
// probe must send. Both the harness servers (which produce the frames) and the probe/tests (which
// assert them) build on it, so "the client decoded the right pixels" is never judged by comparing
// the client against itself: the expected value is derived from this definition, independently of
// any bytes that travelled over the socket.

#include "rfb/pixel.hpp"
#include "rfb/wire.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace poc::core_remote::harness {

struct Rect
{
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct Block
{
    Rect rect;
    std::uint32_t rgb = 0;
    std::string_view label;
};

struct Step
{
    int index = 0;
    std::string_view name;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    bool resizes = false;
    Rect changed;  // the region this step changes relative to the previous step
};

inline constexpr int kStepCount = 6;

inline constexpr std::uint16_t kInitialWidth = 96;
inline constexpr std::uint16_t kInitialHeight = 64;
inline constexpr std::uint16_t kResizedWidth = 128;
inline constexpr std::uint16_t kResizedHeight = 80;

[[nodiscard]] const std::array<Step, kStepCount> &steps() noexcept;
[[nodiscard]] const Step &step(int index);
[[nodiscard]] int stepIndex(std::string_view name);

// The eight known colour blocks. Rects are absolute, so they survive the geometry resize.
[[nodiscard]] const std::vector<Block> &colourBlocks();

// The expected content of one pixel at one step. Pure function: no socket, no server involved.
[[nodiscard]] std::uint32_t pixelAt(std::uint16_t x, std::uint16_t y, int step) noexcept;

// The complete expected surface at `step`.
[[nodiscard]] rfb::Framebuffer framebufferFor(int step);

// The expected surface *after a round trip through `format`*. For a format that cannot represent the
// defined colours exactly (anything below 32 bpp), this is what a correct client must still produce,
// and it is derived from the content definition rather than from any transmitted byte.
[[nodiscard]] rfb::Framebuffer framebufferForFormat(int step, const rfb::PixelFormat &format);

// Raw wire bytes of a whole step in the given format, for the harness servers to transmit.
[[nodiscard]] std::vector<std::uint8_t> encodeFrame(int step, const rfb::PixelFormat &format);

// Raw wire bytes of one rectangle of `step`, for partial/incremental transmissions.
[[nodiscard]] std::vector<std::uint8_t> encodeRect(int step,
                                                  const Rect &rect,
                                                  const rfb::PixelFormat &format);

// ---------------------------------------------------------------------------------------------
// The input script. Executed by the probe, asserted by the input test.
// ---------------------------------------------------------------------------------------------

enum class InputKind {
    PointerMove,
    ButtonPress,
    ButtonRelease,
    Wheel,
    KeyPress,
    KeyRelease,
};

struct InputAction
{
    InputKind kind = InputKind::PointerMove;
    std::uint8_t button = 0;  // pointer button mask bit, or wheel mask bit
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint32_t keysym = 0;
    std::string_view label;
};

[[nodiscard]] const std::vector<InputAction> &inputScript();

}  // namespace poc::core_remote::harness
