#include "rfb/client.hpp"

#include "rfb/vnc_auth.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace poc::core_remote::rfb {
namespace {

constexpr std::string_view kVersionPrefix = "RFB ";

bool isPlausibleSecurityType(std::uint32_t type) noexcept
{
    return type == kSecurityTypeNone || type == kSecurityTypeVncAuth;
}

}  // namespace

const char *toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::None:
        return "None";
    case ErrorCode::ConnectFailed:
        return "ConnectFailed";
    case ErrorCode::IoTimeout:
        return "IoTimeout";
    case ErrorCode::IoError:
        return "IoError";
    case ErrorCode::ConnectionClosed:
        return "ConnectionClosed";
    case ErrorCode::ProtocolVersionUnsupported:
        return "ProtocolVersionUnsupported";
    case ErrorCode::SecurityOfferMalformed:
        return "SecurityOfferMalformed";
    case ErrorCode::SecurityTypeNotOffered:
        return "SecurityTypeNotOffered";
    case ErrorCode::SecurityResultFailed:
        return "SecurityResultFailed";
    case ErrorCode::AuthenticationRejected:
        return "AuthenticationRejected";
    case ErrorCode::ServerInitMalformed:
        return "ServerInitMalformed";
    case ErrorCode::FramebufferSizeRejected:
        return "FramebufferSizeRejected";
    case ErrorCode::RectangleCountExceeded:
        return "RectangleCountExceeded";
    case ErrorCode::PayloadSizeExceeded:
        return "PayloadSizeExceeded";
    case ErrorCode::SizeOverflow:
        return "SizeOverflow";
    case ErrorCode::UnsupportedEncoding:
        return "UnsupportedEncoding";
    case ErrorCode::RectangleOutOfBounds:
        return "RectangleOutOfBounds";
    case ErrorCode::PixelFormatUnsupported:
        return "PixelFormatUnsupported";
    case ErrorCode::UnsupportedServerMessage:
        return "UnsupportedServerMessage";
    case ErrorCode::NotConnected:
        return "NotConnected";
    case ErrorCode::AlreadyConnected:
        return "AlreadyConnected";
    case ErrorCode::ReceiveLimitExceeded:
        return "ReceiveLimitExceeded";
    }
    return "Unknown";
}

Client::Client(ClientConfig config)
    : m_config(std::move(config))
    , m_activeFormat(m_config.pixelFormat)
{
}

Client::~Client()
{
    disconnect();
}

Error Client::fail(ErrorCode code, std::string detail)
{
    m_error.code = code;
    m_error.detail = std::move(detail);
    return m_error;
}

bool Client::connected() const noexcept
{
    return m_socket.valid();
}

int Client::timeoutOrDefault(int timeoutMs) const noexcept
{
    return timeoutMs >= 0 ? timeoutMs : m_config.ioTimeoutMs;
}

Error Client::readExact(std::uint8_t *destination,
                        std::size_t size,
                        int timeoutMs,
                        const char *stage)
{
    // The cached error must never describe an earlier operation: every successful sub-step has to
    // report success, or a stale failure would leak into a later return value.
    m_error = Error{};
    if (!m_socket.valid())
        return fail(ErrorCode::NotConnected, "socket is not open");

    std::string detail;
    const net::IoStatus status = m_socket.readExact(destination, size, timeoutOrDefault(timeoutMs), detail);
    if (status != net::IoStatus::Ok) {
        ErrorCode code = ErrorCode::IoError;
        if (status == net::IoStatus::Timeout)
            code = ErrorCode::IoTimeout;
        else if (status == net::IoStatus::Closed)
            code = ErrorCode::ConnectionClosed;
        return fail(code,
                    std::string(stage) + ": expected " + std::to_string(size) + " bytes, got "
                        + net::toString(status)
                        + (detail.empty() ? std::string() : (": " + detail)));
    }
    m_bytesRead += size;
    return m_error;
}

