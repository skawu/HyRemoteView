// POC_ONLY: protocol primitives, pixel decoding, DES and the deterministic content definition.
//
// These tests run without any socket, so a failure here means the client's own arithmetic or its
// expectation model is wrong - not that a server misbehaved.
#include "harness/deterministic_content.hpp"
#include "rfb/des.hpp"
#include "rfb/pixel.hpp"
#include "rfb/vnc_auth.hpp"
#include "rfb/wire.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace poc::core_remote;

std::uint64_t fromHex(const std::string &text)
{
    std::uint64_t value = 0;
    for (const char character : text)
        value = (value << 4U) | static_cast<std::uint64_t>(character <= '9' ? character - '0'
                                                                           : (character | 0x20) - 'a' + 10);
    return value;
}

void testReaderBounds()
{
    const std::uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78, 0x9a};
    rfb::Reader reader(bytes, sizeof(bytes));
    CHECK(reader.u16() == 0x1234);
    CHECK(reader.u16() == 0x5678);
    CHECK(reader.remaining() == 1);
    CHECK(!reader.failed());

    // A read past the end must fail without reading out of bounds.
    CHECK(reader.u32() == 0);
    CHECK(reader.failed());

    // Once failed, the reader stays failed rather than returning plausible garbage.
    CHECK(reader.u8() == 0);
    CHECK(reader.failed());

    rfb::Reader exhausted(bytes, sizeof(bytes));
    exhausted.skip(sizeof(bytes));
    CHECK(!exhausted.failed());
    CHECK(exhausted.atEnd());
    (void)exhausted.u8();
    CHECK(exhausted.failed());
}

void testOverflowSafeArithmetic()
{
    using rfb::checkedAdd;
    using rfb::checkedMul;

    CHECK(checkedMul(0, 0).value() == 0);
    CHECK(checkedMul(65535, 65535).value() == 4294836225ULL);
    // 0xFFFFFFFF squared is 18446744065119617025, which still fits an unsigned 64-bit integer.
    // Recording that explicitly matters: it is why the wire-level guard has to be a byte *bound*
    // rather than an overflow check alone.
    CHECK(checkedMul(0xFFFFFFFFULL, 0xFFFFFFFFULL).has_value());
    // These really do overflow and must be refused.
    CHECK(!checkedMul(1ULL << 32U, 1ULL << 32U).has_value());
    CHECK(!checkedMul(1ULL << 33U, 1ULL << 32U).has_value());
    CHECK(checkedAdd(1, 2).value() == 3);
    CHECK(!checkedAdd(0xFFFFFFFFFFFFFFFFULL, 1).has_value());

    // The pixel-byte product for the largest legal rectangle in this client must be computable and
    // must exceed a 32-bit size, which is the case a naive `int` would wrap on.
    const auto pixels = checkedMul(65535, 65535);
    CHECK(pixels.has_value());
    const auto bytes = checkedMul(*pixels, 4);
    CHECK(bytes.has_value());
    CHECK(*bytes > 0xFFFFFFFFULL);
}

void testPixelFormatValidation()
{
    std::string reason;

    rfb::PixelFormat native = rfb::PixelFormat::hyremoteNative();
    CHECK(native.valid(reason));
    CHECK(native.bytesPerPixel() == 4);

    rfb::PixelFormat rgb565 = rfb::PixelFormat::rgb565BigEndian();
    CHECK(rgb565.valid(reason));
    CHECK(rgb565.bytesPerPixel() == 2);

    rfb::PixelFormat bad = native;
    bad.trueColor = false;
    CHECK(!bad.valid(reason));

    bad = native;
    bad.bitsPerPixel = 24;
    CHECK(!bad.valid(reason));

    bad = native;
    bad.blueMax = 0;
    CHECK(!bad.valid(reason));

    bad = native;
    bad.blueShift = 40;
    CHECK(!bad.valid(reason));
}

