#pragma once

// POC_ONLY scripted RFB server (test double).
//
// Two jobs, and nothing else:
//   * serve the deterministic content in a controllable way, so client behaviour that does not need
//     HyRemote (framing variants, incremental updates, resize, input recording) can be asserted
//     deterministically and fast;
//   * produce protocol violations on purpose, which a conformant server never produces and which is
//     the only way to test the client's parser bounds.
//
// It is never product code, it never replaces the HyRemote runtime fixture, and the runtime fixture
// - not this double - is what proves interoperability with HyRemote.

#include "harness/deterministic_content.hpp"
#include "net/socket.hpp"
#include "rfb/client.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace poc::core_remote::harness {

enum class ScriptedBehaviour {
    // Conformant exchanges.
    None38,        // RFB 003.008, one security type (None or VNC auth per config)
    VncAuth38,     // RFB 003.008, security type 2
    ThreeSevenFraming38,  // server says 3.8 but the client offered 3.7 (what HyRemote actually does)
    StrictThreeSeven,     // a genuinely 3.7 server: 4-byte security type
    // Deliberate violations, one per negative requirement.
    BadProtocolVersion,
    ZeroSecurityTypes,
    MalformedSecurityOfferCount,
    ZeroSizedServerInit,
    AbsurdFramebufferSize,
    RectangleCountOverflow,
    UnsupportedEncoding,
    OversizedRectanglePayload,
    TruncatedRectangle,
    HeaderTruncatedMidMessage,
    UnsupportedServerMessage,
    ServerCutTextPresent,
};

struct ScriptedServerConfig
{
    ScriptedBehaviour behaviour = ScriptedBehaviour::None38;

    std::uint8_t securityType = rfb::kSecurityTypeNone;
    std::vector<std::uint8_t> password;

    // The deterministic steps served, in order, one per FramebufferUpdateRequest.
    std::vector<int> steps{0};

    std::uint16_t serverInitWidth = kInitialWidth;
    std::uint16_t serverInitHeight = kInitialHeight;

    // Violation payloads.
    std::uint16_t rectangleCount = 4096;

    std::uint32_t colourMapEntryCount = 0;
};

struct ReceivedMessage
{
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t keysym = 0;
    rfb::PixelFormat pixelFormat;
    std::vector<std::int32_t> encodings;
    std::string cutText;
    std::vector<std::uint8_t> raw;
};

struct SessionRecord
{
    std::string clientVersion;
    std::uint8_t selectedSecurityType = 0;
    bool vncChallengeSent = false;
    bool vncResponseHadExpectedLength = false;
    bool vncResponseMatchedLocalComputation = false;
    bool clientInitShared = false;
    std::uint16_t updatesServed = 0;
    std::vector<ReceivedMessage> messages;
};

class ScriptedServer
{
public:
    static std::unique_ptr<ScriptedServer> start(const ScriptedServerConfig &config, std::string &error);

    ~ScriptedServer();
    ScriptedServer(const ScriptedServer &) = delete;
    ScriptedServer &operator=(const ScriptedServer &) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::size_t sessionCount() const;
    [[nodiscard]] std::vector<SessionRecord> sessions() const;
    [[nodiscard]] SessionRecord lastSession() const;
    // Waits until at least `count` sessions have completed (or the timeout expires).
    [[nodiscard]] bool waitForSessions(std::size_t count, int timeoutMs) const;

    void stop() noexcept;

private:
    ScriptedServer() = default;
    void run() noexcept;
    void serveOne(net::Socket &client) noexcept;

    ScriptedServerConfig m_config;
    net::Listener m_listener;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::vector<SessionRecord> m_sessions;
};

}  // namespace poc::core_remote::harness