Error Client::writeAll(const std::vector<std::uint8_t> &payload, const char *stage)
{
    m_error = Error{};
    if (!m_socket.valid())
        return fail(ErrorCode::NotConnected, "socket is not open");

    std::string detail;
    const net::IoStatus status = m_socket.writeAll(payload.data(), payload.size(), m_config.ioTimeoutMs, detail);
    if (status != net::IoStatus::Ok)
        return fail(status == net::IoStatus::Timeout ? ErrorCode::IoTimeout : ErrorCode::IoError,
                    std::string(stage) + ": could not send " + std::to_string(payload.size()) + " bytes"
                        + (detail.empty() ? std::string() : (": " + detail)));
    m_bytesWritten += payload.size();
    return m_error;
}

Error Client::connect()
{
    m_error = Error{};
    if (m_socket.valid())
        return fail(ErrorCode::AlreadyConnected, "client is already connected");

    std::string error;
    m_socket = net::Socket::connectTcp(m_config.host, m_config.port, m_config.connectTimeoutMs, error);
    if (!m_socket.valid())
        return fail(ErrorCode::ConnectFailed, "cannot connect to " + m_config.host + ":"
                                                  + std::to_string(m_config.port) + ": " + error);

    m_bytesRead = 0;
    m_bytesWritten = 0;
    m_bellCount = 0;
    m_serverCutTexts.clear();
    m_server = ServerInfo{};
    m_offerCount = 0;

    const Error handshakeError = handshake();
    if (!handshakeError.ok()) {
        disconnect();
        m_error = handshakeError;  // disconnect() must not erase the reason the handshake failed
        return m_error;
    }
    return m_error;
}

Error Client::reconnect()
{
    disconnect();
    return connect();
}

void Client::disconnect() noexcept
{
    if (m_socket.valid()) {
        m_socket.shutdownSend();
        m_socket.close();
    }
}

Error Client::handshake()
{
    Error error = readServerVersion();
    if (!error.ok())
        return error;

    error = negotiateSecurity(m_server.securityTypeUsed);
    if (!error.ok())
        return error;

    if (m_server.securityTypeUsed == kSecurityTypeVncAuth) {
        error = performVncAuthentication();
        if (!error.ok())
            return error;
    }

    // ClientInit: the shared flag.
    {
        Writer writer;
        writer.u8(m_config.shared ? 1U : 0U);
        error = writeAll(writer.data(), "ClientInit");
        if (!error.ok())
            return error;
    }

    error = readServerInit();
    if (!error.ok())
        return error;

    if (m_config.sendPixelFormat) {
        error = setPixelFormat(m_config.pixelFormat);
        if (!error.ok())
            return error;
    }

    std::vector<std::int32_t> encodings{kEncodingRaw};
    if (m_config.advertiseDesktopSize)
        encodings.push_back(kEncodingDesktopSize);
    return setEncodings(encodings);
}

Error Client::readServerVersion()
{
    std::uint8_t buffer[12] = {};
    const Error error = readExact(buffer, sizeof(buffer), -1, "ProtocolVersion");
    if (!error.ok())
        return error;

    const std::string version(reinterpret_cast<const char *>(buffer), sizeof(buffer));
    m_server.versionString = version;

    if (version.compare(0, kVersionPrefix.size(), kVersionPrefix) != 0 || version[11] != '\n')
        return fail(ErrorCode::ProtocolVersionUnsupported, "malformed version string: " + version);

    const auto digit = [&version](std::size_t index) -> int {
        const char character = version[index];
        return (character >= '0' && character <= '9') ? (character - '0') : -1;
    };
    // The two dotted groups are decimal: "RFB 003.008" means major 3, minor 8.
    const int major = digit(4) * 100 + digit(5) * 10 + digit(6);
    const int minor = digit(8) * 100 + digit(9) * 10 + digit(10);
    if (major != 3 || minor < 0)
        return fail(ErrorCode::ProtocolVersionUnsupported, "malformed version string: " + version);

    const std::uint8_t serverMinor = static_cast<std::uint8_t>(minor);
    std::uint8_t chosenMinor = 0;
    switch (m_config.versionMode) {
    case VersionMode::Prefer38:
        if (serverMinor >= 8)
            chosenMinor = 8;
        else if (serverMinor >= 7)
            chosenMinor = 7;
        break;
    case VersionMode::Lenient37:
    case VersionMode::Strict37:
        if (serverMinor >= 7)
            chosenMinor = 7;
        break;
    }
    if (chosenMinor == 0) {
        return fail(ErrorCode::ProtocolVersionUnsupported,
                    "server offers RFB 003." + std::to_string(serverMinor)
                        + ", which is below the 3.7 baseline this client supports");
    }

    m_server.negotiatedMinor = chosenMinor;
    const std::string ourVersion = versionString(chosenMinor);
    std::vector<std::uint8_t> payload(ourVersion.begin(), ourVersion.end());
    return writeAll(payload, "ProtocolVersion");
}

