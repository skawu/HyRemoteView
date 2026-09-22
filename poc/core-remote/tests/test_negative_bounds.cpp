// POC_ONLY: negative and bounds behaviour.
//
// Requirements checked here, one per case: a deterministic error, no crash, no unbounded allocation
// and no deadlock. The "no unbounded allocation" requirement is checked concretely rather than
// asserted in prose: for the oversized-payload cases the client must fail *before* reading the
// payload, so its total byte count stays small even though the rectangle claims gigabytes.
#include "harness/scripted_server.hpp"
#include "test_support.hpp"

#include <string>

namespace {

using namespace poc::core_remote;

std::unique_ptr<harness::ScriptedServer> startServer(harness::ScriptedServerConfig config)
{
    std::string error;
    auto server = harness::ScriptedServer::start(config, error);
    if (!server)
        std::cerr << "scripted server did not start: " << error << "\n";
    return server;
}

rfb::ClientConfig configFor(const harness::ScriptedServer &server)
{
    rfb::ClientConfig config;
    config.port = server.port();
    config.ioTimeoutMs = 3000;
    config.connectTimeoutMs = 3000;
    return config;
}

// The handshake must fail with the expected code, and the client must not be left "connected".
void expectHandshakeFailure(harness::ScriptedBehaviour behaviour,
                            rfb::ErrorCode expected,
                            const char *name)
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = behaviour;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::Client client(configFor(*server));
    const rfb::Error error = client.connect();
    if (error.ok()) {
        std::cerr << name << ": expected failure " << rfb::toString(expected) << ", but the handshake succeeded\n";
        poc_test::report(__FILE__, __LINE__, name);
    } else if (error.code != expected) {
        std::cerr << name << ": expected " << rfb::toString(expected) << ", got "
                  << rfb::toString(error.code) << " (" << error.detail << ")\n";
        poc_test::report(__FILE__, __LINE__, name);
    }
    CHECK(!client.connected());
    server->stop();
}

void expectUpdateFailure(harness::ScriptedBehaviour behaviour,
                         rfb::ErrorCode expected,
                         const char *name)
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = behaviour;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::Client client(configFor(*server));
    CHECK(client.connect().ok());
    CHECK(client.requestFullUpdate().ok());

    rfb::UpdateInfo update;
    const rfb::Error error = client.waitForFramebufferUpdate(update);
    if (error.ok() || error.code != expected) {
        std::cerr << name << ": expected " << rfb::toString(expected) << ", got "
                  << rfb::toString(error.code) << " (" << error.detail << ")\n";
        poc_test::report(__FILE__, __LINE__, name);
    }

    // Nothing above a few kilobytes may have been read for a payload the client refused.
    const std::uint64_t read = client.bytesRead();
    if (read > 64U * 1024U) {
        std::cerr << name << ": client read " << read << " bytes before refusing the payload\n";
        poc_test::report(__FILE__, __LINE__, name);
    }

    client.disconnect();
    server->stop();
}

void testBadProtocolVersion()
{
    expectHandshakeFailure(harness::ScriptedBehaviour::BadProtocolVersion,
                           rfb::ErrorCode::ProtocolVersionUnsupported, "bad-protocol-version");
}

void testZeroSecurityTypes()
{
    expectHandshakeFailure(harness::ScriptedBehaviour::ZeroSecurityTypes,
                           rfb::ErrorCode::SecurityTypeNotOffered, "zero-security-types");
}

void testMalformedSecurityOfferCount()
{
    expectHandshakeFailure(harness::ScriptedBehaviour::MalformedSecurityOfferCount,
                           rfb::ErrorCode::SecurityOfferMalformed, "malformed-security-offer-count");
}

void testMalformedServerInit()
{
    expectHandshakeFailure(harness::ScriptedBehaviour::ZeroSizedServerInit,
                           rfb::ErrorCode::ServerInitMalformed, "malformed-server-init");
}

void testAbsurdFramebufferDimensions()
{
    expectHandshakeFailure(harness::ScriptedBehaviour::AbsurdFramebufferSize,
                           rfb::ErrorCode::FramebufferSizeRejected, "absurd-framebuffer-dimensions");
}

void testRectangleCountOverflow()
{
    expectUpdateFailure(harness::ScriptedBehaviour::RectangleCountOverflow,
                        rfb::ErrorCode::RectangleCountExceeded, "rectangle-count-oversize");
}

