// POC_ONLY: deterministic frame correctness - full frames, incremental damage, geometry resize and
// an explicitly requested pixel format.
//
// Every assertion compares against a value derived from the content definition in
// `harness/deterministic_content`, never against "what the server sent". A hash that merely matched
// the server would prove nothing.
#include "harness/scripted_server.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

using namespace poc::core_remote;

struct ScenarioResult
{
    std::uint64_t sourceHash[harness::kStepCount] = {};
    std::uint64_t decodedHash[harness::kStepCount] = {};
};

// Runs the full deterministic sequence against the scripted double.
void runScenario(const rfb::PixelFormat &format, bool sendPixelFormat, const char *label)
{
    harness::ScriptedServerConfig serverConfig;
    serverConfig.behaviour = harness::ScriptedBehaviour::None38;
    serverConfig.steps = {0, 1, 2, 3, 4, 5};

    std::string startError;
    auto server = harness::ScriptedServer::start(serverConfig, startError);
    CHECK(server != nullptr);
    if (!server) {
        std::cerr << "scenario " << label << " could not start its server: " << startError << "\n";
        return;
    }

    rfb::ClientConfig config;
    config.port = server->port();
    config.ioTimeoutMs = 4000;
    config.sendPixelFormat = sendPixelFormat;
    config.pixelFormat = format;

    rfb::Client client(config);
    CHECK(client.connect().ok());

    for (int step = 0; step < harness::kStepCount; ++step) {
        const harness::Step &definition = harness::step(step);
        const std::uint64_t expected = harness::framebufferForFormat(step, format).hash();

        rfb::UpdateInfo update;
        if (definition.resizes) {
            // A resize is requested over the geometry the client currently believes in.
            CHECK(client.requestFramebufferUpdate(false, 0, 0,
                                                  static_cast<std::uint16_t>(client.framebuffer().width()),
                                                  static_cast<std::uint16_t>(client.framebuffer().height()))
                      .ok());
        } else if (step == 0) {
            CHECK(client.requestFullUpdate().ok());
        } else {
            CHECK(client.requestFramebufferUpdate(true,
                                                  definition.changed.x,
                                                  definition.changed.y,
                                                  definition.changed.width,
                                                  definition.changed.height)
                      .ok());
        }

        const rfb::Error error = client.waitForFramebufferUpdate(update);
        CHECK(error.ok());
        if (!error.ok()) {
            std::cerr << "scenario " << label << " step " << step << " failed: " << error.detail << "\n";
            break;
        }

        CHECK(!update.rectangles.empty());
        CHECK_EQ(client.framebuffer().width(), definition.width);
        CHECK_EQ(client.framebuffer().height(), definition.height);
        CHECK_EQ(client.framebuffer().hash(), expected);

        if (definition.resizes) {
            CHECK(update.resized);
            CHECK_EQ(update.rectangles.size(), std::size_t{2});
            if (update.rectangles.size() == 2) {
                CHECK_EQ(update.rectangles[0].encoding, rfb::kEncodingDesktopSize);
                CHECK_EQ(update.rectangles[0].width, definition.width);
                CHECK_EQ(update.rectangles[0].height, definition.height);
                CHECK_EQ(update.rectangles[1].encoding, rfb::kEncodingRaw);
            }
        } else {
            CHECK(!update.resized);
            CHECK_EQ(update.rectangles.size(), std::size_t{1});
            if (!update.rectangles.empty()) {
                CHECK_EQ(update.rectangles[0].x, definition.changed.x);
                CHECK_EQ(update.rectangles[0].y, definition.changed.y);
                CHECK_EQ(update.rectangles[0].width, definition.changed.width);
                CHECK_EQ(update.rectangles[0].height, definition.changed.height);

                // The damaged region must also match when hashed on its own, which catches a client
                // that copies the right number of bytes to the wrong place.
                CHECK_EQ(client.framebuffer().regionHash(definition.changed.x,
                                                         definition.changed.y,
                                                         definition.changed.width,
                                                         definition.changed.height),
                         harness::framebufferForFormat(step, format)
                             .regionHash(definition.changed.x, definition.changed.y, definition.changed.width,
                                         definition.changed.height));
            }
        }
        CHECK(update.rawBytes > 0);
    }

    // Direct pixel assertions, not only aggregate hashes: the known colour blocks must be present at
    // their exact coordinates after every damage and the resize.
    const rfb::Framebuffer &framebuffer = client.framebuffer();
    for (const harness::Block &block : harness::colourBlocks()) {
        const std::uint16_t x = static_cast<std::uint16_t>(block.rect.x + block.rect.width / 2);
        const std::uint16_t y = static_cast<std::uint16_t>(block.rect.y + block.rect.height / 2);
        const std::uint32_t expectedPixel =
            harness::framebufferForFormat(harness::kStepCount - 1, format).pixel(x, y);
        CHECK_EQ(framebuffer.pixel(x, y), expectedPixel);
    }

    // The horizontal line at y=30 and the vertical line at x=34 must still be where they belong.
    CHECK_EQ(framebuffer.pixel(70, 30),
             harness::framebufferForFormat(harness::kStepCount - 1, format).pixel(70, 30));
    CHECK_EQ(framebuffer.pixel(34, 58),
             harness::framebufferForFormat(harness::kStepCount - 1, format).pixel(34, 58));

    client.disconnect();
    CHECK(server->waitForSessions(1, 4000));
    CHECK_EQ(server->lastSession().updatesServed, std::size_t{harness::kStepCount});
    server->stop();
}

void testHashSanity()
{
    // Negative control: if consecutive steps hashed identically, the assertions above would be
    // vacuous. This is checked rather than assumed.
    for (int step = 1; step < harness::kStepCount; ++step) {
        CHECK(harness::framebufferFor(step - 1).hash() != harness::framebufferFor(step).hash());
    }
    // Same content, same hash, every time.
    CHECK_EQ(harness::framebufferFor(3).hash(), harness::framebufferFor(3).hash());
    // A single pixel difference must change the hash.
    rfb::Framebuffer tweaked = harness::framebufferFor(3);
    tweaked.setPixel(0, 0, tweaked.pixel(0, 0) ^ 0x000001U);
    CHECK(tweaked.hash() != harness::framebufferFor(3).hash());
}

void testNativeFormatNoSetPixelFormat()
{
    runScenario(rfb::PixelFormat::hyremoteNative(), false, "native-format");
}

void testNativeFormatWithSetPixelFormat()
{
    runScenario(rfb::PixelFormat::hyremoteNative(), true, "native-format-explicit");
}

void testSixteenBitBigEndianFormat()
{
    // A different, legal format: proves SetPixelFormat is honoured on both sides and that the client
    // decodes by format rather than by assumption.
    runScenario(rfb::PixelFormat::rgb565BigEndian(), true, "rgb565-big-endian");
}

}  // namespace

int main()
{
    testHashSanity();
    testNativeFormatNoSetPixelFormat();
    testNativeFormatWithSetPixelFormat();
    testSixteenBitBigEndianFormat();

    return poc_test::finish("deterministic frame correctness (full, incremental, resize, pixel format)");
}
