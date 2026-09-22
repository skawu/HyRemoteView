#include "harness/deterministic_content.hpp"

#include <stdexcept>
#include <string>

namespace poc::core_remote::harness {
namespace {

constexpr std::array<Step, kStepCount> kSteps{{
    {0, "initial-full", kInitialWidth, kInitialHeight, false, {0, 0, kInitialWidth, kInitialHeight}},
    {1, "damage-block", kInitialWidth, kInitialHeight, false, {4, 34, 24, 8}},
    {2, "damage-line", kInitialWidth, kInitialHeight, false, {40, 0, 2, 64}},
    {3, "damage-checker", kInitialWidth, kInitialHeight, false, {60, 40, 30, 18}},
    {4, "resize", kResizedWidth, kResizedHeight, true, {0, 0, kResizedWidth, kResizedHeight}},
    {5, "damage-after-resize", kResizedWidth, kResizedHeight, false, {100, 60, 24, 18}},
}};

bool inside(const Rect &rect, std::uint16_t x, std::uint16_t y) noexcept
{
    return x >= rect.x && y >= rect.y
           && static_cast<std::uint32_t>(x) < static_cast<std::uint32_t>(rect.x) + rect.width
           && static_cast<std::uint32_t>(y) < static_cast<std::uint32_t>(rect.y) + rect.height;
}

bool atOrAfter(int step, int threshold) noexcept
{
    return step >= threshold;
}

std::uint32_t basePixel(std::uint16_t x, std::uint16_t y) noexcept
{
    // 1. 4x4 checker background.
    const bool even = (((x / 4U) + (y / 4U)) % 2U) == 0U;
    std::uint32_t colour = even ? 0x1a202cU : 0x2c3442U;

    // 2. The eight known colour blocks.
    for (const Block &block : colourBlocks()) {
        if (inside(block.rect, x, y))
            colour = block.rgb;
    }

    // 3. A horizontal line and a vertical line, so a one-pixel misalignment is detectable.
    if (y == 30U)
        colour = 0xff8000U;
    if (x == 34U)
        colour = 0x0080ffU;

    // 4. A diagonal, so a row/column swap is detectable.
    if (y >= 32U && y < 48U && x == static_cast<std::uint16_t>(2U * (y - 32U)))
        colour = 0xdcdc3cU;

    // 5. A deterministic value band.
    if (y >= 48U && y < 56U) {
        const std::uint32_t red = (static_cast<std::uint32_t>(x) * 3U) % 256U;
        const std::uint32_t green = (static_cast<std::uint32_t>(y) * 5U) % 256U;
        const std::uint32_t blue = (static_cast<std::uint32_t>(x) + y) % 256U;
        colour = (red << 16U) | (green << 8U) | blue;
    }

    return colour;
}

}  // namespace

const std::array<Step, kStepCount> &steps() noexcept
{
    return kSteps;
}

const Step &step(int index)
{
    if (index < 0 || index >= kStepCount)
        throw std::out_of_range("deterministic step index out of range: " + std::to_string(index));
    return kSteps[static_cast<std::size_t>(index)];
}

int stepIndex(std::string_view name)
{
    for (const Step &entry : kSteps) {
        if (entry.name == name)
            return entry.index;
    }
    return -1;
}

const std::vector<Block> &colourBlocks()
{
    static const std::vector<Block> blocks{
        {{2, 2, 16, 12}, 0xff0000U, "red"},
        {{20, 2, 16, 12}, 0x00ff00U, "green"},
        {{38, 2, 16, 12}, 0x0000ffU, "blue"},
        {{56, 2, 16, 12}, 0xffff00U, "yellow"},
        {{2, 16, 16, 12}, 0xff00ffU, "magenta"},
        {{20, 16, 16, 12}, 0x00ffffU, "cyan"},
        {{38, 16, 16, 12}, 0xffffffU, "white"},
        {{56, 16, 16, 12}, 0x080808U, "near-black"},
    };
    return blocks;
}

std::uint32_t pixelAt(std::uint16_t x, std::uint16_t y, int stepIndexValue) noexcept
{
    std::uint32_t colour = basePixel(x, y);

    if (atOrAfter(stepIndexValue, 1) && inside(kSteps[1].changed, x, y))
        colour = 0xff4040U;
    if (atOrAfter(stepIndexValue, 2) && inside(kSteps[2].changed, x, y))
        colour = 0x40ff40U;
    if (atOrAfter(stepIndexValue, 3) && inside(kSteps[3].changed, x, y))
        colour = (((x + y) % 2U) == 0U) ? 0xffffffU : 0x101010U;
    if (atOrAfter(stepIndexValue, 5) && inside(kSteps[5].changed, x, y)) {
        colour = 0x00ff80U;
        if (static_cast<std::uint16_t>(x - kSteps[5].changed.x)
            == static_cast<std::uint16_t>(y - kSteps[5].changed.y)) {
            colour = 0xffffffU;
        }
    }
    return colour;
}

rfb::Framebuffer framebufferFor(int stepIndexValue)
{
    const Step &current = step(stepIndexValue);
    rfb::Framebuffer framebuffer;
    framebuffer.reset(current.width, current.height, 0);
    for (std::uint16_t y = 0; y < current.height; ++y) {
        for (std::uint16_t x = 0; x < current.width; ++x)
            framebuffer.setPixel(x, y, pixelAt(x, y, stepIndexValue));
    }
    return framebuffer;
}

rfb::Framebuffer framebufferForFormat(int stepIndexValue, const rfb::PixelFormat &format)
{
    const Step &current = step(stepIndexValue);
    rfb::Framebuffer framebuffer;
    framebuffer.reset(current.width, current.height, 0);

    std::uint8_t encoded[4] = {};
    for (std::uint16_t y = 0; y < current.height; ++y) {
        for (std::uint16_t x = 0; x < current.width; ++x) {
            rfb::encodePixel(pixelAt(x, y, stepIndexValue), format, encoded);
            framebuffer.setPixel(x, y, rfb::decodePixel(encoded, format));
        }
    }
    return framebuffer;
}

std::vector<std::uint8_t> encodeFrame(int stepIndexValue, const rfb::PixelFormat &format)
{
    const Step &current = step(stepIndexValue);
    return encodeRect(stepIndexValue, Rect{0, 0, current.width, current.height}, format);
}

std::vector<std::uint8_t> encodeRect(int stepIndexValue, const Rect &rect, const rfb::PixelFormat &format)
{
    const Step &current = step(stepIndexValue);
    const std::uint64_t right = static_cast<std::uint64_t>(rect.x) + rect.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(rect.y) + rect.height;
    if (right > current.width || bottom > current.height)
        throw std::out_of_range("encoded rectangle lies outside the deterministic frame");

    const std::uint8_t bytesPerPixel = format.bytesPerPixel();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(rect.width) * rect.height * bytesPerPixel, 0);
    std::size_t offset = 0;
    for (std::uint16_t row = 0; row < rect.height; ++row) {
        for (std::uint16_t column = 0; column < rect.width; ++column) {
            const std::uint16_t x = static_cast<std::uint16_t>(rect.x + column);
            const std::uint16_t y = static_cast<std::uint16_t>(rect.y + row);
            rfb::encodePixel(pixelAt(x, y, stepIndexValue), format, out.data() + offset);
            offset += bytesPerPixel;
        }
    }
    return out;
}

