#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace ods::s4::domain {

class IMediaBufferReader {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    virtual ~IMediaBufferReader() = default;

    [[nodiscard]] virtual std::vector<std::vector<std::uint8_t>> readWindow(
        TimePoint start,
        TimePoint end
    ) const = 0;
};

} // namespace ods::s4::domain
