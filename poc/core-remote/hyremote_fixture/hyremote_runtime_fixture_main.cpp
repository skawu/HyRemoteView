// POC_ONLY HyRemote runtime fixture.
//
// This is the server half of the G3-POC-1A evidence. It does not reimplement RFB: it starts
// HyRemote's own runtime transport (`HyRemote::detail::createRfbTransport`) and drives it with the
// deterministic content defined once in `harness/deterministic_content`. Everything a viewer sees
// therefore travels through HyRemote's real transport, and every input event the runtime publishes
// comes from HyRemote's own normalisation.
//
// A small line-based control channel exists so the client-side probe can make the run deterministic:
// it publishes a frame on demand and reports what the runtime published in return, instead of both
// sides racing a timer.

#include "harness/deterministic_content.hpp"
#include "net/socket.hpp"
#include "rfb/pixel.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>

#include "hyremote/core/frame.hpp"
#include "hyremote/core/storage.hpp"
#include "transport/rfb_transport.hpp"

#ifdef HYREMOTE_HAS_TRANSPORT_SECURITY
#  include "transport/vnc_auth.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace poc::core_remote;

struct FixtureState
{
    std::mutex mutex;
    std::vector<std::string> inputLog;
    std::string securityDescription = "none";
};

const char *kindName(hyremote::InputEventKind kind)
{
    using hyremote::InputEventKind;
    switch (kind) {
    case InputEventKind::PointerMove:
        return "pointer-move";
    case InputEventKind::PointerButton:
        return "pointer-button";
    case InputEventKind::PointerScroll:
        return "pointer-scroll";
    case InputEventKind::Key:
        return "key";
    case InputEventKind::Text:
        return "text";
    case InputEventKind::None:
        return "none";
    }
    return "unknown";
}

std::string formatInputEvent(const hyremote::InputEvent &event)
{
    char buffer[512] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "E %s x=%.1f y=%.1f btn=%d pressed=%d key=%d mods=%u sx=%.1f sy=%.1f text=%s",
                  kindName(event.kind),
                  static_cast<double>(event.x),
                  static_cast<double>(event.y),
                  static_cast<int>(event.button),
                  event.pressed ? 1 : 0,
                  static_cast<int>(event.key),
                  static_cast<unsigned>(event.modifiers),
                  static_cast<double>(event.scrollX),
                  static_cast<double>(event.scrollY),
                  event.kind == hyremote::InputEventKind::Text ? event.textUtf8.c_str() : "");
    return std::string(buffer);
}

hyremote::RemoteFrame makeFrame(int step)
{
    const harness::Step &definition = harness::step(step);
    const std::size_t width = definition.width;
    const std::size_t height = definition.height;

    auto storage = hyremote::CpuFrameStorage::createSinglePlane(width * 4U, height);
    if (!storage || storage->mutablePlane(0) == nullptr)
        throw std::runtime_error("could not allocate the deterministic frame storage");

    const std::vector<std::uint8_t> pixels =
        harness::encodeFrame(step, rfb::PixelFormat::hyremoteNative());
    std::byte *destination = storage->mutablePlane(0);
    std::memcpy(destination, pixels.data(), pixels.size());

    hyremote::RemoteFrame frame;
    frame.id = static_cast<std::uint64_t>(step) + 1U;
    frame.geometry.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    frame.geometry.pixelFormat = hyremote::PixelFormat::Rgba8888;
    frame.geometry.alphaMode = hyremote::AlphaMode::Opaque;
    frame.geometry.planeCount = 1;
    frame.storage = std::move(storage);
    frame.timing.pts = hyremote::Clock::now();
    frame.timing.ptsSource = hyremote::PtsSource::Completion;
    frame.damage = hyremote::Damage::fullFrame();
    return frame;
}