Error Client::negotiateSecurity(std::uint8_t &selectedType)
{
    std::uint8_t first = 0;
    Error error = readExact(&first, 1, -1, "SecurityHandshake");
    if (!error.ok())
        return error;

    bool offerWas38Form = true;
    if (m_config.versionMode == VersionMode::Strict37) {
        // RFC 6143 3.7 form: a 4-byte security type. A strictly-3.7 peer reads exactly this.
        std::uint8_t rest[3] = {};
        error = readExact(rest, sizeof(rest), -1, "SecurityHandshake");
        if (!error.ok())
            return error;
        const std::uint32_t type = (static_cast<std::uint32_t>(first) << 24U)
                                   | (static_cast<std::uint32_t>(rest[0]) << 16U)
                                   | (static_cast<std::uint32_t>(rest[1]) << 8U) | rest[2];
        offerWas38Form = false;
        if (!isPlausibleSecurityType(type)) {
            return fail(ErrorCode::SecurityOfferMalformed,
                        "3.7 security type " + std::to_string(type) + " is not a supported type");
        }
        m_offerCount = 1;
        m_offerTypes[0] = static_cast<std::uint8_t>(type);
    } else if (m_config.versionMode == VersionMode::Lenient37 && first == 0) {
        // A 3.7 u32 security type always starts with 0x00, while a 3.8 count is >= 1, so the first
        // byte alone identifies which framing the server actually used.
        std::uint8_t rest[3] = {};
        error = readExact(rest, sizeof(rest), -1, "SecurityHandshake");
        if (!error.ok())
            return error;
        const std::uint32_t type = (static_cast<std::uint32_t>(rest[0]) << 16U)
                                   | (static_cast<std::uint32_t>(rest[1]) << 8U) | rest[2];
        offerWas38Form = false;
        if (type == 0) {
            return fail(ErrorCode::SecurityTypeNotOffered,
                        "the server refused the connection: it offered no security type");
        }
        if (!isPlausibleSecurityType(type)) {
            return fail(ErrorCode::SecurityOfferMalformed,
                        "security type " + std::to_string(type) + " is not a supported type");
        }
        m_offerCount = 1;
        m_offerTypes[0] = static_cast<std::uint8_t>(type);
    } else {
        const std::size_t count = first;
        if (count == 0) {
            return fail(ErrorCode::SecurityTypeNotOffered,
                        "the server refused the connection: it offered no security type");
        }
        if (count > sizeof(m_offerTypes)) {
            return fail(ErrorCode::SecurityOfferMalformed,
                        "the server offered " + std::to_string(count) + " security types");
        }
        error = readExact(m_offerTypes, count, -1, "SecurityHandshake");
        if (!error.ok())
            return error;
        m_offerCount = count;
    }

    m_server.securityOfferWas38Form = offerWas38Form;

    const auto offered = [this](std::uint8_t type) {
        return std::find(m_offerTypes, m_offerTypes + m_offerCount, type) != m_offerTypes + m_offerCount;
    };

    SecurityPreference preference = m_config.securityPreference;
    if (preference == SecurityPreference::Auto)
        preference = m_config.password.empty() ? SecurityPreference::None : SecurityPreference::VncAuth;

    const std::uint8_t wanted = preference == SecurityPreference::VncAuth ? kSecurityTypeVncAuth
                                                                         : kSecurityTypeNone;
    if (!offered(wanted)) {
        if (wanted == kSecurityTypeVncAuth) {
            return fail(ErrorCode::SecurityTypeNotOffered,
                        "the server does not offer VNC authentication (type 2)");
        }
        return fail(ErrorCode::SecurityTypeNotOffered,
                    "the server does not offer SecurityType None (type 1)");
    }

    selectedType = wanted;
    Writer writer;
    writer.u8(wanted);
    error = writeAll(writer.data(), "SecurityHandshake");
    if (!error.ok())
        return error;

    if (wanted == kSecurityTypeVncAuth)
        return m_error;  // SecurityResult is read as part of the challenge/response exchange

    // SecurityResult for SecurityType None. RFB 3.8 always sends it; 3.7 only sends it when the
    // selected type needs it, so the strictly-3.7 path tolerates its absence instead of desyncing.
    std::uint8_t result[4] = {};
    if (m_config.versionMode == VersionMode::Strict37 && !offerWas38Form) {
        std::string waitError;
        if (m_socket.waitReadable(200, waitError) != net::IoStatus::Ok)
            return m_error;  // no SecurityResult, which is what a 3.7 server does for None
    }

    error = readExact(result, sizeof(result), -1, "SecurityResult");
    if (!error.ok())
        return error;
    m_server.securityResultObserved = true;
    const std::uint32_t value = (static_cast<std::uint32_t>(result[0]) << 24U)
                                | (static_cast<std::uint32_t>(result[1]) << 16U)
                                | (static_cast<std::uint32_t>(result[2]) << 8U) | result[3];
    if (value != 0)
        return fail(ErrorCode::SecurityResultFailed, "SecurityResult reported failure");
    return m_error;
}

