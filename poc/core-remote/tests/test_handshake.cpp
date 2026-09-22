// POC_ONLY: handshake framing and security negotiation against the scripted double.
//
// The 3.7 cases are the interesting ones. HyRemote accepts a "RFB 003.007" client version but always
// answers with the RFB 3.8 security framing, so a strictly-3.7 peer (which reads a 4-byte security
// type) cannot finish. These tests pin both halves of that: the strict path fails against a
// 3.8-framed offer, and the lenient path succeeds. The real server is exercised separately by the
// runtime fixture.
#include "harness/scripted_server.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using namespace poc::core_remote;

rfb::ClientConfig clientConfigFor(const harness::ScriptedServer &server)
{
    rfb::ClientConfig config;
    config.port = server.port();
    config.ioTimeoutMs = 4000;
    config.connectTimeoutMs = 4000;
    return config;
}

bool containsEncoding(const std::vector<std::int32_t> &encodings, std::int32_t wanted)
{
    return std::find(encodings.begin(), encodings.end(), wanted) != encodings.end();
}

std::vector<std::uint8_t> bytesOf(const std::string &text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

constexpr const char *kPassword = "s3cret!";
constexpr const char *kWrongPassword = "wr0ng!!!";

std::unique_ptr<harness::ScriptedServer> startServer(harness::ScriptedServerConfig config)
{
    std::string error;
    auto server = harness::ScriptedServer::start(config, error);
    if (!server)
        std::cerr << "scripted server did not start: " << error << "\n";
    return server;
}

void test38SecurityNone()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::Client client(clientConfigFor(*server));
    const rfb::Error connectError = client.connect();
    if (!connectError.ok()) {
        std::cerr << "connect failed: " << rfb::toString(connectError.code) << " " << connectError.detail
                  << "\n";
    }
    CHECK(connectError.ok());
    CHECK(client.connected());
    CHECK_EQ(client.serverInfo().versionString, std::string("RFB 003.008\n"));
    CHECK_EQ(client.serverInfo().negotiatedMinor, 8);
    CHECK_EQ(client.serverInfo().securityTypeUsed, rfb::kSecurityTypeNone);
    CHECK(client.serverInfo().securityOfferWas38Form);
    CHECK(client.securityResultObserved());
    CHECK_EQ(client.serverInfo().serverName, std::string("POC-Scripted-Server"));
    CHECK_EQ(client.framebuffer().width(), harness::kInitialWidth);
    CHECK_EQ(client.framebuffer().height(), harness::kInitialHeight);
    CHECK_EQ(client.serverInfo().framebufferWidth, harness::kInitialWidth);

    // Nothing the client did not ask for may arrive on its own.
    CHECK(client.bellCount() == 0);
    CHECK(client.serverCutTexts().empty());

    client.disconnect();
    CHECK(!client.connected());

    CHECK(server->waitForSessions(1, 4000));
    const harness::SessionRecord session = server->lastSession();
    CHECK_EQ(session.clientVersion, std::string("RFB 003.008\n"));
    CHECK_EQ(session.selectedSecurityType, rfb::kSecurityTypeNone);
    CHECK(session.clientInitShared);
    CHECK(!session.messages.empty());
    if (!session.messages.empty()) {
        CHECK_EQ(session.messages[0].type, rfb::kClientMessageSetEncodings);
        CHECK(containsEncoding(session.messages[0].encodings, rfb::kEncodingRaw));
        CHECK(containsEncoding(session.messages[0].encodings, rfb::kEncodingDesktopSize));
    }
    server->stop();
}

void test38VncAuthenticationSucceedsAndFails()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::VncAuth38;
    serverConfig.securityType = rfb::kSecurityTypeVncAuth;
    serverConfig.password = bytesOf(kPassword);
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config = clientConfigFor(*server);
    config.password = bytesOf(kPassword);

    {
        rfb::Client client(config);
        CHECK(client.connect().ok());
        CHECK_EQ(client.serverInfo().securityTypeUsed, rfb::kSecurityTypeVncAuth);
        CHECK(client.securityResultObserved());
        CHECK_EQ(client.framebuffer().width(), harness::kInitialWidth);
        client.disconnect();
    }

    CHECK(server->waitForSessions(1, 4000));
    const harness::SessionRecord accepted = server->lastSession();
    CHECK(accepted.vncChallengeSent);
    CHECK(accepted.vncResponseHadExpectedLength);
    CHECK(accepted.vncResponseMatchedLocalComputation);
    CHECK_EQ(accepted.selectedSecurityType, rfb::kSecurityTypeVncAuth);

    // A wrong credential must be refused, and refused as an authentication failure rather than as a
    // generic protocol error.
    {
        rfb::ClientConfig wrong = config;
        wrong.password = bytesOf(kWrongPassword);
        rfb::Client client(wrong);
        const rfb::Error error = client.connect();
        CHECK(!error.ok());
        CHECK_EQ(error.code, rfb::ErrorCode::AuthenticationRejected);
        CHECK(!client.connected());
    }
    CHECK(server->waitForSessions(2, 4000));
    CHECK(!server->lastSession().vncResponseMatchedLocalComputation);

    server->stop();
}

