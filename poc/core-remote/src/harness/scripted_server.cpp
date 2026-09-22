#include "harness/scripted_server.hpp"

#include "rfb/vnc_auth.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace poc::core_remote::harness {
namespace {

using rfb::kEncodingDesktopSize;
using rfb::kEncodingRaw;
using rfb::kSecurityTypeNone;
using rfb::kSecurityTypeVncAuth;
using rfb::Reader;
using rfb::Writer;

constexpr int kIoTimeoutMs = 5000;

bool sendBytes(net::Socket &socket, const std::vector<std::uint8_t> &data, std::string &error)
{
    return socket.writeAll(data.data(), data.size(), kIoTimeoutMs, error) == net::IoStatus::Ok;
}

bool sendBytes(net::Socket &socket, Writer &writer, std::string &error)
{
    return sendBytes(socket, writer.data(), error);
}

// The challenge is fixed rather than random: the harness must be reproducible, and a fixed challenge
// still exercises the full DES path. Interoperability with a *real* server challenge is proven by the
// HyRemote runtime fixture, not here.
std::uint8_t challengeByte(std::size_t index)
{
    return static_cast<std::uint8_t>((index * 11U + 3U) & 0xFFU);
}

struct ConnectionState
{
    rfb::PixelFormat format = rfb::PixelFormat::hyremoteNative();
    std::uint16_t framebufferWidth = kInitialWidth;
    std::uint16_t framebufferHeight = kInitialHeight;
    bool desktopSizeSupported = false;
    std::size_t nextStep = 0;
    SessionRecord record;
};

void appendServerInit(Writer &writer,
                      std::uint16_t width,
                      std::uint16_t height,
                      const rfb::PixelFormat &format,
                      const std::string &name)
{
    writer.u16(width);
    writer.u16(height);
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
    writer.u32(static_cast<std::uint32_t>(name.size()));
    writer.bytes(name.data(), name.size());
}

}  // namespace

std::unique_ptr<ScriptedServer> ScriptedServer::start(const ScriptedServerConfig &config, std::string &error)
{
    std::unique_ptr<ScriptedServer> server(new ScriptedServer());
    server->m_config = config;
    server->m_listener = net::Listener::bindTcp("127.0.0.1", 0, 4, error);
    if (!server->m_listener.valid())
        return nullptr;

    server->m_thread = std::thread([raw = server.get()] { raw->run(); });
    return server;
}

ScriptedServer::~ScriptedServer()
{
    stop();
}

std::uint16_t ScriptedServer::port() const noexcept
{
    return m_listener.port();
}

std::size_t ScriptedServer::sessionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

std::vector<SessionRecord> ScriptedServer::sessions() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions;
}

SessionRecord ScriptedServer::lastSession() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.empty() ? SessionRecord{} : m_sessions.back();
}

bool ScriptedServer::waitForSessions(std::size_t count, int timeoutMs) const
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sessions.size() >= count)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size() >= count;
}

void ScriptedServer::stop() noexcept
{
    m_stop.store(true);
    m_listener.close();
    if (m_thread.joinable())
        m_thread.join();
}

void ScriptedServer::run() noexcept
{
    while (!m_stop.load()) {
        net::Socket client;
        std::string error;
        const net::IoStatus status = m_listener.accept(client, 200, error);
        if (status == net::IoStatus::Ok) {
            serveOne(client);
            continue;
        }
        if (status == net::IoStatus::Closed)
            return;  // listener closed by stop()
        // Timeout simply means no client showed up yet.
    }
}