Error Client::performVncAuthentication()
{
    std::array<std::uint8_t, kVncChallengeBytes> challenge{};
    Error error = readExact(challenge.data(), challenge.size(), -1, "VncAuthentication");
    if (!error.ok())
        return error;

    std::array<std::uint8_t, kVncChallengeBytes> response{};
    std::string authError;
    if (!vncAuthResponse(m_config.password.data(), m_config.password.size(), challenge, response, authError))
        return fail(ErrorCode::AuthenticationRejected, "cannot answer the challenge: " + authError);

    std::vector<std::uint8_t> payload(response.begin(), response.end());
    error = writeAll(payload, "VncAuthentication");
    if (!error.ok())
        return error;

    std::uint8_t result[4] = {};
    error = readExact(result, sizeof(result), -1, "VncAuthentication");
    if (!error.ok())
        return error;
    m_server.securityResultObserved = true;

    const std::uint32_t value = (static_cast<std::uint32_t>(result[0]) << 24U)
                                | (static_cast<std::uint32_t>(result[1]) << 16U)
                                | (static_cast<std::uint32_t>(result[2]) << 8U) | result[3];
    if (value != 0)
        return fail(ErrorCode::AuthenticationRejected, "the server rejected the VNC authentication response");
    return m_error;
}

Error Client::readServerInit()
{
    std::uint8_t buffer[24] = {};
    Error error = readExact(buffer, sizeof(buffer), -1, "ServerInit");
    if (!error.ok())
        return error;

    Reader reader(buffer, sizeof(buffer));
    const std::uint16_t width = reader.u16();
    const std::uint16_t height = reader.u16();
    PixelFormat format;
    format.bitsPerPixel = reader.u8();
    format.depth = reader.u8();
    format.bigEndian = reader.u8() != 0;
    format.trueColor = reader.u8() != 0;
    format.redMax = reader.u16();
    format.greenMax = reader.u16();
    format.blueMax = reader.u16();
    format.redShift = reader.u8();
    format.greenShift = reader.u8();
    format.blueShift = reader.u8();
    reader.skip(3);
    const std::uint32_t nameLength = reader.u32();
    if (reader.failed())
        return fail(ErrorCode::ServerInitMalformed, "ServerInit was truncated");

    if (width == 0 || height == 0)
        return fail(ErrorCode::ServerInitMalformed, "ServerInit declared a zero-sized framebuffer");
    if (width > m_config.limits.maxFramebufferWidth || height > m_config.limits.maxFramebufferHeight) {
        return fail(ErrorCode::FramebufferSizeRejected,
                    "ServerInit declared " + std::to_string(width) + "x" + std::to_string(height)
                        + ", above the configured bound of "
                        + std::to_string(m_config.limits.maxFramebufferWidth) + "x"
                        + std::to_string(m_config.limits.maxFramebufferHeight));
    }

    std::string formatReason;
    if (!format.valid(formatReason))
        return fail(ErrorCode::PixelFormatUnsupported, "ServerInit pixel format: " + formatReason);

    if (nameLength > m_config.limits.maxServerNameBytes) {
        return fail(ErrorCode::PayloadSizeExceeded,
                    "ServerInit name length " + std::to_string(nameLength) + " exceeds the bound of "
                        + std::to_string(m_config.limits.maxServerNameBytes));
    }

    std::string name;
    if (nameLength > 0) {
        std::vector<std::uint8_t> nameBytes(nameLength, 0);
        error = readExact(nameBytes.data(), nameBytes.size(), -1, "ServerInit");
        if (!error.ok())
            return error;
        name.assign(reinterpret_cast<const char *>(nameBytes.data()), nameBytes.size());
    }

    m_server.framebufferWidth = width;
    m_server.framebufferHeight = height;
    m_server.serverPixelFormat = format;
    m_server.serverName = name;
    if (!m_config.sendPixelFormat)
        m_activeFormat = format;
    m_framebuffer.reset(width, height, 0);
    return m_error;
}