std::string hexOf(std::uint64_t value)
{
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::vector<std::uint8_t> bytesFromHex(const std::string &text)
{
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    if (text.size() % 2 != 0)
        return out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        const int high = nibble(text[index]);
        const int low = nibble(text[index + 1]);
        if (high < 0 || low < 0)
            return {};
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

std::string hexOfBytes(const std::vector<std::uint8_t> &bytes)
{
    return rfb::toHex(bytes.data(), bytes.size());
}

class ControlChannel
{
public:
    ControlChannel(std::shared_ptr<FixtureState> state,
                   hyremote::Transport *transport,
                   QCoreApplication *application)
        : m_state(std::move(state))
        , m_transport(transport)
        , m_application(application)
    {
    }

    bool start(std::uint16_t requestedPort, std::string &error)
    {
        m_listener = net::Listener::bindTcp("127.0.0.1", requestedPort, 1, error);
        if (!m_listener.valid())
            return false;
        m_thread = std::thread([this] { run(); });
        return true;
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return m_listener.port(); }

    void stop() noexcept
    {
        m_stop.store(true);
        m_listener.close();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void run() noexcept
    {
        while (!m_stop.load()) {
            net::Socket client;
            std::string error;
            const net::IoStatus status = m_listener.accept(client, 200, error);
            if (status == net::IoStatus::Closed)
                return;
            if (status != net::IoStatus::Ok)
                continue;
            serve(client);
        }
    }

    bool readLine(net::Socket &client, std::string &line, std::string &error)
    {
        line.clear();
        char character = 0;
        while (line.size() < 4096) {
            if (client.readExact(&character, 1, 5000, error) != net::IoStatus::Ok)
                return false;
            if (character == '\n')
                return true;
            line.push_back(character);
        }
        return false;
    }

    void writeLine(net::Socket &client, const std::string &line)
    {
        std::string payload = line + "\n";
        std::string error;
        (void)client.writeAll(payload.data(), payload.size(), 5000, error);
    }

    void serve(net::Socket &client) noexcept
    {
        std::string error;
        std::string line;
        while (!m_stop.load() && readLine(client, line, error)) {
            std::istringstream stream(line);
            std::string command;
            stream >> command;

            if (command == "PING") {
                writeLine(client, "OK PONG");
                continue;
            }

            if (command == "STEP") {
                int step = 0;
                stream >> step;
                if (step < 0 || step >= harness::kStepCount) {
                    writeLine(client, "ERR");
                    continue;
                }
                const harness::Step &definition = harness::step(step);
                m_transport->enqueueFrame(makeFrame(step));

                std::size_t inputCount = 0;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    inputCount = m_state->inputLog.size();
                }
                std::ostringstream reply;
                reply << "OK STEP " << step << " FB " << definition.width << "x" << definition.height
                      << " SRC " << hexOf(harness::framebufferFor(step).hash()) << " INPUT " << inputCount;
                writeLine(client, reply.str());
                continue;
            }

            if (command == "DUMP-INPUT") {
                std::vector<std::string> log;
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    log = m_state->inputLog;
                }
                writeLine(client, "INPUT " + std::to_string(log.size()));
                for (const std::string &entry : log)
                    writeLine(client, entry);
                writeLine(client, "OK INPUT-END " + std::to_string(log.size()));
                continue;
            }

            if (command == "RESET-INPUT") {
                {
                    std::lock_guard<std::mutex> lock(m_state->mutex);
                    m_state->inputLog.clear();
                }
                writeLine(client, "OK RESET-INPUT 0");
                continue;
            }

            if (command == "GEOMETRY") {
                writeLine(client, "OK GEOMETRY 0x0");
                continue;
            }

            if (command == "SECURITY") {
                writeLine(client, "OK SECURITY " + m_state->securityDescription);
                continue;
            }

            if (command == "VNC-RESPONSE") {
                // Cross-check of the client's own DES implementation against the server's
                // OpenSSL-based one. The credential arrives on the control channel, which the probe
                // connects to out-of-band; it is never printed.
                std::string challengeHex;
                std::string passwordHex;
                stream >> challengeHex >> passwordHex;
#ifdef HYREMOTE_HAS_TRANSPORT_SECURITY
                const std::vector<std::uint8_t> challenge = bytesFromHex(challengeHex);
                const std::vector<std::uint8_t> password = bytesFromHex(passwordHex);
                if (challenge.size() != HyRemote::detail::kVncAuthChallengeBytes || password.empty()) {
                    writeLine(client, "ERR");
                    continue;
                }
                QByteArrayView challengeView(reinterpret_cast<const char *>(challenge.data()),
                                             static_cast<qsizetype>(challenge.size()));
                QByteArrayView passwordView(reinterpret_cast<const char *>(password.data()),
                                            static_cast<qsizetype>(password.size()));
                QByteArray response;
                QString authError;
                if (!HyRemote::detail::computeVncAuthResponse(passwordView, challengeView, response,
                                                              authError)) {
                    writeLine(client, "ERR");
                    continue;
                }
                const std::vector<std::uint8_t> responseBytes(response.begin(), response.end());
                writeLine(client, "OK VNC-RESPONSE " + hexOfBytes(responseBytes));
#else
                (void)challengeHex;
                (void)passwordHex;
                writeLine(client, "ERR");
#endif
                continue;
            }

            if (command == "QUIT") {
                writeLine(client, "OK BYE");
                m_stop.store(true);
                QMetaObject::invokeMethod(m_application, [] { QCoreApplication::quit(); },
                                          Qt::QueuedConnection);
                return;
            }

            writeLine(client, "ERR");
        }
    }

    std::shared_ptr<FixtureState> m_state;
    hyremote::Transport *m_transport = nullptr;
    QCoreApplication *m_application = nullptr;
    net::Listener m_listener;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

std::uint16_t probeFreePort()
{
    std::string error;
    net::Listener listener = net::Listener::bindTcp("127.0.0.1", 0, 1, error);
    if (!listener.valid())
        return 0;
    return listener.port();
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    std::uint16_t port = 0;
    std::uint16_t controlPort = 0;
    std::string security = "none";
    std::string passwordEnv;

    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        const auto next = [&index, argc, &argv]() -> std::string {
            return index + 1 < argc ? std::string(argv[++index]) : std::string();
        };
        if (flag == "--port")
            port = static_cast<std::uint16_t>(std::stoi(next()));
        else if (flag == "--control-port")
            controlPort = static_cast<std::uint16_t>(std::stoi(next()));
        else if (flag == "--security")
            security = next();
        else if (flag == "--password-env")
            passwordEnv = next();
        else {
            std::cerr << "unknown option: " << flag << "\n";
            return 64;
        }
    }

    if (port == 0)
        port = probeFreePort();
    if (controlPort == 0)
        controlPort = probeFreePort();
    if (port == 0 || controlPort == 0 || port == controlPort) {
        std::cerr << "could not obtain distinct ports\n";
        return 65;
    }

    auto state = std::make_shared<FixtureState>();

    HyRemote::detail::RfbSecurityConfig securityConfig;
    if (security == "vnc") {
#ifndef HYREMOTE_HAS_TRANSPORT_SECURITY
        std::cerr << "this fixture was built without POC_HYREMOTE_WITH_SECURITY=ON, so VNC "
                     "authentication is unavailable\n";
        return 66;
#else
        const char *password = passwordEnv.empty() ? nullptr : std::getenv(passwordEnv.c_str());
        if (password == nullptr || *password == '\0') {
            std::cerr << "VNC authentication requires a credential in the environment variable named "
                         "by --password-env\n";
            return 64;
        }
        securityConfig.vncAuthenticationRequired = true;
        securityConfig.password = QByteArray(password);
        state->securityDescription = "vnc-auth";
#endif
    }

    auto transport = HyRemote::detail::createRfbTransport(QHostAddress::LocalHost, port, securityConfig);
    if (!transport) {
        std::cerr << "the HyRemote RFB transport could not be created\n";
        return 67;
    }

    std::shared_ptr<FixtureState> sharedState = state;
    const bool started = transport->start(
        [sharedState](const hyremote::InputEvent &event) {
            if (event.kind == hyremote::InputEventKind::None)
                return;
            std::lock_guard<std::mutex> lock(sharedState->mutex);
            sharedState->inputLog.push_back(formatInputEvent(event));
        },
        [](const hyremote::TransportEvent &event) {
            std::cout << "EVENT " << static_cast<int>(event.code) << " " << event.message << std::endl;
        });
    if (!started) {
        std::cerr << "START_FAILED\n";
        return 68;
    }

    // Publishing step 0 before any viewer connects means ServerInit is available immediately: the
    // runtime only sends it once it holds a frame.
    transport->enqueueFrame(makeFrame(0));

    ControlChannel control(state, transport.get(), &application);
    std::string controlError;
    if (!control.start(controlPort, controlError)) {
        std::cerr << "control channel could not start: " << controlError << "\n";
        transport->stop();
        return 69;
    }

    std::cout << "READY transport_port=" << port << " control_port=" << control.port()
              << " security=" << state->securityDescription << std::endl;

    const int exitCode = application.exec();

    control.stop();
    transport->stop();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        std::cout << "STOPPED input_events=" << state->inputLog.size() << std::endl;
    }
    return exitCode;
}
