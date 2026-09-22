#pragma once

// POC_ONLY minimal RFB client.
//
// Scope is exactly the G3-POC-1A list: RFB 3.8 handshake (and 3.7 negotiation where the server
// permits it), SecurityType None for local/trusted use, VNC Authentication (type 2),
// ClientInit/ServerInit, SetPixelFormat, SetEncodings, Raw encoding, the DesktopSize
// pseudo-encoding, FramebufferUpdateRequest with full and incremental updates, KeyEvent,
// PointerEvent, wheel, reconnect and clean disconnect.
//
// Deliberately absent: CopyRect, ZRLE/Tight/Hextile, cursor pseudo-encoding, clipboard message
// handling beyond bounded consumption, colour-map formats, and any protocol abstraction layer. A
// server message outside the implemented set is a deterministic error, never a silent no-op.

#include "net/socket.hpp"
#include "rfb/pixel.hpp"
#include "rfb/wire.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace poc::core_remote::rfb {

enum class ErrorCode {
    None,
    ConnectFailed,
    IoTimeout,
    IoError,
    ConnectionClosed,
    ProtocolVersionUnsupported,
    SecurityOfferMalformed,
    SecurityTypeNotOffered,
    SecurityResultFailed,
    AuthenticationRejected,
    ServerInitMalformed,
    FramebufferSizeRejected,
    RectangleCountExceeded,
    PayloadSizeExceeded,
    SizeOverflow,
    UnsupportedEncoding,
    RectangleOutOfBounds,
    PixelFormatUnsupported,
    UnsupportedServerMessage,
    NotConnected,
    AlreadyConnected,
    ReceiveLimitExceeded,
};

[[nodiscard]] const char *toString(ErrorCode code) noexcept;

struct Error
{
    ErrorCode code = ErrorCode::None;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::None; }
};

// How the client presents its protocol version, and how it therefore reads the security offer.
//
//   Prefer38  - send "RFB 003.008"; the offer is the 3.8 form (count + types).
//   Lenient37 - send "RFB 003.007"; accept either framing, decided from the first byte: a 3.7 u32
//               security type starts with 0x00, while a 3.8 count is >= 1.
//   Strict37  - send "RFB 003.007" and insist on the RFC 6143 3.7 form (a 4-byte security type).
//               This exists so the harness can demonstrate what a strictly-3.7 peer experiences.
enum class VersionMode {
    Prefer38,
    Lenient37,
    Strict37,
};

enum class SecurityPreference {
    Auto,      // VNC auth when a password is configured, otherwise None
    None,
    VncAuth,
};

struct ClientConfig
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 5921;
    int connectTimeoutMs = 5000;
    int ioTimeoutMs = 5000;

    VersionMode versionMode = VersionMode::Prefer38;
    SecurityPreference securityPreference = SecurityPreference::Auto;
    std::vector<std::uint8_t> password;  // 1..8 bytes; never written to a report or a log

    bool shared = true;
    bool advertiseDesktopSize = true;
    bool sendPixelFormat = false;
    PixelFormat pixelFormat = PixelFormat::hyremoteNative();
    Limits limits{};
};

struct ServerInfo
{
    std::string versionString;
    std::uint8_t negotiatedMinor = 0;
    std::uint8_t securityTypeUsed = 0;
    std::uint16_t framebufferWidth = 0;
    std::uint16_t framebufferHeight = 0;
    PixelFormat serverPixelFormat;
    std::string serverName;
    bool securityResultObserved = false;
    bool securityOfferWas38Form = true;
};

struct RectangleInfo
{
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int32_t encoding = 0;
};

struct UpdateInfo
{
    std::vector<RectangleInfo> rectangles;
    bool resized = false;
    std::uint64_t rawBytes = 0;
};

enum class ServerMessageKind {
    FramebufferUpdate,
    Bell,
    ServerCutText,
};

struct ServerMessage
{
    ServerMessageKind kind = ServerMessageKind::Bell;
    UpdateInfo update;
    std::string cutText;
};

class Client
{
public:
    explicit Client(ClientConfig config);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // --- lifecycle -----------------------------------------------------------------------------