Error Client::setPixelFormat(const PixelFormat &format)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "setPixelFormat before connect");

    std::string reason;
    if (!format.valid(reason))
        return fail(ErrorCode::PixelFormatUnsupported, reason);

    Writer writer;
    writer.u8(kClientMessageSetPixelFormat);
    writer.fill(0, 3);
    writer.u8(format.bitsPerPixel);
    writer.u8(format.depth);
    writer.u8(format.bigEndian ? 1U : 0U);
    writer.u8(format.trueColor ? 1U : 0U);
    writer.u16(format.redMax);
    writer.u16(format.greenMax);
    writer.u16(format.blueMax);
    writer.u8(format.redShift);
    writer.u8(format.greenShift);
    writer.u8(format.blueShift);
    writer.fill(0, 3);

    const Error error = writeAll(writer.data(), "SetPixelFormat");
    if (error.ok())
        m_activeFormat = format;
    return error;
}

Error Client::setEncodings(const std::vector<std::int32_t> &encodings)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "setEncodings before connect");
    if (encodings.empty() || encodings.size() > m_config.limits.maxAdvertisedEncodings) {
        return fail(ErrorCode::PayloadSizeExceeded,
                    "the client may advertise 1 to " + std::to_string(m_config.limits.maxAdvertisedEncodings)
                        + " encodings");
    }

    Writer writer;
    writer.u8(kClientMessageSetEncodings);
    writer.u8(0);
    writer.u16(static_cast<std::uint16_t>(encodings.size()));
    for (const std::int32_t encoding : encodings)
        writer.i32(encoding);
    return writeAll(writer.data(), "SetEncodings");
}

