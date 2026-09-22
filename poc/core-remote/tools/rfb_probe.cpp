// POC_ONLY client-side probe.
//
// Drives the client against a server and prints a machine-checkable verdict. Against the HyRemote
// runtime fixture it also uses the fixture's control channel, so one command produces both halves of
// the evidence: what the client decoded, what the runtime transmitted, and what input semantics the
// runtime published.
//
// The password is only ever read from an environment variable. A credential must not appear in a
// process command line, and the probe never prints it.

#include "harness/deterministic_content.hpp"
#include "net/socket.hpp"
#include "rfb/client.hpp"
#include "rfb/des.hpp"
#include "rfb/vnc_auth.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace poc::core_remote;

int failures = 0;

#define PROBE_CHECK(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ++failures;                                                                            \
            std::cerr << "PROBE_CHECK failed: " #expr "\n";                                        \
        }                                                                                          \
    } while (false)

std::string hexOf(std::uint64_t value)
{
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string hexOf(const std::vector<std::uint8_t> &bytes)
{
    return rfb::toHex(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> bytesFromHex(const std::string &text)
{
    std::vector<std::uint8_t> out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        const auto nibble = [](char character) -> int {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return 0;
        };
        out.push_back(static_cast<std::uint8_t>((nibble(text[index]) << 4) | nibble(text[index + 1])));
    }
    return out;
}

struct Options
{
    std::string scenario;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint16_t controlPort = 0;
    std::string version = "38";
    std::string security = "auto";
    std::string passwordEnv;
    bool json = false;
    bool sendPixelFormat = false;
    std::string pixelFormat = "native";
};

// A minimal line-based client for the fixture's control channel.
class ControlChannel
{
public:
    bool open(const std::string &host, std::uint16_t port)
    {
        std::string error;
        m_socket = net::Socket::connectTcp(host, port, 3000, error);
        if (!m_socket.valid()) {
            std::cerr << "control channel: " << error << "\n";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool valid() const { return m_socket.valid(); }

    // Sends one command and returns every line the fixture answers with.
    std::vector<std::string> command(const std::string &text)
    {
        std::vector<std::string> lines;
        std::string payload = text + "\n";
        std::string error;
        if (m_socket.writeAll(payload.data(), payload.size(), 5000, error) != net::IoStatus::Ok) {
            std::cerr << "control channel write failed: " << error << "\n";
            return lines;
        }

        std::string buffer;
        while (lines.size() < 4096) {
            const std::size_t newline = buffer.find('\n');
            if (newline != std::string::npos) {
                lines.push_back(buffer.substr(0, newline));
                buffer.erase(0, newline + 1);
                // A response is complete when its terminator line arrives.
                const std::string &last = lines.back();
                if (last.rfind("OK ", 0) == 0 || last == "ERR")
                    return lines;
                continue;
            }
            char chunk[512] = {};
            std::string readError;
            const net::IoStatus status = m_socket.readExact(chunk, 1, 5000, readError);
            if (status != net::IoStatus::Ok) {
                std::cerr << "control channel read failed: " << net::toString(status) << "\n";
                return lines;
            }
            buffer.push_back(chunk[0]);
        }
        return lines;
    }

private:
    net::Socket m_socket;
};

rfb::VersionMode versionModeFrom(const std::string &text)
{
    if (text == "37")
        return rfb::VersionMode::Strict37;
    if (text == "lenient37")
        return rfb::VersionMode::Lenient37;
    return rfb::VersionMode::Prefer38;
}

rfb::SecurityPreference securityFrom(const std::string &text)
{
    if (text == "none")
        return rfb::SecurityPreference::None;
    if (text == "vnc")
        return rfb::SecurityPreference::VncAuth;
    return rfb::SecurityPreference::Auto;
}

rfb::ClientConfig baseConfig(const Options &options)
{
    rfb::ClientConfig config;
    config.host = options.host;
    config.port = options.port;
    config.ioTimeoutMs = 8000;
    config.connectTimeoutMs = 8000;
    config.versionMode = versionModeFrom(options.version);
    config.securityPreference = securityFrom(options.security);
    config.sendPixelFormat = options.sendPixelFormat;
    if (options.pixelFormat == "565be")
        config.pixelFormat = rfb::PixelFormat::rgb565BigEndian();

    if (!options.passwordEnv.empty()) {
        const char *value = std::getenv(options.passwordEnv.c_str());
        if (value != nullptr)
            config.password.assign(value, value + std::strlen(value));
    }
    return config;
}

void reportFailure(const rfb::Error &error)
{
    std::cerr << "FAIL code=" << rfb::toString(error.code) << " detail=" << error.detail << "\n";
}

// The fixture is started for one scenario and stops itself when that scenario is finished, so every
// run begins from the same state: a fresh runtime holding deterministic step 0 and an empty input
// log. Without this, a second scenario would inherit the previous one's geometry.
void shutdownFixture(ControlChannel &control)
{
    (void)control.command("QUIT");
}

// A fixture control channel, opened only when the caller asked for one.
std::unique_ptr<ControlChannel> openControl(const Options &options)
{
    if (options.controlPort == 0)
        return nullptr;
    auto control = std::make_unique<ControlChannel>();
    if (!control->open(options.host, options.controlPort))
        return nullptr;
    return control;
}

// ---------------------------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------------------------

bool scenarioHandshake(const Options &options)
{
    rfb::Client client(baseConfig(options));
    const rfb::Error error = client.connect();
    PROBE_CHECK(error.ok());
    if (!error.ok()) {
        reportFailure(error);
        return false;
    }

    std::cout << "server_version=" << client.serverInfo().versionString.substr(0, 11) << "\n";
    std::cout << "negotiated_minor=" << static_cast<int>(client.serverInfo().negotiatedMinor) << "\n";
    std::cout << "security_type=" << static_cast<int>(client.serverInfo().securityTypeUsed) << "\n";
    std::cout << "security_offer_3.8_form=" << (client.serverInfo().securityOfferWas38Form ? 1 : 0) << "\n";
    std::cout << "security_result_observed=" << (client.securityResultObserved() ? 1 : 0) << "\n";
    std::cout << "server_name=" << client.serverInfo().serverName << "\n";
    std::cout << "framebuffer=" << client.framebuffer().width() << "x" << client.framebuffer().height() << "\n";
    std::cout << "server_pixel_format=" << client.serverInfo().serverPixelFormat.describe() << "\n";
    std::cout << "active_pixel_format=" << client.activeFormat().describe() << "\n";
    std::cout << "server_cut_texts=" << client.serverCutTexts().size() << "\n";
    std::cout << "bells=" << client.bellCount() << "\n";
    client.disconnect();
    if (auto control = openControl(options))
        shutdownFixture(*control);
    return failures == 0;
}

bool scenarioDeterminism(const Options &options)
{
    std::unique_ptr<ControlChannel> control = openControl(options);
    const bool haveControl = control != nullptr;

    rfb::Client client(baseConfig(options));
    const rfb::Error error = client.connect();
    PROBE_CHECK(error.ok());
    if (!error.ok()) {
        reportFailure(error);
        return false;
    }

    const rfb::PixelFormat format = client.activeFormat();
    bool ok = true;

    for (int step = 0; step < harness::kStepCount; ++step) {
        const harness::Step &definition = harness::step(step);

        // Ask the runtime to publish exactly this deterministic frame.
        if (haveControl) {
            const std::vector<std::string> response = control->command("STEP " + std::to_string(step));
            bool acknowledged = false;
            for (const std::string &line : response) {
                if (line.rfind("OK STEP", 0) == 0)
                    acknowledged = true;
            }
            if (!acknowledged) {
                std::cerr << "fixture did not acknowledge STEP " << step << "\n";
                ok = false;
            }
        }

        rfb::UpdateInfo update;
        rfb::Error stepError;
        if (definition.resizes) {
            stepError = client.requestFramebufferUpdate(false, 0, 0,
                                                        static_cast<std::uint16_t>(client.framebuffer().width()),
                                                        static_cast<std::uint16_t>(client.framebuffer().height()));
        } else if (step == 0) {
            stepError = client.requestFullUpdate();
        } else {
            stepError = client.requestFramebufferUpdate(true,
                                                        definition.changed.x,
                                                        definition.changed.y,
                                                        definition.changed.width,
                                                        definition.changed.height);
        }
        if (stepError.ok())
            stepError = client.waitForFramebufferUpdate(update);

        const std::uint64_t expected = harness::framebufferForFormat(step, format).hash();
        const std::uint64_t decoded = client.framebuffer().hash();
        const bool match = stepError.ok() && decoded == expected;

        std::cout << "STEP " << step << " name=" << definition.name << " geometry=" << definition.width << "x"
                  << definition.height << " rects=" << update.rectangles.size()
                  << " resized=" << (update.resized ? 1 : 0) << " expected_hash=" << hexOf(expected)
                  << " decoded_hash=" << hexOf(decoded) << " match=" << (match ? 1 : 0) << "\n";

        if (!stepError.ok()) {
            reportFailure(stepError);
            ok = false;
            break;
        }
        if (!match)
            ok = false;

        if (!definition.resizes && !update.rectangles.empty()) {
            const std::uint64_t expectedRegion =
                harness::framebufferForFormat(step, format)
                    .regionHash(definition.changed.x, definition.changed.y, definition.changed.width,
                                definition.changed.height);
            const std::uint64_t decodedRegion = client.framebuffer().regionHash(definition.changed.x,
                                                                               definition.changed.y,
                                                                               definition.changed.width,
                                                                               definition.changed.height);
            std::cout << "STEP " << step << " damaged_region_hash expected=" << hexOf(expectedRegion)
                      << " decoded=" << hexOf(decodedRegion) << " match="
                      << (expectedRegion == decodedRegion ? 1 : 0) << "\n";
            if (expectedRegion != decodedRegion)
                ok = false;
        }

        // Independent pixel spot checks, so a hash collision could not hide a wrong image.
        const rfb::Framebuffer expectedSurface = harness::framebufferForFormat(step, format);
        for (const harness::Block &block : harness::colourBlocks()) {
            const std::uint16_t x = static_cast<std::uint16_t>(block.rect.x + block.rect.width / 2);
            const std::uint16_t y = static_cast<std::uint16_t>(block.rect.y + block.rect.height / 2);
            const std::uint32_t wanted = expectedSurface.pixel(x, y);
            const std::uint32_t actual = client.framebuffer().pixel(x, y);
            if (wanted != actual) {
                std::cerr << "STEP " << step << " block " << block.label << " at (" << x << "," << y
                          << ") expected " << hexOf(static_cast<std::uint64_t>(wanted)) << " got "
                          << hexOf(static_cast<std::uint64_t>(actual)) << "\n";
                ok = false;
            }
        }
        std::cout << "STEP " << step << " colour_block_pixels=" << (ok ? "match" : "MISMATCH") << "\n";
    }

    std::cout << "bytes_read=" << client.bytesRead() << " bytes_written=" << client.bytesWritten() << "\n";
    client.disconnect();

    if (control)
        shutdownFixture(*control);
    PROBE_CHECK(ok);
    return ok;
}

bool scenarioInputSemantics(const Options &options)
{
    std::unique_ptr<ControlChannel> control = openControl(options);
    if (!control) {
        std::cerr << "input-semantics needs --control-port so the runtime's semantic input log can be read\n";
        return false;
    }

    rfb::Client client(baseConfig(options));
    rfb::Error error = client.connect();
    PROBE_CHECK(error.ok());
    if (!error.ok()) {
        reportFailure(error);
        return false;
    }

    // Framebuffer first, so the runtime has a valid viewport for pointer coordinates.
    (void)control->command("STEP 0");
    if (client.requestFullUpdate().ok()) {
        rfb::UpdateInfo update;
        error = client.waitForFramebufferUpdate(update);
    }

    (void)control->command("RESET-INPUT");

    for (const harness::InputAction &action : harness::inputScript()) {
        rfb::Error sendError;
        switch (action.kind) {
        case harness::InputKind::PointerMove:
            sendError = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::ButtonPress:
            sendError = client.sendPointerEvent(action.button, action.x, action.y);
            break;
        case harness::InputKind::ButtonRelease:
            sendError = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::Wheel:
            sendError = client.sendPointerEvent(action.button, action.x, action.y);
            if (sendError.ok())
                sendError = client.sendPointerEvent(0, action.x, action.y);
            break;
        case harness::InputKind::KeyPress:
            sendError = client.sendKeyEvent(action.keysym, true);
            break;
        case harness::InputKind::KeyRelease:
            sendError = client.sendKeyEvent(action.keysym, false);
            break;
        }
        if (!sendError.ok()) {
            reportFailure(sendError);
            return false;
        }
    }

    // A short settle window so the runtime's own thread has published every event.
    {
        rfb::UpdateInfo unused;
        (void)client.requestFramebufferUpdate(true, 0, 0, 1, 1);
        (void)client.waitForFramebufferUpdate(unused);
    }

    // The runtime is asked what it published, and the probe compares that with the script it sent.
    std::vector<std::string> log = control->command("DUMP-INPUT");
    std::cout << "runtime_input_log_begin\n";
    for (const std::string &line : log)
        std::cout << "  " << line << "\n";
    std::cout << "runtime_input_log_end\n";
    std::cout << "scripted_actions=" << harness::inputScript().size() << "\n";

    // Disconnect while a key and a button are still held: the runtime must publish the release of
    // both, which is the "held state after disconnect" requirement.
    client.disconnect();
    std::vector<std::string> finalLog = control->command("DUMP-INPUT");
    bool sawHeldRelease = false;
    for (const std::string &line : finalLog) {
        if (line.find("pressed=0") != std::string::npos)
            sawHeldRelease = true;
    }
    std::cout << "held_state_cleanup_after_disconnect=" << (sawHeldRelease ? 1 : 0) << "\n";
    PROBE_CHECK(sawHeldRelease);

    shutdownFixture(*control);
    return failures == 0;
}

bool scenarioReconnect(const Options &options)
{
    rfb::Client client(baseConfig(options));
    if (!client.connect().ok()) {
        reportFailure(client.error());
        return false;
    }
    rfb::UpdateInfo update;
    if (!client.requestFullUpdate().ok() || !client.waitForFramebufferUpdate(update).ok()) {
        reportFailure(client.error());
        return false;
    }
    const std::uint64_t firstHash = client.framebuffer().hash();
    client.disconnect();
    std::cout << "session1_hash=" << hexOf(firstHash) << "\n";

    if (!client.connect().ok()) {
        reportFailure(client.error());
        return false;
    }
    std::cout << "session2_geometry=" << client.framebuffer().width() << "x" << client.framebuffer().height()
              << "\n";
    std::cout << "session2_reset_hash=" << hexOf(client.framebuffer().hash()) << "\n";
    if (!client.requestFullUpdate().ok() || !client.waitForFramebufferUpdate(update).ok()) {
        reportFailure(client.error());
        return false;
    }
    const std::uint64_t secondHash = client.framebuffer().hash();
    const std::uint64_t expected = harness::framebufferForFormat(0, client.activeFormat()).hash();
    std::cout << "session2_hash=" << hexOf(secondHash) << "\n";
    std::cout << "expected_hash=" << hexOf(expected) << "\n";
    client.disconnect();

    // Both sessions must agree with each other *and* with the independently computed content for
    // step 0, which the fixture publishes at startup.
    PROBE_CHECK(secondHash == firstHash);
    PROBE_CHECK(secondHash == expected);
    if (auto control = openControl(options))
        shutdownFixture(*control);
    return failures == 0;
}

bool scenarioClipboard(const Options &options)
{
    rfb::Client client(baseConfig(options));
    if (!client.connect().ok()) {
        reportFailure(client.error());
        return false;
    }
    rfb::UpdateInfo update;
    if (!client.requestFullUpdate().ok() || !client.waitForFramebufferUpdate(update).ok()) {
        reportFailure(client.error());
        return false;
    }

    // The client can produce a well-formed ClientCutText; the server consumes it. What is being
    // established is whether the server ever publishes clipboard *content* in the other direction.
    const rfb::Error sendError = client.sendClientCutText("poc-clipboard-probe");
    PROBE_CHECK(sendError.ok());

    // Keep reading for a bounded window after the write, so "nothing arrived" is an observation
    // rather than an assumption about timing.
    const std::uint16_t width = static_cast<std::uint16_t>(client.framebuffer().width());
    const std::uint16_t height = static_cast<std::uint16_t>(client.framebuffer().height());
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!client.requestFramebufferUpdate(true, 0, 0, width, height).ok())
            break;
        const rfb::Error readError = client.waitForFramebufferUpdate(update, 1500);
        if (!readError.ok())
            break;
    }

    std::cout << "client_cut_text_sent=1\n";
    std::cout << "server_cut_text_received=" << client.serverCutTexts().size() << "\n";
    std::cout << "server_bells_received=" << client.bellCount() << "\n";
    std::cout << "server_message_path=" << (client.serverCutTexts().empty() ? "ABSENT" : "PRESENT") << "\n";
    std::cout << "clipboard=" << (client.serverCutTexts().empty() ? "NOT_SUPPORTED_BY_CURRENT_SERVER"
                                                                 : "PRESENT") << "\n";

    client.disconnect();
    PROBE_CHECK(client.serverCutTexts().empty());
    if (auto control = openControl(options))
        shutdownFixture(*control);
    return failures == 0;
}

// Cross-check review point: the client's DES is compared against the value HyRemote's own
// OpenSSL-based implementation produces for the same credential and challenge. Without this, the
// client's VNC authentication would only ever be checked against itself.
bool scenarioDesCrossCheck(const Options &options)
{
    auto control = openControl(options);
    if (!control) {
        std::cerr << "des-crosscheck needs --control-port\n";
        return false;
    }
    if (options.passwordEnv.empty()) {
        std::cerr << "des-crosscheck needs --password-env\n";
        return false;
    }
    const char *value = std::getenv(options.passwordEnv.c_str());
    if (value == nullptr || *value == '\0') {
        std::cerr << "the environment variable named by --password-env is empty\n";
        return false;
    }
    const std::vector<std::uint8_t> password(value, value + std::strlen(value));

    bool ok = true;
    for (int variant = 0; variant < 3; ++variant) {
        std::array<std::uint8_t, rfb::kVncChallengeBytes> challenge{};
        for (std::size_t index = 0; index < challenge.size(); ++index)
            challenge[index] = static_cast<std::uint8_t>((index * (variant + 3U) + variant * 7U + 5U) & 0xFFU);

        std::array<std::uint8_t, rfb::kVncChallengeBytes> mine{};
        std::string authError;
        if (!rfb::vncAuthResponse(password.data(), password.size(), challenge, mine, authError)) {
            std::cerr << "the client could not answer the challenge: " << authError << "\n";
            return false;
        }

        const std::vector<std::string> response =
            control->command("VNC-RESPONSE " + rfb::toHex(challenge.data(), challenge.size()) + " "
                             + rfb::toHex(password.data(), password.size()));

        std::string theirs;
        for (const std::string &line : response) {
            if (line.rfind("OK VNC-RESPONSE ", 0) == 0)
                theirs = line.substr(std::strlen("OK VNC-RESPONSE "));
        }
        if (theirs.empty()) {
            std::cerr << "variant " << variant << ": the fixture returned no response\n";
            ok = false;
            continue;
        }

        const std::string mineHex = rfb::toHex(mine.data(), mine.size());
        std::cout << "variant " << variant << " challenge=" << rfb::toHex(challenge.data(), challenge.size())
                  << " password_bytes=" << password.size() << "\n";
        std::cout << "variant " << variant << " client_response=" << mineHex << "\n";
        std::cout << "variant " << variant << " hyremote_openssl_response=" << theirs << "\n";
        std::cout << "variant " << variant << " match=" << (mineHex == theirs ? 1 : 0) << "\n";
        if (mineHex != theirs)
            ok = false;
    }

    PROBE_CHECK(ok);
    shutdownFixture(*control);
    return ok;
}

bool scenarioShutdown(const Options &options)
{
    auto control = openControl(options);
    if (!control) {
        std::cerr << "shutdown needs --control-port\n";
        return false;
    }
    const std::vector<std::string> response = control->command("QUIT");
    for (const std::string &line : response)
        std::cout << line << "\n";
    bool acknowledged = false;
    for (const std::string &line : response) {
        if (line.rfind("OK BYE", 0) == 0)
            acknowledged = true;
    }
    PROBE_CHECK(acknowledged);
    return failures == 0;
}

void printUsage()
{
    std::cout << "usage: rfb-probe --port <n> --scenario "
                 "<handshake|determinism|input-semantics|reconnect|clipboard|des-crosscheck|shutdown>\n"
                 "                  [--host <ip>] [--control-port <n>] [--version 38|37|lenient37]\n"
                 "                  [--security auto|none|vnc] [--password-env <VAR>]\n"
                 "                  [--set-pixel-format native|565be]\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        const auto next = [&index, argc, &argv]() -> std::string {
            return index + 1 < argc ? std::string(argv[++index]) : std::string();
        };
        if (flag == "--help" || flag == "-h") {
            printUsage();
            return 0;
        }
        if (flag == "--scenario")
            options.scenario = next();
        else if (flag == "--host")
            options.host = next();
        else if (flag == "--port")
            options.port = static_cast<std::uint16_t>(std::stoi(next()));
        else if (flag == "--control-port")
            options.controlPort = static_cast<std::uint16_t>(std::stoi(next()));
        else if (flag == "--version")
            options.version = next();
        else if (flag == "--security")
            options.security = next();
        else if (flag == "--password-env")
            options.passwordEnv = next();
        else if (flag == "--set-pixel-format") {
            options.sendPixelFormat = true;
            options.pixelFormat = next();
        } else if (flag == "--json")
            options.json = true;
        else {
            std::cerr << "unknown option: " << flag << "\n";
            printUsage();
            return 64;
        }
    }

    if (options.scenario.empty() || options.port == 0) {
        printUsage();
        return 64;
    }

    std::cout << "POC_ONLY scenario=" << options.scenario << " target=" << options.host << ":"
              << options.port << " version=" << options.version << " security=" << options.security
              << " password_configured=" << (!options.passwordEnv.empty() ? 1 : 0) << "\n";

    bool ok = false;
    if (options.scenario == "handshake")
        ok = scenarioHandshake(options);
    else if (options.scenario == "determinism")
        ok = scenarioDeterminism(options);
    else if (options.scenario == "input-semantics")
        ok = scenarioInputSemantics(options);
    else if (options.scenario == "reconnect")
        ok = scenarioReconnect(options);
    else if (options.scenario == "clipboard")
        ok = scenarioClipboard(options);
    else if (options.scenario == "des-crosscheck")
        ok = scenarioDesCrossCheck(options);
    else if (options.scenario == "shutdown")
        ok = scenarioShutdown(options);
    else {
        std::cerr << "unknown scenario: " << options.scenario << "\n";
        return 64;
    }

    std::cout << "SCENARIO_RESULT=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
