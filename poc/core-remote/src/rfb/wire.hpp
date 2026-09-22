#pragma once

// POC_ONLY RFB wire primitives.
//
// Two rules are enforced here and nowhere else, so the rest of the client cannot get them wrong:
//   1. every read is bounds-checked against the buffer it reads from;
//   2. every size computation that came from the peer is overflow-checked before it is used for an
//      allocation, so a hostile rectangle or name length can never produce a wrapped size.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poc::core_remote::rfb {

// ---------------------------------------------------------------------------------------------
// Protocol constants (RFC 6143, plus the two encodings this PoC implements).
// ---------------------------------------------------------------------------------------------

inline constexpr std::uint8_t kProtocolMajor = 3;
inline constexpr std::uint8_t kProtocolMinor38 = 8;
inline constexpr std::uint8_t kProtocolMinor37 = 7;

inline constexpr std::uint8_t kSecurityTypeInvalid = 0;
inline constexpr std::uint8_t kSecurityTypeNone = 1;
inline constexpr std::uint8_t kSecurityTypeVncAuth = 2;

inline constexpr std::uint8_t kClientMessageSetPixelFormat = 0;
inline constexpr std::uint8_t kClientMessageSetEncodings = 2;
inline constexpr std::uint8_t kClientMessageFramebufferUpdateRequest = 3;
inline constexpr std::uint8_t kClientMessageKeyEvent = 4;
inline constexpr std::uint8_t kClientMessagePointerEvent = 5;
inline constexpr std::uint8_t kClientMessageClientCutText = 6;

inline constexpr std::uint8_t kServerMessageFramebufferUpdate = 0;
inline constexpr std::uint8_t kServerMessageSetColourMapEntries = 1;
inline constexpr std::uint8_t kServerMessageBell = 2;
inline constexpr std::uint8_t kServerMessageServerCutText = 3;

inline constexpr std::int32_t kEncodingRaw = 0;
inline constexpr std::int32_t kEncodingCopyRect = 1;
inline constexpr std::int32_t kEncodingDesktopSize = -223;
inline constexpr std::int32_t kEncodingCursor = -239;

// Pointer button mask bits, as used by the wire protocol.
inline constexpr std::uint8_t kButtonLeft = 0x01;
inline constexpr std::uint8_t kButtonMiddle = 0x02;
inline constexpr std::uint8_t kButtonRight = 0x04;
inline constexpr std::uint8_t kWheelUp = 0x08;
inline constexpr std::uint8_t kWheelDown = 0x10;
inline constexpr std::uint8_t kWheelLeft = 0x20;
inline constexpr std::uint8_t kWheelRight = 0x40;

// X11 keysyms this client uses. Values are fixed by the X11 keysym definition, not by HyRemote.
inline constexpr std::uint32_t kKeysymBackspace = 0xff08;
inline constexpr std::uint32_t kKeysymTab = 0xff09;
inline constexpr std::uint32_t kKeysymReturn = 0xff0d;
inline constexpr std::uint32_t kKeysymEscape = 0xff1b;
inline constexpr std::uint32_t kKeysymShiftLeft = 0xffe1;
inline constexpr std::uint32_t kKeysymShiftRight = 0xffe2;
inline constexpr std::uint32_t kKeysymControlLeft = 0xffe3;
inline constexpr std::uint32_t kKeysymControlRight = 0xffe4;
inline constexpr std::uint32_t kKeysymAltLeft = 0xffe9;
inline constexpr std::uint32_t kKeysymAltRight = 0xffea;
inline constexpr std::uint32_t kKeysymSpace = 0x0020;

[[nodiscard]] constexpr std::uint32_t keysymForAscii(char character) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(character));
}

// ---------------------------------------------------------------------------------------------
// Parser hard limits. Every one of these is checked before the value is used.
// ---------------------------------------------------------------------------------------------

struct Limits
{
    // Framebuffer dimensions the client will accept from ServerInit or DesktopSize.
    std::uint16_t maxFramebufferWidth = 16384;
    std::uint16_t maxFramebufferHeight = 16384;
    // Rectangles in one FramebufferUpdate.
    std::uint16_t maxRectangleCount = 1024;
    // Server name in ServerInit.
    std::uint32_t maxServerNameBytes = 4096;
    // Raw payload of a single rectangle.
    std::uint64_t maxRectangleBytes = 64ULL * 1024ULL * 1024ULL;
    // Bytes the client will read before it gives up on the rest of the current message.
    std::uint64_t maxPendingReceiveBytes = 96ULL * 1024ULL * 1024ULL;
    // ServerCutText payload. The server is not expected to send one at all.
    std::uint32_t maxCutTextBytes = 64U * 1024U;
    // Encodings this client will advertise.
    std::uint16_t maxAdvertisedEncodings = 64;
    // Encodings a server may announce in a SetColourMapEntries message.
    std::uint16_t maxColourMapEntries = 256;
};

// Overflow-safe size arithmetic. `nullopt` means "the peer's numbers cannot describe a legal size",
// which is exactly the outcome the negative tests require instead of a wrapped allocation.
[[nodiscard]] std::optional<std::uint64_t> checkedMul(std::uint64_t lhs, std::uint64_t rhs) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checkedAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept;

// ---------------------------------------------------------------------------------------------
// Byte reader / writer
// ---------------------------------------------------------------------------------------------

class Reader
{
public:
    Reader(const std::uint8_t *data, std::size_t size) noexcept
        : m_data(data)
        , m_size(size)
    {
    }

    [[nodiscard]] std::size_t remaining() const noexcept;
    [[nodiscard]] bool atEnd() const noexcept;
    // True when the requested bytes are *not* available; callers use it to fail deterministically.
    [[nodiscard]] bool failed() const noexcept;

    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::uint16_t u16() noexcept;
    [[nodiscard]] std::uint32_t u32() noexcept;
    [[nodiscard]] std::int32_t i32() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t count) noexcept;
    // Reads `count` bytes into the caller's buffer without allocating.
    [[nodiscard]] bool readInto(std::uint8_t *destination, std::size_t count) noexcept;
    void skip(std::size_t count) noexcept;

private:
    [[nodiscard]] bool require(std::size_t count) noexcept;

    const std::uint8_t *m_data = nullptr;
    std::size_t m_size = 0;
    std::size_t m_offset = 0;
    bool m_failed = false;
};

class Writer
{
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void i32(std::int32_t value);
    void bytes(const void *data, std::size_t size);
    void fill(std::uint8_t value, std::size_t count);

    [[nodiscard]] const std::vector<std::uint8_t> &data() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> take();
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<std::uint8_t> m_data;
};

// ---------------------------------------------------------------------------------------------
// Small helpers shared by the client and the harness servers
// ---------------------------------------------------------------------------------------------

[[nodiscard]] std::string toHex(const std::uint8_t *data, std::size_t size);
[[nodiscard]] std::string versionString(std::uint8_t minor);

}  // namespace poc::core_remote::rfb