Error Client::requestFramebufferUpdate(bool incremental,
                                       std::uint16_t x,
                                       std::uint16_t y,
                                       std::uint16_t width,
                                       std::uint16_t height)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "FramebufferUpdateRequest before connect");

    Writer writer;
    writer.u8(kClientMessageFramebufferUpdateRequest);
    writer.u8(incremental ? 1U : 0U);
    writer.u16(x);
    writer.u16(y);
    writer.u16(width);
    writer.u16(height);
    return writeAll(writer.data(), "FramebufferUpdateRequest");
}

Error Client::requestFullUpdate()
{
    return requestFramebufferUpdate(false, 0, 0, m_framebuffer.width(), m_framebuffer.height());
}

Error Client::readServerMessage(ServerMessage &message, int timeoutMs)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "readServerMessage before connect");

    std::uint8_t type = 0;
    Error error = readExact(&type, 1, timeoutMs, "ServerMessage");
    if (!error.ok())
        return error;

    message = ServerMessage{};
    switch (type) {
    case kServerMessageFramebufferUpdate: {
        message.kind = ServerMessageKind::FramebufferUpdate;
        return readFramebufferUpdate(message.update);
    }
    case kServerMessageBell: {
        // A bell carries no payload at all; recording it keeps the message stream unambiguous.
        ++m_bellCount;
        message.kind = ServerMessageKind::Bell;
        return m_error;
    }
    case kServerMessageServerCutText: {
        std::uint8_t header[7] = {};
        error = readExact(header, sizeof(header), timeoutMs, "ServerCutText");
        if (!error.ok())
            return error;
        Reader reader(header, sizeof(header));
        reader.skip(3);
        const std::uint32_t length = reader.u32();
        if (length > m_config.limits.maxCutTextBytes) {
            return fail(ErrorCode::PayloadSizeExceeded,
                        "ServerCutText length " + std::to_string(length) + " exceeds the bound of "
                            + std::to_string(m_config.limits.maxCutTextBytes));
        }
        std::vector<std::uint8_t> payload(length, 0);
        if (length > 0) {
            error = readExact(payload.data(), payload.size(), timeoutMs, "ServerCutText");
            if (!error.ok())
                return error;
        }
        message.kind = ServerMessageKind::ServerCutText;
        message.cutText.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
        m_serverCutTexts.push_back(message.cutText);
        return m_error;
    }
    case kServerMessageSetColourMapEntries:
        return fail(ErrorCode::UnsupportedServerMessage,
                    "SetColourMapEntries is not part of this client's pixel format contract");
    default:
        return fail(ErrorCode::UnsupportedServerMessage,
                    "unsupported server message type " + std::to_string(type));
    }
}

Error Client::waitForFramebufferUpdate(UpdateInfo &update, int timeoutMs)
{
    ServerMessage message;
    for (;;) {
        const Error error = readServerMessage(message, timeoutMs);
        if (!error.ok())
            return error;
        if (message.kind == ServerMessageKind::FramebufferUpdate) {
            update = message.update;
            return m_error;
        }
    }
}

Error Client::consumeBytes(std::uint64_t count, std::uint64_t &budget, const char *stage)
{
    m_error = Error{};
    if (count > budget) {
        return fail(ErrorCode::ReceiveLimitExceeded,
                    std::string(stage) + ": " + std::to_string(count)
                        + " more bytes would exceed the configured pending-receive bound of "
                        + std::to_string(m_config.limits.maxPendingReceiveBytes));
    }
    budget -= count;
    return m_error;
}