void testPixelRoundTrip()
{
    const std::array<std::uint32_t, 6> colours{0x000000U, 0xFFFFFFU, 0xFF0000U, 0x00FF00U, 0x0000FFU, 0x123456U};

    for (const std::uint32_t colour : colours) {
        // The 32 bpp native format reproduces every channel exactly.
        {
            const rfb::PixelFormat format = rfb::PixelFormat::hyremoteNative();
            std::uint8_t buffer[4] = {};
            rfb::encodePixel(colour, format, buffer);
            CHECK_EQ(rfb::decodePixel(buffer, format), colour);
        }

        // 16 bpp quantises: the error must stay within one step of a 5/6/5 channel.
        {
            const rfb::PixelFormat format = rfb::PixelFormat::rgb565BigEndian();
            std::uint8_t buffer[4] = {};
            rfb::encodePixel(colour, format, buffer);
            const std::uint32_t decoded = rfb::decodePixel(buffer, format);
            for (int shift = 0; shift < 24; shift += 8) {
                const std::uint32_t original = (colour >> shift) & 0xFFU;
                const std::uint32_t roundTripped = (decoded >> shift) & 0xFFU;
                const std::uint32_t difference =
                    original > roundTripped ? original - roundTripped : roundTripped - original;
                CHECK(difference <= 8U);
            }
        }

        // 8 bpp with a 3/3/2 split quantises hard. The requirement is a deterministic, bounded,
        // idempotent round trip - not channel equality.
        {
            rfb::PixelFormat format;
            format.bitsPerPixel = 8;
            format.depth = 8;
            format.redMax = 7;
            format.greenMax = 7;
            format.blueMax = 3;
            format.redShift = 5;
            format.greenShift = 2;
            format.blueShift = 0;

            std::uint8_t buffer[4] = {};
            rfb::encodePixel(colour, format, buffer);
            const std::uint32_t decoded = rfb::decodePixel(buffer, format);
            for (int shift = 0; shift < 24; shift += 8) {
                const std::uint32_t original = (colour >> shift) & 0xFFU;
                const std::uint32_t roundTripped = (decoded >> shift) & 0xFFU;
                const std::uint32_t difference =
                    original > roundTripped ? original - roundTripped : roundTripped - original;
                CHECK(difference <= 85U);  // one step of a 2-bit channel
            }

            // Re-encoding the decoded value must be stable, or the client would drift per frame.
            std::uint8_t again[4] = {};
            rfb::encodePixel(decoded, format, again);
            CHECK_EQ(rfb::decodePixel(again, format), decoded);
        }
    }

    // The wire layout itself: the HyRemote native format is 32 bpp little-endian with red in the
    // lowest byte, so the first three bytes of a pixel are R, G, B.
    std::uint8_t nativeBytes[4] = {};
    std::uint8_t bigEndianBytes[4] = {};
    rfb::encodePixel(0x112233U, rfb::PixelFormat::hyremoteNative(), nativeBytes);
    CHECK_EQ(int(nativeBytes[0]), 0x11);  // red
    CHECK_EQ(int(nativeBytes[1]), 0x22);  // green
    CHECK_EQ(int(nativeBytes[2]), 0x33);  // blue

    // Pure red is a full 0xFF in the native format's lowest byte, while the 16 bpp big-endian form
    // puts red in the top five bits: 31 << 11 = 0xF800. Different layouts, so different bytes.
    rfb::encodePixel(0xFF0000U, rfb::PixelFormat::hyremoteNative(), nativeBytes);
    rfb::encodePixel(0xFF0000U, rfb::PixelFormat::rgb565BigEndian(), bigEndianBytes);
    CHECK_EQ(int(nativeBytes[0]), 0xFF);
    CHECK_EQ(int(nativeBytes[1]), 0x00);
    CHECK_EQ(int(nativeBytes[2]), 0x00);
    CHECK_EQ(int(bigEndianBytes[0]), 0xF8);
    CHECK(nativeBytes[0] != bigEndianBytes[0]);
}

rfb::Framebuffer solidSurface(std::uint16_t width, std::uint16_t height, std::uint32_t fill)
{
    rfb::Framebuffer framebuffer;
    framebuffer.reset(width, height, fill);
    return framebuffer;
}

