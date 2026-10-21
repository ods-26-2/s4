#include "s4/infrastructure/gstreamer/mock_ram_buffer_facade.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

namespace ods::s4::infrastructure {

MockRAMBufferFacade::MockRAMBufferFacade(
    std::chrono::seconds maxWindow,
    std::uint32_t fps,
    std::uint32_t width,
    std::uint32_t height
) : m_maxWindow(maxWindow),
    m_fps(fps),
    m_width(width),
    m_height(height) {}

MockRAMBufferFacade::~MockRAMBufferFacade() {
    stopCapture();
}

void MockRAMBufferFacade::startCapture(const std::string&) {
    stopCapture();
    m_capturing.store(true);
    m_captureThread = std::thread([this] { captureLoop(); });
}

void MockRAMBufferFacade::stopCapture() {
    m_capturing.store(false);
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
}

void MockRAMBufferFacade::captureLoop() {
    const auto interval = std::chrono::microseconds(1000000 / m_fps);
    while (m_capturing.load()) {
        const std::size_t frameSize = static_cast<std::size_t>(m_width) * m_height;
        VideoFrame frame;
        frame.data.assign(frameSize, 0);
        for (std::size_t i = 0; i < frameSize; ++i) {
            frame.data[i] = static_cast<std::uint8_t>(i % 251);
        }
        frame.timestamp = std::chrono::system_clock::now();
        frame.width = m_width;
        frame.height = m_height;
        feedFrame(std::move(frame));
        std::this_thread::sleep_for(interval);
    }
}

void MockRAMBufferFacade::feedFrame(VideoFrame frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.push_back(std::move(frame));
    const std::size_t maxFrames = static_cast<std::size_t>(m_maxWindow.count()) * m_fps;
    while (m_frames.size() > maxFrames) {
        m_frames.pop_front();
    }
}

std::vector<VideoFrame> MockRAMBufferFacade::extractWindow(
    domain::IMediaBufferReader::TimePoint start,
    domain::IMediaBufferReader::TimePoint end
) {
    std::vector<VideoFrame> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& frame : m_frames) {
        if (frame.timestamp >= start && frame.timestamp < end) {
            result.push_back(frame);
        }
    }
    return result;
}

std::vector<std::vector<std::uint8_t>> MockRAMBufferFacade::readWindow(
    domain::IMediaBufferReader::TimePoint start,
    domain::IMediaBufferReader::TimePoint end
) const {
    std::vector<std::vector<std::uint8_t>> result;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& frame : m_frames) {
        if (frame.timestamp >= start && frame.timestamp < end) {
            result.push_back(frame.data);
        }
    }
    return result;
}

} // namespace ods::s4::infrastructure