void testUnsupportedEncoding()
{
    expectUpdateFailure(harness::ScriptedBehaviour::UnsupportedEncoding,
                        rfb::ErrorCode::UnsupportedEncoding, "unsupported-encoding");
}

void testPixelByteMultiplicationOverflowCandidate()
{
    // A rectangle of 65535x65535 pixels at 4 bytes per pixel is ~17 GiB. The product itself still
    // fits a 64-bit integer, so the arithmetic cannot wrap - it is the byte bound that has to refuse
    // it. The assertion is that the refusal is deterministic and happens before any read.
    expectUpdateFailure(harness::ScriptedBehaviour::OversizedRectanglePayload,
                        rfb::ErrorCode::PayloadSizeExceeded, "pixel-byte-overflow-candidate");
}

void testTruncatedRectangle()
{
    expectUpdateFailure(harness::ScriptedBehaviour::TruncatedRectangle,
                        rfb::ErrorCode::ConnectionClosed, "truncated-rectangle");
}

void testDisconnectMidMessage()
{
    expectUpdateFailure(harness::ScriptedBehaviour::HeaderTruncatedMidMessage,
                        rfb::ErrorCode::ConnectionClosed, "disconnect-mid-message");
}

void testUnsupportedServerMessage()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::UnsupportedServerMessage;
    serverConfig.colourMapEntryCount = 512;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::Client client(configFor(*server));
    CHECK(client.connect().ok());

    rfb::ServerMessage message;
    const rfb::Error error = client.readServerMessage(message, 3000);
    CHECK(!error.ok());
    CHECK_EQ(error.code, rfb::ErrorCode::UnsupportedServerMessage);
    client.disconnect();
    server->stop();
}

void testReceiveBufferCap()
{
    // The server is conformant; the client's own pending-receive bound is what has to trigger. A full
    // 96x64 frame is 24576 bytes, well above the 1024-byte bound configured here.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    serverConfig.steps = {0};
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config = configFor(*server);
    config.limits.maxPendingReceiveBytes = 1024;
    rfb::Client client(config);
    CHECK(client.connect().ok());
    CHECK(client.requestFullUpdate().ok());

    rfb::UpdateInfo update;
    const rfb::Error error = client.waitForFramebufferUpdate(update);
    CHECK(!error.ok());
    CHECK_EQ(error.code, rfb::ErrorCode::ReceiveLimitExceeded);
    CHECK(client.bytesRead() < 64U * 1024U);
    client.disconnect();
    server->stop();
}

void testFramebufferDimensionCap()
{
    // A frame the client will not accept, driven by the client's own configured bound.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config = configFor(*server);
    config.limits.maxFramebufferWidth = 32;
    config.limits.maxFramebufferHeight = 32;
    rfb::Client client(config);
    const rfb::Error error = client.connect();
    CHECK(!error.ok());
    CHECK_EQ(error.code, rfb::ErrorCode::FramebufferSizeRejected);
    server->stop();
}

void testRectangleCountBoundIsNotReachedWhenLegal()
{
    // Control case: a legal 2-rectangle update must pass, so the bound is not simply always firing.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    serverConfig.steps = {0, 4};
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::Client client(configFor(*server));
    CHECK(client.connect().ok());
    CHECK(client.requestFullUpdate().ok());
    rfb::UpdateInfo update;
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK(client.requestFramebufferUpdate(false, 0, 0, harness::kInitialWidth, harness::kInitialHeight).ok());
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK_EQ(update.rectangles.size(), std::size_t{2});
    client.disconnect();
    server->stop();
}

}  // namespace

int main()
{
    testBadProtocolVersion();
    testZeroSecurityTypes();
    testMalformedSecurityOfferCount();
    testMalformedServerInit();
    testAbsurdFramebufferDimensions();
    testRectangleCountOverflow();
    testUnsupportedEncoding();
    testPixelByteMultiplicationOverflowCandidate();
    testTruncatedRectangle();
    testDisconnectMidMessage();
    testUnsupportedServerMessage();
    testReceiveBufferCap();
    testFramebufferDimensionCap();
    testRectangleCountBoundIsNotReachedWhenLegal();

    return poc_test::finish("negative and bounds behaviour (deterministic errors, no unbounded reads)");
}
