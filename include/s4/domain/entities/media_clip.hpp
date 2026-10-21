#pragma once

#include <string>
#include <chrono>
#include <vector>
#include "s4/domain/value_objects/clip_descriptor.hpp"
#include "s4/domain/value_objects/time_window.hpp"

namespace ods::s4::domain {

class MediaClip {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    MediaClip(
        std::string clipId,
        std::string eventId,
        std::string filePath,
        std::string sha256Hash,
        TimeWindow timeWindow,
        bool isRetained = true,
        bool isLockedForAudit = false,
        TimePoint createdAt = std::chrono::system_clock::now(),
        std::vector<PointOfInterest> pointsOfInterest = {}
    ) : m_clipId(std::move(clipId)),
        m_eventId(std::move(eventId)),
        m_filePath(std::move(filePath)),
        m_sha256Hash(std::move(sha256Hash)),
        m_timeWindow(std::move(timeWindow)),
        m_isRetained(isRetained),
        m_isLockedForAudit(isLockedForAudit),
        m_createdAt(createdAt),
        m_pointsOfInterest(std::move(pointsOfInterest)) {}

    [[nodiscard]] const std::string& clipId() const noexcept { return m_clipId; }
    [[nodiscard]] const std::string& eventId() const noexcept { return m_eventId; }
    [[nodiscard]] const std::string& filePath() const noexcept { return m_filePath; }
    [[nodiscard]] const std::string& sha256Hash() const noexcept { return m_sha256Hash; }
    [[nodiscard]] const TimeWindow& timeWindow() const noexcept { return m_timeWindow; }
    [[nodiscard]] bool isRetained() const noexcept { return m_isRetained; }
    [[nodiscard]] bool isLockedForAudit() const noexcept { return m_isLockedForAudit; }
    [[nodiscard]] TimePoint createdAt() const noexcept { return m_createdAt; }
    [[nodiscard]] const std::vector<PointOfInterest>& pointsOfInterest() const noexcept {
        return m_pointsOfInterest;
    }

    void markAsPurged() noexcept {
        m_isRetained = false;
    }

    void lockForAudit() noexcept {
        m_isLockedForAudit = true;
    }

    void unlockForAudit() noexcept {
        m_isLockedForAudit = false;
    }

private:
    std::string m_clipId;
    std::string m_eventId;
    std::string m_filePath;
    std::string m_sha256Hash;
    TimeWindow m_timeWindow;
    bool m_isRetained{true};
    bool m_isLockedForAudit{false};
    TimePoint m_createdAt;
    std::vector<PointOfInterest> m_pointsOfInterest;
};

} // namespace ods::s4::domain
