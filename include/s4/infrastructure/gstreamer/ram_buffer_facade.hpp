#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace ods::s4::infrastructure {

struct VideoFrame {
    std::vector<uint8_t> data;
    std::chrono::system_clock::time_point timestamp;
    uint32_t width{0};
    uint32_t height{0};
};

class IRAMBufferFacade {
public:
    virtual ~IRAMBufferFacade() = default;

    virtual void startCapture(const std::string& pipelineDesc) = 0;
    virtual void stopCapture() = 0;
    virtual std::vector<VideoFrame> extractWindow(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) = 0;
};

} // namespace ods::s4::infrastructure
