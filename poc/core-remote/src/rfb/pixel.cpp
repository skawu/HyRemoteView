#include "rfb/pixel.hpp"

#include <array>
#include <limits>

namespace poc::core_remote::rfb {
namespace {

// The server scales 8-bit channels with (value * max + 127) / 255, so the inverse is a divided
// rounding. Getting this wrong by one is exactly the kind of "looks correct" failure the PoC must
// not have, which is why the client decodes rather than copies bytes.
std::uint32_t scaleTo8(std::uint32_t component, std::uint16_t max) noexcept
{
    if (max == 0)
        return 0;
    return (component * 255U + max / 2U) / max;
}

}  // namespace

PixelFormat PixelFormat::hyremoteNative() noexcept
{
    return PixelFormat{};
}

PixelFormat PixelFormat::rgb565BigEndian() noexcept
{
    PixelFormat format;
    format.bitsPerPixel = 16;
    format.depth = 16;
    format.bigEndian = true;
    format.trueColor = true;
    format.redMax = 31;
    format.greenMax = 63;
    format.blueMax = 31;
    format.redShift = 11;
    format.greenShift = 5;
    format.blueShift = 0;
    return format;
}

bool PixelFormat::valid(std::string &reason) const
{
    if (!trueColor) {
        reason = "only true-colour pixel formats are accepted by this POC client";
        return false;
    }
    if (bitsPerPixel != 8 && bitsPerPixel != 16 && bitsPerPixel != 32) {
        reason = "bits-per-pixel must be 8, 16 or 32";
        return false;
    }
    if (redMax == 0 || greenMax == 0 || blueMax == 0) {
        reason = "colour maxima must be non-zero";
        return false;
    }

    const std::uint64_t limit = bitsPerPixel == 32 ? std::numeric_limits<std::uint32_t>::max()
                                                  : ((std::uint64_t{1} << bitsPerPixel) - 1U);
    const auto fits = [limit](std::uint16_t max, std::uint8_t shift) {
        return shift < 32U && (static_cast<std::uint64_t>(max) << shift) <= limit;
    };
    if (!fits(redMax, redShift) || !fits(greenMax, greenShift) || !fits(blueMax, blueShift)) {
        reason = "a colour maximum does not fit the declared bits-per-pixel";
        return false;
    }
    reason.clear();
    return true;
}

std::string PixelFormat::describe() const
{
    std::string out = std::to_string(bitsPerPixel) + "bpp depth=" + std::to_string(depth);
    out += bigEndian ? " big-endian" : " little-endian";
    out += " max=" + std::to_string(redMax) + "/" + std::to_string(greenMax) + "/"
           + std::to_string(blueMax);
    out += " shift=" + std::to_string(redShift) + "/" + std::to_string(greenShift) + "/"
           + std::to_string(blueShift);
    return out;
}

std::uint32_t decodePixel(const std::uint8_t *bytes, const PixelFormat &format) noexcept
{
    const std::uint8_t count = format.bytesPerPixel();
    if (bytes == nullptr || count == 0)
        return 0;

    std::uint32_t value = 0;
    if (format.bigEndian) {
        for (std::uint8_t index = 0; index < count; ++index)
            value = (value << 8U) | bytes[index];
    } else {
        for (std::uint8_t index = 0; index < count; ++index)
            value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
    }

    const std::uint32_t red = scaleTo8((value >> format.redShift) & format.redMax, format.redMax);
    const std::uint32_t green = scaleTo8((value >> format.greenShift) & format.greenMax, format.greenMax);
    const std::uint32_t blue = scaleTo8((value >> format.blueShift) & format.blueMax, format.blueMax);
    return (red << 16U) | (green << 8U) | blue;
}

void encodePixel(std::uint32_t rgb, const PixelFormat &format, std::uint8_t *out) noexcept
{
    const std::uint8_t count = format.bytesPerPixel();
    if (out == nullptr || count == 0)
        return;

    const std::uint32_t red = (rgb >> 16U) & 0xffU;
    const std::uint32_t green = (rgb >> 8U) & 0xffU;
    const std::uint32_t blue = rgb & 0xffU;
    const auto toComponent = [](std::uint32_t value, std::uint16_t max) {
        return (value * max + 127U) / 255U;
    };

    const std::uint32_t value = (toComponent(red, format.redMax) << format.redShift)
                                | (toComponent(green, format.greenMax) << format.greenShift)
                                | (toComponent(blue, format.blueMax) << format.blueShift);
    if (format.bigEndian) {
        for (std::uint8_t index = 0; index < count; ++index)
            out[index] = static_cast<std::uint8_t>((value >> (8U * (count - 1U - index))) & 0xffU);
    } else {
        for (std::uint8_t index = 0; index < count; ++index)
            out[index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU);
    }
}

std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void Framebuffer::reset(std::uint16_t width, std::uint16_t height, std::uint32_t fillRgb)
{
    m_width = width;
    m_height = height;
    m_pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fillRgb);
}

