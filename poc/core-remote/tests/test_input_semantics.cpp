// POC_ONLY: input correctness at the wire level.
//
// The client's input has to be checked as *protocol messages*, not as "some bytes were sent": the
// assertions below reconstruct the exact sequence the wire contract requires from the shared input
// script and compare it field by field. The semantic end-result (what the remote application
// received) is asserted against the HyRemote runtime fixture, which normalises these messages.
#include "harness/scripted_server.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

using namespace poc::core_remote;

struct WireInput
{
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint32_t keysym = 0;
};

// Expands the shared script into the message sequence RFB requires.
std::vector<WireInput> expectedWireSequence()
{
    std::vector<WireInput> expected;
    for (const harness::InputAction &action : harness::inputScript()) {
        WireInput pointer;
        pointer.type = rfb::kClientMessagePointerEvent;
        pointer.x = action.x;
        pointer.y = action.y;

        switch (action.kind) {
        case harness::InputKind::PointerMove:
            pointer.flags = 0;
            expected.push_back(pointer);
            break;
        case harness::InputKind::ButtonPress:
            pointer.flags = action.button;
            expected.push_back(pointer);
            break;
        case harness::InputKind::ButtonRelease:
            pointer.flags = 0;
            expected.push_back(pointer);
            break;
        case harness::InputKind::Wheel:
            // A wheel notch is a press/release pair on the wheel mask bit.
            pointer.flags = action.button;
            expected.push_back(pointer);
            pointer.flags = 0;
            expected.push_back(pointer);
            break;
        case harness::InputKind::KeyPress:
        case harness::InputKind::KeyRelease: {
            WireInput key;
            key.type = rfb::kClientMessageKeyEvent;
            key.flags = action.kind == harness::InputKind::KeyPress ? 1U : 0U;
            key.keysym = action.keysym;
            expected.push_back(key);
            break;
        }
        }
    }
    return expected;
}

void testInputScriptProducesTheRequiredMessages()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    std::string startError;
    auto server = harness::ScriptedServer::start(serverConfig, startError);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);
    CHECK(client.connect().ok());

    // Execute the shared script through the client's public API.
    for (const harness::InputAction &action : harness::inputScript()) {
        rfb::Error error;
        switch (action.kind) {
        case harness::InputKind::PointerMove:
            error = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::ButtonPress:
            error = client.sendPointerEvent(action.button, action.x, action.y);
            break;
        case harness::InputKind::ButtonRelease:
            error = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::Wheel:
            error = client.sendPointerEvent(action.button, action.x, action.y);
            if (error.ok())
                error = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::KeyPress:
            error = client.sendKeyEvent(action.keysym, true);
            break;
        case harness::InputKind::KeyRelease:
            error = client.sendKeyEvent(action.keysym, false);
            break;
        }
        CHECK(error.ok());
    }

    // ClientCutText is exercised because the client must be able to produce a well-formed message the
    // server only consumes. It is not used to claim clipboard support.
    CHECK(client.sendClientCutText("poc-clipboard-probe").ok());

    client.disconnect();
    CHECK(server->waitForSessions(1, 5000));
    const harness::SessionRecord session = server->lastSession();
    server->stop();

    CHECK(!session.messages.empty());
    if (session.messages.empty())
        return;
    CHECK_EQ(session.messages[0].type, rfb::kClientMessageSetEncodings);

    const std::vector<WireInput> expected = expectedWireSequence();
    // messages: the SetEncodings message, then the script, then one ClientCutText.
    CHECK_EQ(session.messages.size(), expected.size() + 2);

    for (std::size_t index = 0; index < expected.size() && index + 1 < session.messages.size(); ++index) {
        const harness::ReceivedMessage &actual = session.messages[index + 1];
        CHECK_EQ(int(actual.type), int(expected[index].type));
        CHECK_EQ(int(actual.flags), int(expected[index].flags));
        if (actual.type == rfb::kClientMessagePointerEvent) {
            CHECK_EQ(actual.x, expected[index].x);
            CHECK_EQ(actual.y, expected[index].y);
        } else {
            CHECK_EQ(actual.keysym, expected[index].keysym);
        }
    }

    const harness::ReceivedMessage &cut = session.messages.back();
    CHECK_EQ(int(cut.type), int(rfb::kClientMessageClientCutText));
    CHECK_EQ(cut.cutText, std::string("poc-clipboard-probe"));
}

void testWheelIsAPressReleasePair()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    std::string startError;
    auto server = harness::ScriptedServer::start(serverConfig, startError);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);
    CHECK(client.connect().ok());

    CHECK(client.sendWheel(1, 1, 7, 9).ok());  // one notch up, one notch right
    client.disconnect();
    CHECK(server->waitForSessions(1, 5000));
    const harness::SessionRecord session = server->lastSession();
    server->stop();

    // SetEncodings + (up press, up release, right press, right release)
    CHECK_EQ(session.messages.size(), std::size_t{5});
    if (session.messages.size() < 5)
        return;
    CHECK_EQ(int(session.messages[1].flags), int(rfb::kWheelUp));
    CHECK_EQ(int(session.messages[2].flags), 0);
    CHECK_EQ(int(session.messages[3].flags), int(rfb::kWheelRight));
    CHECK_EQ(int(session.messages[4].flags), 0);
    for (std::size_t index = 1; index < session.messages.size(); ++index) {
        CHECK_EQ(session.messages[index].x, std::uint16_t{7});
        CHECK_EQ(session.messages[index].y, std::uint16_t{9});
    }
}

void testKeyEventShape()
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    std::string startError;
    auto server = harness::ScriptedServer::start(serverConfig, startError);
    CHECK(server != nullptr);
    if (!server)
        return;

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    rfb::Client client(config);
    CHECK(client.connect().ok());

    // The keysym the client puts on the wire must be the X11 keysym, not a local key code: the
    // server, not the client, decides what 'A' means.
    CHECK(client.sendKeyEvent(rfb::keysymForAscii('A'), true).ok());
    CHECK(client.sendKeyEvent(rfb::keysymForAscii('A'), false).ok());
    CHECK(client.sendKeyEvent(rfb::kKeysymShiftLeft, true).ok());
    CHECK(client.sendKeyEvent(rfb::kKeysymShiftLeft, false).ok());
    CHECK(client.sendKeyEvent(rfb::kKeysymControlLeft, true).ok());
    CHECK(client.sendKeyEvent(rfb::kKeysymAltLeft, true).ok());

    client.disconnect();
    CHECK(server->waitForSessions(1, 5000));
    const harness::SessionRecord session = server->lastSession();
    server->stop();

    CHECK_EQ(session.messages.size(), std::size_t{7});
    if (session.messages.size() < 7)
        return;
    CHECK_EQ(int(session.messages[1].keysym), int('A'));
    CHECK_EQ(int(session.messages[1].flags), 1);
    CHECK_EQ(int(session.messages[2].flags), 0);
    CHECK_EQ(int(session.messages[3].keysym), int(rfb::kKeysymShiftLeft));
    CHECK_EQ(int(session.messages[4].keysym), int(rfb::kKeysymShiftLeft));
    CHECK_EQ(int(session.messages[5].keysym), int(rfb::kKeysymControlLeft));
    CHECK_EQ(int(session.messages[6].keysym), int(rfb::kKeysymAltLeft));
}

}  // namespace

int main()
{
    testInputScriptProducesTheRequiredMessages();
    testWheelIsAPressReleasePair();
    testKeyEventShape();

    return poc_test::finish("input correctness on the wire (pointer, buttons, wheel, keys, modifiers)");
}
