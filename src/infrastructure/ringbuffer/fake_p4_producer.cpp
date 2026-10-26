#include "s4/infrastructure/ringbuffer/fake_p4_producer.hpp"

#include <uuid/uuid.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

// Largura das faixas do padrao e quanto ele anda por quadro, em pixels.
constexpr std::size_t kStripeWidth = 16;
constexpr std::size_t kStripeStepPerImage = 4;
constexpr std::uint8_t kDarkLuma = 60;
constexpr std::uint8_t kBrightLuma = 200;
constexpr std::uint8_t kNeutralChroma = 128;

std::string new_session_id() {
    uuid_t uuid;
    char text[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, text);
    return text;
}

domain::Nanoseconds now_utc_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Ruido deterministico (LCG): mesmas imagens em toda execucao.
std::uint8_t noisy(std::uint8_t base, std::uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    const int noise = static_cast<int>((state >> 24) & 0x1F) - 16;
    const int value = static_cast<int>(base) + noise;
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

} // namespace

FakeP4Producer::FakeP4Producer(FakeP4Options options) : m_options(std::move(options)) {
    if (m_options.distinctImages == 0) {
        throw domain::FrameSourceError("fake P4 needs at least one distinct image");
    }
    m_images.reserve(m_options.distinctImages);
    for (std::size_t index = 0; index < m_options.distinctImages; ++index) {
        m_images.push_back(renderImage(index));
    }
    m_nextCapturedAtUtcNs =
        m_options.startCapturedAtUtcNs != 0 ? m_options.startCapturedAtUtcNs : now_utc_ns();

    m_session.sessionId = new_session_id();
    m_session.mode = m_options.mode;
    m_session.startedAtUtcNs = m_nextCapturedAtUtcNs;
    m_session.sources = {m_options.sourceId};
    m_session.reason = domain::SessionChangeReason::Start;
}

FakeP4Frame FakeP4Producer::nextFrame() {
    const domain::RawVideoFormat& format = m_options.format;
    FakeP4Frame frame;
    frame.metadata.capturedAtUtcNs = m_nextCapturedAtUtcNs;
    frame.metadata.sourceId = m_options.sourceId;
    frame.metadata.sessionId = m_session.sessionId;
    frame.metadata.frameIndex = m_nextFrameIndex;
    frame.metadata.imageWidth = format.width();
    frame.metadata.imageHeight = format.height();
    frame.metadata.pixelFormat = format.pixelFormat();
    frame.metadata.fps = format.fps();
    frame.metadata.raw = true;
    frame.pixels = &m_images[m_framesProduced % m_images.size()];

    m_nextCapturedAtUtcNs += format.frameDurationNs();
    ++m_nextFrameIndex;
    ++m_framesProduced;
    return frame;
}

domain::SessionStarted FakeP4Producer::startNewSession(
    domain::SessionChangeReason reason,
    std::optional<domain::Nanoseconds> rewindToUtcNs
) {
    domain::SessionStarted next;
    next.sessionId = new_session_id();
    next.mode = m_options.mode;
    next.sources = {m_options.sourceId};
    next.previousSessionId = m_session.sessionId;
    next.reason = reason;
    if (reason == domain::SessionChangeReason::Seek || reason == domain::SessionChangeReason::Loop) {
        m_nextCapturedAtUtcNs = rewindToUtcNs.value_or(m_session.startedAtUtcNs);
    }
    next.startedAtUtcNs = m_nextCapturedAtUtcNs;
    m_nextFrameIndex = 0;
    m_session = next;
    return next;
}

std::vector<std::uint8_t> FakeP4Producer::renderImage(std::size_t index) const {
    const domain::RawVideoFormat& format = m_options.format;
    const std::size_t width = format.width();
    const std::size_t height = format.height();
    std::vector<std::uint8_t> image(format.frameBytes(), kNeutralChroma);
    std::uint32_t noiseState = static_cast<std::uint32_t>(index) * 2654435761U + 1U;
    const std::size_t shift = index * kStripeStepPerImage;

    const auto luma = [&](std::size_t x, std::size_t y) {
        const bool bright = ((x + y / 2 + shift) / kStripeWidth) % 2 == 0;
        return noisy(bright ? kBrightLuma : kDarkLuma, noiseState);
    };

    switch (format.pixelFormat()) {
        case domain::RawPixelFormat::NV12:
        case domain::RawPixelFormat::GRAY: {
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    image[y * width + x] = luma(x, y);
                }
            }
            if (format.pixelFormat() == domain::RawPixelFormat::NV12) {
                // Plano UV intercalado, meia resolucao: um gradiente que anda.
                std::uint8_t* uv = image.data() + width * height;
                for (std::size_t y = 0; y < height / 2; ++y) {
                    for (std::size_t x = 0; x < width / 2; ++x) {
                        uv[y * width + 2 * x] = static_cast<std::uint8_t>(96 + (x + shift) % 64);
                        uv[y * width + 2 * x + 1] = static_cast<std::uint8_t>(96 + y % 64);
                    }
                }
            }
            break;
        }
        case domain::RawPixelFormat::RGB:
        case domain::RawPixelFormat::BGR: {
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    const std::uint8_t value = luma(x, y);
                    std::uint8_t* pixel = image.data() + (y * width + x) * 3;
                    pixel[0] = value;
                    pixel[1] = static_cast<std::uint8_t>(value / 2);
                    pixel[2] = static_cast<std::uint8_t>(255 - value);
                }
            }
            break;
        }
    }
    return image;
}

// ------------------------------------------------------------------- feed

FakeP4Feed::FakeP4Feed(
    FakeP4Options options,
    FrameSink frames,
    SessionSink sessions,
    std::size_t maxFrames,
    bool realtime
) : m_producer(std::move(options)),
    m_frames(std::move(frames)),
    m_sessions(std::move(sessions)),
    m_maxFrames(maxFrames),
    m_realtime(realtime) {}

FakeP4Feed::~FakeP4Feed() {
    stop();
}

void FakeP4Feed::start() {
    if (m_thread.joinable()) {
        return;
    }
    m_stopRequested.store(false);
    m_thread = std::thread([this] { run(); });
}

void FakeP4Feed::stop() {
    m_stopRequested.store(true);
    waitUntilFinished();
}

void FakeP4Feed::waitUntilFinished() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void FakeP4Feed::run() {
    if (m_sessions) {
        m_sessions(m_producer.currentSession());
    }
    const auto frameDuration = std::chrono::nanoseconds(m_producer.options().format.frameDurationNs());
    auto nextTick = std::chrono::steady_clock::now();
    for (std::size_t produced = 0; !m_stopRequested.load(); ++produced) {
        if (m_maxFrames != 0 && produced >= m_maxFrames) {
            return;
        }
        const FakeP4Frame frame = m_producer.nextFrame();
        try {
            m_frames(frame.metadata, frame.pixels->data(), frame.pixels->size());
            m_delivered.fetch_add(1);
        } catch (const std::exception& error) {
            const std::uint64_t failed = m_failed.fetch_add(1) + 1;
            if (failed == 1 || failed % 30 == 0) {
                std::fprintf(stderr, "[S4.1] fake P4 frame not accepted (%llu so far): %s\n",
                             static_cast<unsigned long long>(failed), error.what());
            }
        }
        if (m_realtime) {
            nextTick += frameDuration;
            std::this_thread::sleep_until(nextTick);
        }
    }
}

} // namespace ods::s4::infrastructure
