#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace ods::s4::domain {

struct PointOfInterest {
    double x{0.0};
    double y{0.0};
    std::string label;
};

class ClipDescriptor {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    ClipDescriptor(
        std::string clipId,
        std::string fileUri,
        TimePoint startTime,
        TimePoint endTime,
        std::vector<PointOfInterest> pointsOfInterest = {}
    ) : m_clipId(std::move(clipId)),
        m_fileUri(std::move(fileUri)),
        m_startTime(startTime),
        m_endTime(endTime),
        m_pointsOfInterest(std::move(pointsOfInterest)) {}

    [[nodiscard]] const std::string& clipId() const noexcept { return m_clipId; }
    [[nodiscard]] const std::string& fileUri() const noexcept { return m_fileUri; }
    [[nodiscard]] TimePoint startTime() const noexcept { return m_startTime; }
    [[nodiscard]] TimePoint endTime() const noexcept { return m_endTime; }
    [[nodiscard]] const std::vector<PointOfInterest>& pointsOfInterest() const noexcept { return m_pointsOfInterest; }

private:
    std::string m_clipId;
    std::string m_fileUri;
    TimePoint m_startTime;
    TimePoint m_endTime;
    std::vector<PointOfInterest> m_pointsOfInterest;
};

} // namespace ods::s4::domain