void testFramebufferBoundsAndHash()
{
    rfb::Framebuffer framebuffer;
    framebuffer.reset(8, 4, 0x010203U);
    CHECK(framebuffer.width() == 8);
    CHECK(framebuffer.height() == 4);
    CHECK(framebuffer.pixel(0, 0) == 0x010203U);

    // Reads and writes outside the surface are no-ops rather than out-of-bounds accesses.
    CHECK(framebuffer.pixel(8, 0) == 0);
    framebuffer.setPixel(100, 100, 0xFFFFFFU);
    CHECK(framebuffer.hash() == solidSurface(framebuffer.width(), framebuffer.height(), 0x010203U).hash());

    const std::uint64_t before = framebuffer.hash();
    framebuffer.setPixel(3, 2, 0xABCDEFU);
    CHECK(framebuffer.hash() != before);

    // Geometry participates in the hash, so a resize can never produce a false match.
    rfb::Framebuffer smaller;
    smaller.reset(4, 4, 0x010203U);
    CHECK(smaller.hash() != framebuffer.hash());

    // A rectangle that does not fit must be refused before anything is written.
    std::string error;
    std::vector<std::uint8_t> payload(4 * 4 * 4, 0);
    CHECK(!framebuffer.blitRaw(6, 0, 4, 4, payload.data(), payload.size(),
                               rfb::PixelFormat::hyremoteNative(), error));
    CHECK(!error.empty());

    // A payload whose size disagrees with its geometry must also be refused.
    error.clear();
    CHECK(!framebuffer.blitRaw(0, 0, 4, 4, payload.data(), payload.size() - 5,
                               rfb::PixelFormat::hyremoteNative(), error));
    CHECK(!error.empty());

    error.clear();
    CHECK(framebuffer.blitRaw(0, 0, 4, 4, payload.data(), payload.size(),
                              rfb::PixelFormat::hyremoteNative(), error));
    CHECK(error.empty());
}