Error Client::readFramebufferUpdate(UpdateInfo &update)
{
    std::uint8_t header[3] = {};
    Error error = readExact(header, sizeof(header), -1, "FramebufferUpdate");
    if (!error.ok())
        return error;

    Reader reader(header, sizeof(header));
    (void)reader.u8();  // padding
    const std::uint16_t rectangleCount = reader.u16();

    if (rectangleCount > m_config.limits.maxRectangleCount) {
        return fail(ErrorCode::RectangleCountExceeded,
                    "FramebufferUpdate announced " + std::to_string(rectangleCount)
                        + " rectangles, above the configured bound of "
                        + std::to_string(m_config.limits.maxRectangleCount));
    }

    std::uint64_t budget = m_config.limits.maxPendingReceiveBytes;
    update = UpdateInfo{};
    update.rectangles.reserve(rectangleCount);

    for (std::uint16_t index = 0; index < rectangleCount; ++index) {
        std::uint8_t raw[12] = {};
        error = readExact(raw, sizeof(raw), -1, "FramebufferUpdate");
        if (!error.ok())
            return error;

        Reader rectReader(raw, sizeof(raw));
        RectangleInfo rectangle;
        rectangle.x = rectReader.u16();
        rectangle.y = rectReader.u16();
        rectangle.width = rectReader.u16();
        rectangle.height = rectReader.u16();
        rectangle.encoding = rectReader.i32();
        update.rectangles.push_back(rectangle);

        if (rectangle.encoding == kEncodingDesktopSize) {
            error = applyDesktopSize(rectangle, update.resized);
            if (!error.ok())
                return error;
            continue;
        }
        if (rectangle.encoding != kEncodingRaw) {
            return fail(ErrorCode::UnsupportedEncoding,
                        "rectangle " + std::to_string(index) + " used encoding "
                            + std::to_string(rectangle.encoding)
                            + ", which this client does not implement");
        }

        // The per-rectangle bound is evaluated first so an oversized rectangle is reported as such,
        // rather than being folded into the cumulative budget diagnostic. Both checks happen before
        // any allocation.
        const auto pixels = checkedMul(rectangle.width, rectangle.height);
        if (!pixels)
            return fail(ErrorCode::SizeOverflow, "raw rectangle geometry overflows a 64-bit size");
        const auto byteCount = checkedMul(*pixels, m_activeFormat.bytesPerPixel());
        if (!byteCount)
            return fail(ErrorCode::SizeOverflow, "raw rectangle pixel-byte count overflows a 64-bit size");
        if (*byteCount > m_config.limits.maxRectangleBytes) {
            return fail(ErrorCode::PayloadSizeExceeded,
                        "raw rectangle payload of " + std::to_string(*byteCount)
                            + " bytes exceeds the bound of "
                            + std::to_string(m_config.limits.maxRectangleBytes));
        }

        error = consumeBytes(*byteCount, budget, "RawRectangle");
        if (!error.ok())
            return error;

        error = applyRawRectangle(rectangle);
        if (!error.ok())
            return error;
        update.rawBytes += static_cast<std::uint64_t>(rectangle.width) * rectangle.height
                           * m_activeFormat.bytesPerPixel();
    }

    return m_error;
}

Error Client::applyRawRectangle(const RectangleInfo &rectangle)
{
    const std::uint8_t bytesPerPixel = m_activeFormat.bytesPerPixel();
    const auto pixels = checkedMul(rectangle.width, rectangle.height);
    if (!pixels)
        return fail(ErrorCode::SizeOverflow, "raw rectangle geometry overflows a 64-bit size");
    const auto byteCount = checkedMul(*pixels, bytesPerPixel);
    if (!byteCount)
        return fail(ErrorCode::SizeOverflow, "raw rectangle pixel-byte count overflows a 64-bit size");
    if (*byteCount > m_config.limits.maxRectangleBytes) {
        return fail(ErrorCode::PayloadSizeExceeded,
                    "raw rectangle payload of " + std::to_string(*byteCount) + " bytes exceeds the bound of "
                        + std::to_string(m_config.limits.maxRectangleBytes));
    }

    // The allocation happens only after every bound above has been satisfied.
    m_scratch.resize(static_cast<std::size_t>(*byteCount));
    Error error = readExact(m_scratch.data(), m_scratch.size(), -1, "RawRectangle");
    if (!error.ok())
        return error;

    std::string blitError;
    if (!m_framebuffer.blitRaw(rectangle.x,
                               rectangle.y,
                               rectangle.width,
                               rectangle.height,
                               m_scratch.data(),
                               m_scratch.size(),
                               m_activeFormat,
                               blitError)) {
        return fail(ErrorCode::RectangleOutOfBounds,
                    "raw rectangle " + std::to_string(rectangle.x) + "," + std::to_string(rectangle.y) + " "
                        + std::to_string(rectangle.width) + "x" + std::to_string(rectangle.height)
                        + " cannot be applied: " + blitError);
    }
    return m_error;
}

