#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"

#include <chrono>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

SyntheticFrameSource::SyntheticFrameSource(SyntheticSourceOptions options)
    : m_options(std::move(options)) {
    if (m_options.fps <= 0.0 || m_options.gop == 0) {
        throw domain::FrameSourceError("fps and gop must be positive");
    }
}

SyntheticFrameSource::~SyntheticFrameSource() {
    stop();
}

void SyntheticFrameSource::start(domain::FrameCallback onFrame) {
    if (m_running.exchange(true)) {
        return;
    }
    m_stopRequested.store(false);
    m_thread = std::thread([this, onFrame] { produce(onFrame); });
}

void SyntheticFrameSource::stop() {
    m_stopRequested.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
}

void SyntheticFrameSource::waitUntilFinished() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
}

void SyntheticFrameSource::produce(domain::FrameCallback onFrame) {
    const auto interval = std::chrono::duration<double>(1.0 / m_options.fps);
    for (std::size_t index = 0; !m_stopRequested.load(); ++index) {
        if (m_options.maxFrames != 0 && index >= m_options.maxFrames) {
            return;
        }
        const std::vector<std::uint8_t> payload = payloadAt(index);
        domain::CapturedFrame frame;
        frame.captureTsNs = timestampAt(index);
        frame.data = payload.data();
        frame.length = payload.size();
        frame.isKeyframe = isKeyframeAt(index);
        frame.sessionId = m_options.sessionId;
        onFrame(frame);
        if (m_options.realtime) {
            std::this_thread::sleep_for(interval);
        }
    }
}

std::vector<std::uint8_t> SyntheticFrameSource::payloadAt(std::size_t index) const {
    const std::size_t size = isKeyframeAt(index) ? m_options.keyframeBytes : m_options.deltaBytes;
    std::vector<std::uint8_t> payload(size, static_cast<std::uint8_t>(index % 256));
    for (std::size_t byte = 0; byte < 4 && byte < size; ++byte) {
        payload[byte] = static_cast<std::uint8_t>((index >> (8 * (3 - byte))) & 0xFF);
    }
    return payload;
}

domain::Nanoseconds SyntheticFrameSource::timestampAt(std::size_t index) const {
    const auto offsetNs = static_cast<domain::Nanoseconds>(
        static_cast<double>(index) * domain::kNanosecondsPerSecond / m_options.fps
    );
    return m_options.startTsNs + offsetNs;
}

bool SyntheticFrameSource::isKeyframeAt(std::size_t index) const {
    return index % m_options.gop == 0;
}

} // namespace ods::s4::infrastructure
