#include "s4/presentation/http/clip_descriptor_controller.hpp"

#include "s4/application/dtos/iso_time_utils.hpp"
#include "s4/domain/value_objects/media_kind.hpp"

#include <chrono>

namespace ods::s4::presentation {

std::optional<application::ClipDescriptorDTO> ClipDescriptorController::getClipDescriptor(const std::string& clipId) const {
    if (!m_repository) {
        return std::nullopt;
    }

    const auto clipOpt = m_repository->findById(clipId);
    if (!clipOpt.has_value()) {
        return std::nullopt;
    }

    const auto& clip = *clipOpt;
    application::ClipDescriptorDTO dto;
    dto.clipId = clip.clipId();
    dto.fileUri = clip.filePath();
    dto.startTimeIso = application::to_iso8601(clip.timeWindow().startTime());
    dto.endTimeIso = application::to_iso8601(clip.timeWindow().endTime());
    dto.sha256Hash = clip.sha256Hash();
    dto.isRetained = clip.isRetained();
    dto.isLockedForAudit = clip.isLockedForAudit();
    dto.mediaType = domain::to_contract_name(clip.mediaKind());
    if (clip.expiresAt().has_value()) {
        dto.expiresAtIso = application::to_iso8601(*clip.expiresAt());
    }
    dto.isAvailable = clip.isAvailableAt(std::chrono::system_clock::now());

    for (const auto& point : clip.pointsOfInterest()) {
        dto.pointsOfInterest.push_back({point.x, point.y, point.label});
    }

    return dto;
}

} // namespace ods::s4::presentation