void testSecurityTypeNotOffered()
{
    // The server offers VNC authentication only; a client that insists on None must be refused
    // rather than quietly downgraded.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::VncAuth38;
    serverConfig.securityType = rfb::kSecurityTypeVncAuth;
    serverConfig.password = bytesOf(kPassword);
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config = clientConfigFor(*server);
    config.securityPreference = rfb::SecurityPreference::None;
    rfb::Client client(config);
    const rfb::Error error = client.connect();
    CHECK(!error.ok());
    CHECK_EQ(error.code, rfb::ErrorCode::SecurityTypeNotOffered);
    server->stop();
}

void testThreeSevenLenientSucceedsWhereStrictFails()
{
    // What HyRemote actually does: it accepts "RFB 003.007" and then frames the security offer in the
    // 3.8 form.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::ThreeSevenFraming38;
    serverConfig.securityType = rfb::kSecurityTypeNone;

    {
        auto server = startServer(serverConfig);
        CHECK(server != nullptr);
        if (server) {
            rfb::ClientConfig config = clientConfigFor(*server);
            config.versionMode = rfb::VersionMode::Strict37;
            rfb::Client client(config);
            const rfb::Error error = client.connect();
            CHECK(!error.ok());
            CHECK_EQ(error.code, rfb::ErrorCode::IoTimeout);
            CHECK(!client.connected());
            server->stop();
        }
    }

    {
        auto server = startServer(serverConfig);
        CHECK(server != nullptr);
        if (server) {
            rfb::ClientConfig config = clientConfigFor(*server);
            config.versionMode = rfb::VersionMode::Lenient37;
            rfb::Client client(config);
            CHECK(client.connect().ok());
            CHECK_EQ(client.serverInfo().negotiatedMinor, 7);
            CHECK_EQ(client.serverInfo().securityTypeUsed, rfb::kSecurityTypeNone);
            CHECK(client.serverInfo().securityOfferWas38Form);
            CHECK_EQ(client.framebuffer().width(), harness::kInitialWidth);
            client.disconnect();

            CHECK(server->waitForSessions(1, 4000));
            CHECK_EQ(server->lastSession().clientVersion, std::string("RFB 003.007\n"));
            server->stop();
        }
    }
}

void testStrictThreeSevenAgainstARealThreeSevenServer()
{
    // A genuinely 3.7 server: a 4-byte security type, and no SecurityResult for SecurityType None.
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::StrictThreeSeven;
    serverConfig.securityType = rfb::kSecurityTypeNone;
    auto server = startServer(serverConfig);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config = clientConfigFor(*server);
    config.versionMode = rfb::VersionMode::Strict37;
    rfb::Client client(config);
    CHECK(client.connect().ok());
    CHECK_EQ(client.serverInfo().negotiatedMinor, 7);
    CHECK(!client.serverInfo().securityOfferWas38Form);
    CHECK(!client.securityResultObserved());  // 3.7 sends none for SecurityType None
    CHECK_EQ(client.framebuffer().width(), harness::kInitialWidth);
    client.disconnect();
    server->stop();
}

void testServerInitVariantsAreRejected()
{
    {
        harness::ScriptedServerConfig serverConfig;
        serverConfig.behaviour = harness::ScriptedBehaviour::ZeroSizedServerInit;
        auto server = startServer(serverConfig);
        CHECK(server != nullptr);
        if (server) {
            rfb::Client client(clientConfigFor(*server));
            const rfb::Error error = client.connect();
            CHECK(!error.ok());
            CHECK_EQ(error.code, rfb::ErrorCode::ServerInitMalformed);
            server->stop();
        }
    }

    {
        harness::ScriptedServerConfig serverConfig;
        serverConfig.behaviour = harness::ScriptedBehaviour::AbsurdFramebufferSize;
        auto server = startServer(serverConfig);
        CHECK(server != nullptr);
        if (server) {
            rfb::Client client(clientConfigFor(*server));
            const rfb::Error error = client.connect();
            CHECK(!error.ok());
            CHECK_EQ(error.code, rfb::ErrorCode::FramebufferSizeRejected);
            server->stop();
        }
    }

    {
        // A name longer than the configured bound is refused before anything is allocated for it.
        harness::ScriptedServerConfig serverConfig;
        serverConfig.behaviour = harness::ScriptedBehaviour::None38;
        auto server = startServer(serverConfig);
        CHECK(server != nullptr);
        if (server) {
            rfb::ClientConfig config = clientConfigFor(*server);
            config.limits.maxServerNameBytes = 4;  // "POC-Scripted-Server" is longer
            rfb::Client client(config);
            const rfb::Error error = client.connect();
            CHECK(!error.ok());
            CHECK_EQ(error.code, rfb::ErrorCode::PayloadSizeExceeded);
            server->stop();
        }
    }
}

}  // namespace

int main()
{
    test38SecurityNone();
    test38VncAuthenticationSucceedsAndFails();
    testSecurityTypeNotOffered();
    testThreeSevenLenientSucceedsWhereStrictFails();
    testStrictThreeSevenAgainstARealThreeSevenServer();
    testServerInitVariantsAreRejected();

    return poc_test::finish("RFB handshake, security negotiation and ServerInit bounds");
}
