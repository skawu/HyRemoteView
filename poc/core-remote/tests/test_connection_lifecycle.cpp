// POC_ONLY: connection lifecycle - clean disconnect, reconnect, and what the server does and does
// not send outside the framebuffer stream.
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

void testCleanDisconnectAndReconnect()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    serverConfig.steps = {0};
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);

    CHECK(client.connect().ok());
    CHECK(client.requestFullUpdate().ok());
    rfb::UpdateInfo update;
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK_EQ(client.framebuffer().hash(), harness::framebufferForFormat(0, rfb::PixelFormat::hyremoteNative()).hash());

    // A clean disconnect is the client closing its own side; the server keeps running.
    client.disconnect();
    CHECK(!client.connected());

    // Reconnect reuses the same object and must renegotiate from scratch.
    CHECK(client.connect().ok());
    CHECK(client.connected());
    CHECK_EQ(client.framebuffer().width(), harness::kInitialWidth);
    CHECK_EQ(client.framebuffer().height(), harness::kInitialHeight);
    {
        // The surface is exactly a reset one: nothing has been drawn on this session yet.
        rfb::Framebuffer fresh;
        fresh.reset(harness::kInitialWidth, harness::kInitialHeight, 0);
        CHECK_EQ(client.framebuffer().hash(), fresh.hash());
    }
    CHECK(client.requestFullUpdate().ok());
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK_EQ(client.framebuffer().hash(), harness::framebufferForFormat(0, rfb::PixelFormat::hyremoteNative()).hash());

    client.disconnect();
    CHECK(server->waitForSessions(2, 5000));
    const std::vector<harness::SessionRecord> sessions = server->sessions();
    CHECK_EQ(sessions.size(), std::size_t{2});
    for (const harness::SessionRecord &session : sessions) {
        CHECK_EQ(session.clientVersion, std::string("RFB 003.008\n"));
        CHECK(session.clientInitShared);
        CHECK_EQ(session.updatesServed, std::size_t{1});
    }
    server->stop();
}

void testServerCutTextIsConsumedAndRecorded()
{
    // The client must be able to consume a ServerCutText if one ever arrives. This does not claim the
    // current HyRemote server provides clipboard content - the runtime fixture asserts the opposite.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::ServerCutTextPresent;
    serverConfig.steps = {0};
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);
    CHECK(client.connect().ok());

    rfb::ServerMessage message;
    CHECK(client.readServerMessage(message, 3000).ok());
    CHECK_EQ(int(message.kind), int(rfb::ServerMessageKind::ServerCutText));
    CHECK_EQ(message.cutText, std::string("poc-clipboard-probe"));
    CHECK_EQ(client.serverCutTexts().size(), std::size_t{1});

    // The framebuffer stream must be unaffected by the interleaved message.
    CHECK(client.requestFullUpdate().ok());
    rfb::UpdateInfo update;
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK_EQ(client.framebuffer().hash(), harness::framebufferForFormat(0, rfb::PixelFormat::hyremoteNative()).hash());

    client.disconnect();
    server->stop();
}

void testServerSendsNothingUnsolicited()
{
    // Evidence for the clipboard item: over a conformant session the server publishes no clipboard
    // content and no bell at all, so "clipboard" cannot be claimed from this transport.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    serverConfig.steps = {0, 1};
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);
    CHECK(client.connect().ok());
    CHECK(client.requestFullUpdate().ok());
    rfb::UpdateInfo update;
    CHECK(client.waitForFramebufferUpdate(update).ok());
    CHECK(client.requestFramebufferUpdate(true, 4, 34, 24, 8).ok());
    CHECK(client.waitForFramebufferUpdate(update).ok());

    CHECK(client.serverCutTexts().empty());
    CHECK(client.bellCount() == 0);

    client.disconnect();
    server->stop();
}

void testOperationsBeforeConnectAreRefused()
{
    rfb::ClientConfig config;
    config.port = 1;  // nothing is listening; no connection is attempted here
    rfb::Client client(config);
    CHECK(!client.connected());

    const rfb::Error update = client.requestFramebufferUpdate(false, 0, 0, 1, 1);
    CHECK(!update.ok());
    CHECK_EQ(update.code, rfb::ErrorCode::NotConnected);

    const rfb::Error key = client.sendKeyEvent(rfb::keysymForAscii('A'), true);
    CHECK(!key.ok());
    CHECK_EQ(key.code, rfb::ErrorCode::NotConnected);

    rfb::ServerMessage message;
    CHECK(!client.readServerMessage(message, 10).ok());
}

}  // namespace

int main()
{
    testCleanDisconnectAndReconnect();
    testServerCutTextIsConsumedAndRecorded();
    testServerSendsNothingUnsolicited();
    testOperationsBeforeConnectAreRefused();

    return poc_test::finish("connection lifecycle (clean disconnect, reconnect, unsolicited messages)");
}
