#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <vector>
#include "s4/domain/value_objects/clip_descriptor.hpp"
#include "s4/domain/value_objects/media_kind.hpp"
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
        std::vector<PointOfInterest> pointsOfInterest = {},
        std::optional<MediaKind> mediaKind = std::nullopt,
        std::optional<TimePoint> expiresAt = std::nullopt
    ) : m_clipId(std::move(clipId)),
        m_eventId(std::move(eventId)),
        m_filePath(std::move(filePath)),
        m_sha256Hash(std::move(sha256Hash)),
        m_timeWindow(std::move(timeWindow)),
        m_isRetained(isRetained),
        m_isLockedForAudit(isLockedForAudit),
        m_createdAt(createdAt),
        m_pointsOfInterest(std::move(pointsOfInterest)),
        m_mediaKind(mediaKind.value_or(media_kind_of(m_filePath))),
        m_expiresAt(expiresAt) {}

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
    [[nodiscard]] MediaKind mediaKind() const noexcept { return m_mediaKind; }
    // Prazo prometido no contrato 6c (expires_at): depois dele o media_uri nao
    // resolve mais. Vazio so em clipes antigos ou criados sem politica.
    [[nodiscard]] const std::optional<TimePoint>& expiresAt() const noexcept { return m_expiresAt; }

    // A evidencia pode ser entregue a quem pede? Nao, se ja foi expurgada ou se
    // o prazo venceu e a varredura ainda nao passou (a leitura nao espera o
    // daemon). Evidencia travada para auditoria continua disponivel: e
    // justamente por isso que ela sobrevive ao prazo.
    [[nodiscard]] bool isAvailableAt(TimePoint now) const noexcept {
        if (!m_isRetained) return false;
        if (m_isLockedForAudit || !m_expiresAt.has_value()) return true;
        return now < *m_expiresAt;
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
    MediaKind m_mediaKind;
    std::optional<TimePoint> m_expiresAt;
};

} // namespace ods::s4::domain
