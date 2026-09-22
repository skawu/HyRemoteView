#pragma once

// POC_ONLY pixel format model and framebuffer.
//
// The client decodes raw rectangles into a normalised 0x00RRGGBB surface so that assertions are
// written against the *content the remote target displayed*, not against whatever byte layout the
// server happened to use. The framebuffer also owns the deterministic hash used as evidence.

#include "rfb/wire.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace poc::core_remote::rfb {

struct PixelFormat
{
    std::uint8_t bitsPerPixel = 32;
    std::uint8_t depth = 24;
    bool bigEndian = false;
    bool trueColor = true;
    std::uint16_t redMax = 255;
    std::uint16_t greenMax = 255;
    std::uint16_t blueMax = 255;
    std::uint8_t redShift = 0;
    std::uint8_t greenShift = 8;
    std::uint8_t blueShift = 16;

    // The format HyRemote advertises in ServerInit: 32 bpp little-endian, 8 bits per channel.
    [[nodiscard]] static PixelFormat hyremoteNative() noexcept;
    // A deliberately different but legal format, used to prove SetPixelFormat is honoured.
    [[nodiscard]] static PixelFormat rgb565BigEndian() noexcept;

    [[nodiscard]] std::uint8_t bytesPerPixel() const noexcept { return static_cast<std::uint8_t>(bitsPerPixel / 8U); }
    [[nodiscard]] bool valid(std::string &reason) const;
    [[nodiscard]] std::string describe() const;
};

// Decodes one wire pixel into 0x00RRGGBB.
[[nodiscard]] std::uint32_t decodePixel(const std::uint8_t *bytes, const PixelFormat &format) noexcept;

// Encodes 0x00RRGGBB into the wire layout. Used by the POC_ONLY harness servers so the frame the
// server transmits and the frame the client expects come from the same definition.
void encodePixel(std::uint32_t rgb, const PixelFormat &format, std::uint8_t *out) noexcept;

class Framebuffer
{
public:
    Framebuffer() = default;

    void reset(std::uint16_t width, std::uint16_t height, std::uint32_t fillRgb = 0);

    [[nodiscard]] std::uint16_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint16_t height() const noexcept { return m_height; }
    [[nodiscard]] bool empty() const noexcept { return m_pixels.empty(); }
    [[nodiscard]] const std::vector<std::uint32_t> &pixels() const noexcept { return m_pixels; }

    [[nodiscard]] std::uint32_t pixel(std::uint16_t x, std::uint16_t y) const noexcept;
    void setPixel(std::uint16_t x, std::uint16_t y, std::uint32_t rgb) noexcept;

    // Copies a decoded rectangle into the surface. Fails, without allocating, when the rectangle is
    // outside the surface or when the byte count does not match the format.
    [[nodiscard]] bool blitRaw(std::uint16_t x,
                               std::uint16_t y,
                               std::uint16_t width,
                               std::uint16_t height,
                               const std::uint8_t *bytes,
                               std::size_t byteCount,
                               const PixelFormat &format,
                               std::string &error);

    // FNV-1a 64 over "width x height" plus every pixel's R, G and B bytes. Deterministic and
    // independent of the surface's internal layout, so the harness servers can compute the same
    // value from the frame they produced.
    [[nodiscard]] std::uint64_t hash() const noexcept;

    // FNV-1a 64 over one rectangle of the surface, for partial-update assertions.
    [[nodiscard]] std::uint64_t regionHash(std::uint16_t x,
                                           std::uint16_t y,
                                           std::uint16_t width,
                                           std::uint16_t height) const noexcept;

private:
    std::uint16_t m_width = 0;
    std::uint16_t m_height = 0;
    std::vector<std::uint32_t> m_pixels;
};

// FNV-1a 64, the single hash used everywhere in this harness.
[[nodiscard]] std::uint64_t fnv1a64(const std::uint8_t *data, std::size_t size) noexcept;

}  // namespace poc::core_remote::rfb