const std::vector<InputAction> &inputScript()
{
    // Covers the input surface #9 requires: pointer moves, all three buttons, wheel in both axes,
    // A-Z and 0-9 keysyms, Shift/Ctrl/Alt, key press/release, and a combination that is left held
    // so the disconnect-cleanup behaviour can be observed.
    static const std::vector<InputAction> script{
        {InputKind::PointerMove, 0, 10, 20, 0, "pointer move to (10,20)"},
        {InputKind::ButtonPress, rfb::kButtonLeft, 10, 20, 0, "left press"},
        {InputKind::ButtonRelease, rfb::kButtonLeft, 10, 20, 0, "left release"},
        {InputKind::ButtonPress, rfb::kButtonMiddle, 11, 21, 0, "middle press"},
        {InputKind::ButtonRelease, rfb::kButtonMiddle, 11, 21, 0, "middle release"},
        {InputKind::ButtonPress, rfb::kButtonRight, 12, 22, 0, "right press"},
        {InputKind::ButtonRelease, rfb::kButtonRight, 12, 22, 0, "right release"},
        {InputKind::Wheel, rfb::kWheelUp, 12, 22, 0, "wheel up"},
        {InputKind::Wheel, rfb::kWheelDown, 12, 22, 0, "wheel down"},
        {InputKind::Wheel, rfb::kWheelLeft, 12, 22, 0, "wheel left"},
        {InputKind::Wheel, rfb::kWheelRight, 12, 22, 0, "wheel right"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('A'), "key press A"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('A'), "key release A"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('Z'), "key press Z"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('Z'), "key release Z"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('0'), "key press 0"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('0'), "key release 0"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('9'), "key press 9"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('9'), "key release 9"},
        {InputKind::KeyPress, 0, 0, 0, rfb::kKeysymShiftLeft, "shift press"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('A'), "shift+A press"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('A'), "shift+A release"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::kKeysymShiftLeft, "shift release"},
        {InputKind::KeyPress, 0, 0, 0, rfb::kKeysymControlLeft, "ctrl press"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('C'), "ctrl+C press"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::keysymForAscii('C'), "ctrl+C release"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::kKeysymControlLeft, "ctrl release"},
        {InputKind::KeyPress, 0, 0, 0, rfb::kKeysymAltLeft, "alt press"},
        {InputKind::KeyRelease, 0, 0, 0, rfb::kKeysymAltLeft, "alt release"},
        {InputKind::KeyPress, 0, 0, 0, rfb::keysymForAscii('Q'), "held key press Q"},
        {InputKind::ButtonPress, rfb::kButtonLeft, 30, 40, 0, "held left press"},
    };
    return script;
}

}  // namespace poc::core_remote::harness