Error Client::applyDesktopSize(const RectangleInfo &rectangle, bool &resized)
{
    if (rectangle.width == 0 || rectangle.height == 0)
        return fail(ErrorCode::FramebufferSizeRejected, "DesktopSize announced a zero-sized framebuffer");
    if (rectangle.width > m_config.limits.maxFramebufferWidth
        || rectangle.height > m_config.limits.maxFramebufferHeight) {
        return fail(ErrorCode::FramebufferSizeRejected,
                    "DesktopSize announced " + std::to_string(rectangle.width) + "x"
                        + std::to_string(rectangle.height) + ", above the configured bound");
    }

    // The pseudo-encoding defines the new size; the contents are explicitly undefined, so the client
    // clears the surface and relies on the following rectangle(s) to fill it.
    m_framebuffer.reset(rectangle.width, rectangle.height, 0);
    m_server.framebufferWidth = rectangle.width;
    m_server.framebufferHeight = rectangle.height;
    resized = true;
    return m_error;
}

Error Client::sendKeyEvent(std::uint32_t keysym, bool pressed)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "sendKeyEvent before connect");
    Writer writer;
    writer.u8(kClientMessageKeyEvent);
    writer.u8(pressed ? 1U : 0U);
    writer.fill(0, 2);
    writer.u32(keysym);
    return writeAll(writer.data(), "KeyEvent");
}

Error Client::sendPointerEvent(std::uint8_t buttonMask, std::uint16_t x, std::uint16_t y)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "sendPointerEvent before connect");
    Writer writer;
    writer.u8(kClientMessagePointerEvent);
    writer.u8(buttonMask);
    writer.u16(x);
    writer.u16(y);
    return writeAll(writer.data(), "PointerEvent");
}

Error Client::sendWheel(int verticalNotches,
                        int horizontalNotches,
                        std::uint16_t x,
                        std::uint16_t y)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "sendWheel before connect");
    m_error = Error{};

    // One notch is one press/release pair of the corresponding mask bit: the protocol has no scroll
    // event of its own.
    const auto pressRelease = [this, x, y](std::uint8_t bit) -> Error {
        Error error = sendPointerEvent(bit, x, y);
        if (!error.ok())
            return error;
        return sendPointerEvent(0, x, y);
    };

    for (int index = 0; index < verticalNotches; ++index) {
        const Error error = pressRelease(kWheelUp);
        if (!error.ok())
            return error;
    }
    for (int index = 0; index < -verticalNotches; ++index) {
        const Error error = pressRelease(kWheelDown);
        if (!error.ok())
            return error;
    }
    for (int index = 0; index < horizontalNotches; ++index) {
        const Error error = pressRelease(kWheelRight);
        if (!error.ok())
            return error;
    }
    for (int index = 0; index < -horizontalNotches; ++index) {
        const Error error = pressRelease(kWheelLeft);
        if (!error.ok())
            return error;
    }
    return m_error;
}

Error Client::sendClientCutText(const std::string &text)
{
    if (!connected())
        return fail(ErrorCode::NotConnected, "sendClientCutText before connect");
    if (text.size() > m_config.limits.maxCutTextBytes)
        return fail(ErrorCode::PayloadSizeExceeded, "ClientCutText payload exceeds the configured bound");

    Writer writer;
    writer.u8(kClientMessageClientCutText);
    writer.fill(0, 3);
    writer.u32(static_cast<std::uint32_t>(text.size()));
    writer.bytes(text.data(), text.size());
    return writeAll(writer.data(), "ClientCutText");
}

}  // namespace poc::core_remote::rfb
