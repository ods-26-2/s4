#pragma once

#include "s4/domain/services/media_buffer_reader.hpp"
#include "s4/infrastructure/gstreamer/ram_buffer_facade.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ods::s4::infrastructure {

class MockRAMBufferFacade : public IRAMBufferFacade, public domain::IMediaBufferReader {
public:
    explicit MockRAMBufferFacade(
        std::chrono::seconds maxWindow = std::chrono::seconds(30),
        std::uint32_t fps = 15,
        std::uint32_t width = 640,
        std::uint32_t height = 480
    );

    ~MockRAMBufferFacade() override;

    void startCapture(const std::string& pipelineDesc) override;
    void stopCapture() override;
    [[nodiscard]] std::vector<VideoFrame> extractWindow(
        domain::IMediaBufferReader::TimePoint start,
        domain::IMediaBufferReader::TimePoint end
    ) override;
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> readWindow(
        domain::IMediaBufferReader::TimePoint start,
        domain::IMediaBufferReader::TimePoint end
    ) const override;

    void feedFrame(VideoFrame frame);

private:
    void captureLoop();

    std::deque<VideoFrame> m_frames;
    mutable std::mutex m_mutex;
    std::thread m_captureThread;
    std::atomic<bool> m_capturing{false};
    std::chrono::seconds m_maxWindow;
    std::uint32_t m_fps;
    std::uint32_t m_width;
    std::uint32_t m_height;
};

} // namespace ods::s4::infrastructure