std::uint32_t Framebuffer::pixel(std::uint16_t x, std::uint16_t y) const noexcept
{
    if (x >= m_width || y >= m_height)
        return 0;
    return m_pixels[static_cast<std::size_t>(y) * m_width + x];
}

void Framebuffer::setPixel(std::uint16_t x, std::uint16_t y, std::uint32_t rgb) noexcept
{
    if (x >= m_width || y >= m_height)
        return;
    m_pixels[static_cast<std::size_t>(y) * m_width + x] = rgb;
}

bool Framebuffer::blitRaw(std::uint16_t x,
                          std::uint16_t y,
                          std::uint16_t width,
                          std::uint16_t height,
                          const std::uint8_t *bytes,
                          std::size_t byteCount,
                          const PixelFormat &format,
                          std::string &error)
{
    const std::uint8_t bytesPerPixel = format.bytesPerPixel();
    if (bytesPerPixel == 0) {
        error = "pixel format has no bytes per pixel";
        return false;
    }

    // All arithmetic below is on 64-bit values that were already overflow-checked by the caller.
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * bytesPerPixel;
    if (expected != byteCount) {
        error = "raw rectangle payload size does not match its geometry";
        return false;
    }

    const std::uint32_t right = static_cast<std::uint32_t>(x) + width;
    const std::uint32_t bottom = static_cast<std::uint32_t>(y) + height;
    if (right > m_width || bottom > m_height) {
        error = "raw rectangle lies outside the framebuffer";
        return false;
    }

    std::size_t offset = 0;
    for (std::uint16_t row = 0; row < height; ++row) {
        for (std::uint16_t column = 0; column < width; ++column) {
            setPixel(static_cast<std::uint16_t>(x + column),
                     static_cast<std::uint16_t>(y + row),
                     decodePixel(bytes + offset, format));
            offset += bytesPerPixel;
        }
    }
    error.clear();
    return true;
}

std::uint64_t Framebuffer::hash() const noexcept
{
    std::array<std::uint8_t, 4> geometry{};
    geometry[0] = static_cast<std::uint8_t>((m_width >> 8U) & 0xffU);
    geometry[1] = static_cast<std::uint8_t>(m_width & 0xffU);
    geometry[2] = static_cast<std::uint8_t>((m_height >> 8U) & 0xffU);
    geometry[3] = static_cast<std::uint8_t>(m_height & 0xffU);

    std::uint64_t hash = fnv1a64(geometry.data(), geometry.size());
    const std::uint8_t *raw = reinterpret_cast<const std::uint8_t *>(m_pixels.data());
    const std::size_t bytes = m_pixels.size() * sizeof(std::uint32_t);
    for (std::size_t index = 0; index < bytes; ++index) {
        // Only R, G and B participate, so an unused high byte can never change the result.
        if ((index % 4U) == 3U)
            continue;
        hash ^= raw[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t Framebuffer::regionHash(std::uint16_t x,
                                      std::uint16_t y,
                                      std::uint16_t width,
                                      std::uint16_t height) const noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (std::uint16_t row = 0; row < height; ++row) {
        for (std::uint16_t column = 0; column < width; ++column) {
            const std::uint32_t value = pixel(static_cast<std::uint16_t>(x + column),
                                              static_cast<std::uint16_t>(y + row));
            mix(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
            mix(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
            mix(static_cast<std::uint8_t>(value & 0xffU));
        }
    }
    return hash;
}

}  // namespace poc::core_remote::rfb