    // Opens the socket and completes the handshake, including the security exchange, ClientInit and
    // ServerInit. On failure the socket is closed and `error()` explains the deterministic reason.
    [[nodiscard]] Error connect();
    // Clean disconnect: the socket is closed by the client, which is what a viewer does when the
    // operator closes it. RFB has no shutdown message for the client to send.
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] Error reconnect();

    // --- post-handshake configuration ----------------------------------------------------------

    [[nodiscard]] Error setPixelFormat(const PixelFormat &format);
    [[nodiscard]] Error setEncodings(const std::vector<std::int32_t> &encodings);
    [[nodiscard]] Error requestFramebufferUpdate(bool incremental,
                                                 std::uint16_t x,
                                                 std::uint16_t y,
                                                 std::uint16_t width,
                                                 std::uint16_t height);
    [[nodiscard]] Error requestFullUpdate();

    // Reads exactly one server message, applying it to the framebuffer. Bell and ServerCutText are
    // recorded and reported rather than ignored, so their absence can be asserted.
    [[nodiscard]] Error readServerMessage(ServerMessage &message, int timeoutMs = -1);
    // Reads messages until a FramebufferUpdate arrives.
    [[nodiscard]] Error waitForFramebufferUpdate(UpdateInfo &update, int timeoutMs = -1);

    // --- input --------------------------------------------------------------------------------

    [[nodiscard]] Error sendKeyEvent(std::uint32_t keysym, bool pressed);
    [[nodiscard]] Error sendPointerEvent(std::uint8_t buttonMask, std::uint16_t x, std::uint16_t y);
    // One wheel notch: a press/release pair on the corresponding mask bit, which is how an RFB
    // viewer expresses a scroll.
    [[nodiscard]] Error sendWheel(int verticalNotches,
                                  int horizontalNotches,
                                  std::uint16_t x,
                                  std::uint16_t y);
    // Sent to show the client can produce a well-formed message the server only consumes. Not used
    // to claim clipboard support: the server publishes no clipboard content at all.
    [[nodiscard]] Error sendClientCutText(const std::string &text);

    // --- inspection ---------------------------------------------------------------------------

    [[nodiscard]] const Error &error() const noexcept { return m_error; }
    [[nodiscard]] const ServerInfo &serverInfo() const noexcept { return m_server; }
    [[nodiscard]] const Framebuffer &framebuffer() const noexcept { return m_framebuffer; }
    [[nodiscard]] const PixelFormat &activeFormat() const noexcept { return m_activeFormat; }
    [[nodiscard]] std::uint64_t bytesRead() const noexcept { return m_bytesRead; }
    [[nodiscard]] std::uint64_t bytesWritten() const noexcept { return m_bytesWritten; }
    [[nodiscard]] std::size_t bellCount() const noexcept { return m_bellCount; }
    [[nodiscard]] const std::vector<std::string> &serverCutTexts() const noexcept { return m_serverCutTexts; }
    [[nodiscard]] bool securityResultObserved() const noexcept { return m_server.securityResultObserved; }
    [[nodiscard]] bool securityOfferWas38Form() const noexcept { return m_server.securityOfferWas38Form; }

private:
    [[nodiscard]] Error fail(ErrorCode code, std::string detail);
    [[nodiscard]] Error readExact(std::uint8_t *destination, std::size_t size, int timeoutMs, const char *stage);
    [[nodiscard]] Error writeAll(const std::vector<std::uint8_t> &payload, const char *stage);
    [[nodiscard]] Error handshake();
    [[nodiscard]] Error readServerVersion();
    [[nodiscard]] Error negotiateSecurity(std::uint8_t &selectedType);
    [[nodiscard]] Error performVncAuthentication();
    [[nodiscard]] Error readServerInit();
    [[nodiscard]] Error readFramebufferUpdate(UpdateInfo &update);
    [[nodiscard]] Error applyRawRectangle(const RectangleInfo &rectangle);
    [[nodiscard]] Error applyDesktopSize(const RectangleInfo &rectangle, bool &resized);
    [[nodiscard]] Error consumeBytes(std::uint64_t count, std::uint64_t &budget, const char *stage);
    [[nodiscard]] int timeoutOrDefault(int timeoutMs) const noexcept;

    ClientConfig m_config;
    net::Socket m_socket;
    Error m_error;
    ServerInfo m_server;
    PixelFormat m_activeFormat;
    Framebuffer m_framebuffer;
    std::vector<std::uint8_t> m_scratch;
    std::vector<std::string> m_serverCutTexts;
    std::size_t m_bellCount = 0;
    std::uint64_t m_bytesRead = 0;
    std::uint64_t m_bytesWritten = 0;
    std::uint8_t m_offerTypes[32] = {};
    std::size_t m_offerCount = 0;
};

}  // namespace poc::core_remote::rfb