void ScriptedServer::serveOne(net::Socket &client) noexcept
{
    const auto recordSession = [this](const SessionRecord &record) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.push_back(record);
    };

    std::string ioError;
    ConnectionState state;

    // ---------------------------------------------------------------- protocol version

    if (m_config.behaviour == ScriptedBehaviour::BadProtocolVersion) {
        // Below the 3.7 baseline this client supports.
        const std::string version = "RFB 003.006\n";
        std::vector<std::uint8_t> payload(version.begin(), version.end());
        (void)sendBytes(client, payload, ioError);
        recordSession(state.record);
        return;
    }

    const bool strictThreeSeven = m_config.behaviour == ScriptedBehaviour::StrictThreeSeven;
    const std::string serverVersion = strictThreeSeven ? rfb::versionString(7) : rfb::versionString(8);
    {
        std::vector<std::uint8_t> payload(serverVersion.begin(), serverVersion.end());
        if (!sendBytes(client, payload, ioError)) {
            recordSession(state.record);
            return;
        }
    }

    std::uint8_t clientVersion[12] = {};
    if (client.readExact(clientVersion, sizeof(clientVersion), kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
        recordSession(state.record);
        return;
    }
    state.record.clientVersion.assign(reinterpret_cast<const char *>(clientVersion), sizeof(clientVersion));

    // ---------------------------------------------------------------- security offer

    if (m_config.behaviour == ScriptedBehaviour::ZeroSecurityTypes) {
        const std::uint8_t zero = 0;
        (void)client.writeAll(&zero, 1, kIoTimeoutMs, ioError);
        recordSession(state.record);
        return;
    }
    if (m_config.behaviour == ScriptedBehaviour::MalformedSecurityOfferCount) {
        const std::uint8_t offer[2] = {0xFF, kSecurityTypeNone};
        (void)client.writeAll(offer, sizeof(offer), kIoTimeoutMs, ioError);
        recordSession(state.record);
        return;
    }

    const std::uint8_t offeredType = m_config.securityType == kSecurityTypeVncAuth ? kSecurityTypeVncAuth
                                                                                   : kSecurityTypeNone;
    {
        Writer offer;
        if (strictThreeSeven) {
            offer.u32(offeredType);  // RFC 6143 3.7 form: a 4-byte security type
        } else {
            offer.u8(1);
            offer.u8(offeredType);
        }
        if (!sendBytes(client, offer, ioError)) {
            recordSession(state.record);
            return;
        }
    }

    std::uint8_t selected = 0;
    if (client.readExact(&selected, 1, kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
        recordSession(state.record);
        return;
    }
    state.record.selectedSecurityType = selected;

    if (selected == kSecurityTypeVncAuth) {
        std::array<std::uint8_t, rfb::kVncChallengeBytes> challenge{};
        for (std::size_t index = 0; index < challenge.size(); ++index)
            challenge[index] = challengeByte(index);
        if (client.writeAll(challenge.data(), challenge.size(), kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
            recordSession(state.record);
            return;
        }
        state.record.vncChallengeSent = true;

        std::array<std::uint8_t, rfb::kVncChallengeBytes> response{};
        if (client.readExact(response.data(), response.size(), kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
            recordSession(state.record);
            return;
        }
        state.record.vncResponseHadExpectedLength = true;

        // The double checks the shape and, as a convenience, that the client's answer matches the
        // response this harness computes for the configured password. This is deliberately *not*
        // treated as proof of interoperability: only the HyRemote fixture verifies against the
        // server's own OpenSSL-based implementation.
        std::array<std::uint8_t, rfb::kVncChallengeBytes> expected{};
        std::string authError;
        if (!m_config.password.empty()
            && rfb::vncAuthResponse(m_config.password.data(), m_config.password.size(), challenge, expected,
                                    authError)) {
            state.record.vncResponseMatchedLocalComputation = expected == response;
        }

        Writer result;
        result.u32(state.record.vncResponseMatchedLocalComputation ? 0U : 1U);
        if (!sendBytes(client, result, ioError)) {
            recordSession(state.record);
            return;
        }
        if (!state.record.vncResponseMatchedLocalComputation) {
            recordSession(state.record);
            return;
        }
    } else if (selected == kSecurityTypeNone) {
        // A genuinely 3.7 server does not send SecurityResult for SecurityType None; the 3.8-form
        // exchange does, and HyRemote always does.
        if (!strictThreeSeven) {
            Writer result;
            result.u32(0);
            if (!sendBytes(client, result, ioError)) {
                recordSession(state.record);
                return;
            }
        }
    } else {
        recordSession(state.record);
        return;
    }

    // ---------------------------------------------------------------- ClientInit / ServerInit

    std::uint8_t shared = 0;
    if (client.readExact(&shared, 1, kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
        recordSession(state.record);
        return;
    }
    state.record.clientInitShared = shared != 0;

    std::uint16_t initWidth = m_config.serverInitWidth;
    std::uint16_t initHeight = m_config.serverInitHeight;
    if (m_config.behaviour == ScriptedBehaviour::ZeroSizedServerInit) {
        initWidth = 0;
        initHeight = 0;
    } else if (m_config.behaviour == ScriptedBehaviour::AbsurdFramebufferSize) {
        initWidth = 60000;
        initHeight = 60000;
    }

    {
        Writer init;
        appendServerInit(init, initWidth, initHeight, rfb::PixelFormat::hyremoteNative(),
                         "POC-Scripted-Server");
        if (!sendBytes(client, init, ioError)) {
            recordSession(state.record);
            return;
        }
    }

    if (m_config.behaviour == ScriptedBehaviour::AbsurdFramebufferSize
        || m_config.behaviour == ScriptedBehaviour::ZeroSizedServerInit) {
        // The client is expected to reject ServerInit; nothing further is served.
        recordSession(state.record);
        return;
    }

    state.framebufferWidth = initWidth;
    state.framebufferHeight = initHeight;

    if (m_config.behaviour == ScriptedBehaviour::ServerCutTextPresent) {
        static const std::string kText = "poc-clipboard-probe";
        Writer cut;
        cut.u8(rfb::kServerMessageServerCutText);
        cut.fill(0, 3);
        cut.u32(static_cast<std::uint32_t>(kText.size()));
        cut.bytes(kText.data(), kText.size());
        (void)sendBytes(client, cut, ioError);
    }

    if (m_config.behaviour == ScriptedBehaviour::UnsupportedServerMessage) {
        Writer map;
        map.u8(rfb::kServerMessageSetColourMapEntries);
        map.u8(0);
        map.u16(0);
        map.u16(m_config.colourMapEntryCount == 0 ? 512 : static_cast<std::uint16_t>(m_config.colourMapEntryCount));
        map.fill(0, 6 * 2);
        (void)sendBytes(client, map, ioError);
    }

    // ---------------------------------------------------------------- message loop

    for (;;) {
        std::uint8_t type = 0;
        const net::IoStatus status = client.readExact(&type, 1, kIoTimeoutMs, ioError);
        if (status != net::IoStatus::Ok)
            break;

        if (type == rfb::kClientMessageSetPixelFormat) {
            std::uint8_t tail[19] = {};
            if (client.readExact(tail, sizeof(tail), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            ReceivedMessage message;
            message.type = type;
            rfb::Reader reader(tail, sizeof(tail));
            (void)reader.u8();
            (void)reader.u8();
            (void)reader.u8();
            rfb::PixelFormat format;
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
            message.pixelFormat = format;
            if (!reader.failed() && format.bytesPerPixel() != 0 && format.trueColor) {
                state.format = format;
                message.pixelFormat = format;
            }
            state.record.messages.push_back(message);
            continue;
        }

        if (type == rfb::kClientMessageSetEncodings) {
            std::uint8_t head[3] = {};
            if (client.readExact(head, sizeof(head), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            const std::uint16_t count = static_cast<std::uint16_t>((head[1] << 8U) | head[2]);
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(count) * 4, 0);
            if (count > 0
                && client.readExact(payload.data(), payload.size(), kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
                break;
            }
            ReceivedMessage message;
            message.type = type;
            rfb::Reader reader(payload.data(), payload.size());
            for (std::uint16_t index = 0; index < count; ++index) {
                const std::int32_t encoding = reader.i32();
                message.encodings.push_back(encoding);
                if (encoding == kEncodingDesktopSize)
                    state.desktopSizeSupported = true;
            }
            state.record.messages.push_back(message);
            continue;
        }

        if (type == rfb::kClientMessageFramebufferUpdateRequest) {
            std::uint8_t tail[9] = {};
            if (client.readExact(tail, sizeof(tail), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            ReceivedMessage message;
            message.type = type;
            rfb::Reader reader(tail, sizeof(tail));
            message.flags = reader.u8();
            message.x = reader.u16();
            message.y = reader.u16();
            message.width = reader.u16();
            message.height = reader.u16();
            state.record.messages.push_back(message);

            // ---- deliberate violations, one per negative requirement -------------------------
            if (m_config.behaviour == ScriptedBehaviour::HeaderTruncatedMidMessage) {
                const std::uint8_t partial[2] = {rfb::kServerMessageFramebufferUpdate, 0};
                (void)client.writeAll(partial, sizeof(partial), kIoTimeoutMs, ioError);
                break;
            }
            if (m_config.behaviour == ScriptedBehaviour::RectangleCountOverflow) {
                Writer update;
                update.u8(rfb::kServerMessageFramebufferUpdate);
                update.u8(0);
                update.u16(m_config.rectangleCount);
                (void)sendBytes(client, update, ioError);
                break;
            }
            if (m_config.behaviour == ScriptedBehaviour::UnsupportedEncoding) {
                Writer update;
                update.u8(rfb::kServerMessageFramebufferUpdate);
                update.u8(0);
                update.u16(1);
                update.u16(0);
                update.u16(0);
                update.u16(16);
                update.u16(16);
                update.i32(5);  // Hextile: never advertised by this client
                (void)sendBytes(client, update, ioError);
                break;
            }
            if (m_config.behaviour == ScriptedBehaviour::OversizedRectanglePayload) {
                Writer update;
                update.u8(rfb::kServerMessageFramebufferUpdate);
                update.u8(0);
                update.u16(1);
                update.u16(0);
                update.u16(0);
                update.u16(65535);
                update.u16(65535);
                update.i32(kEncodingRaw);
                (void)sendBytes(client, update, ioError);
                break;
            }
            if (m_config.behaviour == ScriptedBehaviour::TruncatedRectangle) {
                Writer update;
                update.u8(rfb::kServerMessageFramebufferUpdate);
                update.u8(0);
                update.u16(1);
                update.u16(0);
                update.u16(0);
                update.u16(16);
                update.u16(16);
                update.i32(kEncodingRaw);
                update.fill(0x5A, 32);  // far short of the 1024 bytes the rectangle promises
                (void)sendBytes(client, update, ioError);
                break;
            }

            // ---- conformant serving ---------------------------------------------------------
            const int stepIndex =
                m_config.steps.empty()
                    ? 0
                    : m_config.steps[std::min(state.nextStep, m_config.steps.size() - 1)];
            ++state.nextStep;
            const Step &definition = step(stepIndex);

            const bool resized = definition.width != state.framebufferWidth
                                 || definition.height != state.framebufferHeight;
            if (resized && !state.desktopSizeSupported)
                break;  // a client that did not advertise DesktopSize cannot be resized

            Rect region{0, 0, definition.width, definition.height};
            if (!resized) {
                const std::uint32_t right = static_cast<std::uint32_t>(message.x) + message.width;
                const std::uint32_t bottom = static_cast<std::uint32_t>(message.y) + message.height;
                const std::uint32_t clippedRight = std::min<std::uint32_t>(right, definition.width);
                const std::uint32_t clippedBottom = std::min<std::uint32_t>(bottom, definition.height);
                if (message.x >= definition.width || message.y >= definition.height)
                    continue;
                region = Rect{message.x,
                              message.y,
                              static_cast<std::uint16_t>(clippedRight - message.x),
                              static_cast<std::uint16_t>(clippedBottom - message.y)};
            }

            Writer update;
            update.u8(rfb::kServerMessageFramebufferUpdate);
            update.u8(0);
            update.u16(resized ? 2 : 1);
            if (resized) {
                update.u16(0);
                update.u16(0);
                update.u16(definition.width);
                update.u16(definition.height);
                update.i32(kEncodingDesktopSize);
            }
            update.u16(region.x);
            update.u16(region.y);
            update.u16(region.width);
            update.u16(region.height);
            update.i32(kEncodingRaw);
            const std::vector<std::uint8_t> pixels = encodeRect(stepIndex, region, state.format);
            update.bytes(pixels.data(), pixels.size());

            if (!sendBytes(client, update, ioError))
                break;

            state.framebufferWidth = definition.width;
            state.framebufferHeight = definition.height;
            ++state.record.updatesServed;
            continue;
        }

        if (type == rfb::kClientMessageKeyEvent) {
            std::uint8_t tail[7] = {};
            if (client.readExact(tail, sizeof(tail), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            ReceivedMessage message;
            message.type = type;
            rfb::Reader reader(tail, sizeof(tail));
            message.flags = reader.u8();
            (void)reader.u16();
            message.keysym = reader.u32();
            state.record.messages.push_back(message);
            continue;
        }

        if (type == rfb::kClientMessagePointerEvent) {
            std::uint8_t tail[5] = {};
            if (client.readExact(tail, sizeof(tail), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            ReceivedMessage message;
            message.type = type;
            rfb::Reader reader(tail, sizeof(tail));
            message.flags = reader.u8();
            message.x = reader.u16();
            message.y = reader.u16();
            state.record.messages.push_back(message);
            continue;
        }

        if (type == rfb::kClientMessageClientCutText) {
            std::uint8_t head[7] = {};
            if (client.readExact(head, sizeof(head), kIoTimeoutMs, ioError) != net::IoStatus::Ok)
                break;
            rfb::Reader reader(head, sizeof(head));
            reader.skip(3);
            const std::uint32_t length = reader.u32();
            std::vector<std::uint8_t> payload(length, 0);
            if (length > 0
                && client.readExact(payload.data(), payload.size(), kIoTimeoutMs, ioError) != net::IoStatus::Ok) {
                break;
            }
            ReceivedMessage message;
            message.type = type;
            message.cutText.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
            state.record.messages.push_back(message);
            continue;
        }

        break;  // unknown client message: end this session
    }

    recordSession(state.record);
}

}  // namespace poc::core_remote::harness