void testDesKnownAnswer()
{
    // FIPS 81 / classic DES test vector: key 0123456789ABCDEF, plaintext "Now is t".
    const std::array<std::uint8_t, 8> key{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const std::array<std::uint8_t, 8> plaintext{'N', 'o', 'w', ' ', 'i', 's', ' ', 't'};
    const std::array<std::uint8_t, 8> encrypted = rfb::desEncryptBlock(key, plaintext);
    CHECK(fromHex("3fa40e8a984d4815") == fromHex(rfb::toHex(encrypted.data(), encrypted.size())));

    // A second vector from the same family, so a single lucky coincidence cannot pass.
    const std::array<std::uint8_t, 8> key2{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const std::array<std::uint8_t, 8> plaintext2{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const std::array<std::uint8_t, 8> encrypted2 = rfb::desEncryptBlock(key2, plaintext2);
    CHECK(fromHex("d5d44ff720683d0d") == fromHex(rfb::toHex(encrypted2.data(), encrypted2.size())));

    CHECK(rfb::reverseBits(0x01) == 0x80);
    CHECK(rfb::reverseBits(0xF0) == 0x0F);
}

void testVncAuthResponseShape()
{
    // The same password and challenge must always produce the same response, and a different
    // password must not.
    std::array<std::uint8_t, rfb::kVncChallengeBytes> challenge{};
    for (std::size_t index = 0; index < challenge.size(); ++index)
        challenge[index] = static_cast<std::uint8_t>(index * 7U + 1U);

    const std::string password = "s3cret!";
    std::array<std::uint8_t, rfb::kVncChallengeBytes> first{};
    std::array<std::uint8_t, rfb::kVncChallengeBytes> second{};
    std::string error;
    CHECK(rfb::vncAuthResponse(reinterpret_cast<const std::uint8_t *>(password.data()), password.size(),
                               challenge, first, error));
    CHECK(rfb::vncAuthResponse(reinterpret_cast<const std::uint8_t *>(password.data()), password.size(),
                               challenge, second, error));
    CHECK(first == second);

    const std::string other = "wr0ng!!!";
    std::array<std::uint8_t, rfb::kVncChallengeBytes> third{};
    CHECK(rfb::vncAuthResponse(reinterpret_cast<const std::uint8_t *>(other.data()), other.size(),
                               challenge, third, error));
    CHECK(first != third);

    // The 1..8 byte credential rule is part of the protocol, not a convenience.
    CHECK(!rfb::vncAuthResponse(nullptr, 0, challenge, first, error));
    const std::string tooLong = "123456789";
    CHECK(!rfb::vncAuthResponse(reinterpret_cast<const std::uint8_t *>(tooLong.data()), tooLong.size(),
                                challenge, first, error));
}

void testDeterministicContentIsStable()
{
    // The expectation model must be a pure function of (x, y, step): repeated evaluation must agree.
    for (const harness::Step &step : harness::steps()) {
        for (std::uint16_t y = 0; y < step.height; y += 3) {
            for (std::uint16_t x = 0; x < step.width; x += 3)
                CHECK(harness::pixelAt(x, y, step.index) == harness::pixelAt(x, y, step.index));
        }
    }

    // Every step must differ from every other step, or a step would not be evidence of anything.
    std::vector<std::uint64_t> hashes;
    for (const harness::Step &step : harness::steps())
        hashes.push_back(harness::framebufferFor(step.index).hash());
    for (std::size_t first = 0; first < hashes.size(); ++first) {
        for (std::size_t second = first + 1; second < hashes.size(); ++second)
            CHECK(hashes[first] != hashes[second]);
    }

    // The known colour blocks must be present verbatim in the expected surface, and the transmitted
    // encoding must decode back to them.
    const rfb::Framebuffer expected = harness::framebufferFor(0);
    for (const harness::Block &block : harness::colourBlocks()) {
        const std::uint16_t x = static_cast<std::uint16_t>(block.rect.x + block.rect.width / 2);
        const std::uint16_t y = static_cast<std::uint16_t>(block.rect.y + block.rect.height / 2);
        CHECK(expected.pixel(x, y) == block.rgb);
    }

    const rfb::PixelFormat format = rfb::PixelFormat::hyremoteNative();
    const std::vector<std::uint8_t> encoded = harness::encodeFrame(0, format);
    CHECK(encoded.size() == static_cast<std::size_t>(harness::kInitialWidth) * harness::kInitialHeight * 4);

    // Decoding the encoded bytes must reproduce the expected surface exactly.
    rfb::Framebuffer decoded;
    decoded.reset(harness::kInitialWidth, harness::kInitialHeight, 0);
    std::string error;
    CHECK(decoded.blitRaw(0, 0, harness::kInitialWidth, harness::kInitialHeight, encoded.data(),
                          encoded.size(), format, error));
    CHECK(decoded.hash() == expected.hash());

    // A partial rectangle must decode to the same content as the same region of the full frame.
    const harness::Rect region{20, 16, 16, 12};
    const std::vector<std::uint8_t> partial = harness::encodeRect(3, region, format);
    CHECK(partial.size() == static_cast<std::size_t>(region.width) * region.height * 4);
    rfb::Framebuffer partialSurface;
    partialSurface.reset(region.width, region.height, 0);
    CHECK(partialSurface.blitRaw(0, 0, region.width, region.height, partial.data(), partial.size(),
                                 format, error));
    const rfb::Framebuffer fullAtStepThree = harness::framebufferFor(3);
    // The partial surface is a frame in its own right, so its region hash at (0,0) must equal the
    // full frame's region hash at the region's real offset.
    CHECK_EQ(partialSurface.regionHash(0, 0, region.width, region.height),
             fullAtStepThree.regionHash(region.x, region.y, region.width, region.height));
}

void testVersionStrings()
{
    CHECK(rfb::versionString(8) == "RFB 003.008\n");
    CHECK(rfb::versionString(7) == "RFB 003.007\n");
    CHECK(rfb::versionString(8).size() == 12);
}

}  // namespace

int main()
{
    testReaderBounds();
    testOverflowSafeArithmetic();
    testPixelFormatValidation();
    testPixelRoundTrip();
    testFramebufferBoundsAndHash();
    testDesKnownAnswer();
    testVncAuthResponseShape();
    testDeterministicContentIsStable();
    testVersionStrings();

    return poc_test::finish("RFB primitives, pixel decode, DES, and deterministic content definition");
}
