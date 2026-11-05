#include "s4/infrastructure/b3_bus/clip_payload.hpp"

#include <filesystem>

#include "s4/application/dtos/iso_time_utils.hpp"
#include "s4/domain/value_objects/media_kind.hpp"
#include "s4/domain/value_objects/retention_policy.hpp"

namespace ods::s4::infrastructure {

std::string clip_media_uri(const domain::MediaClip& clip) {
    return "s4://clips/" + std::filesystem::path(clip.filePath()).filename().string();
}

nlohmann::json clip_payload(const domain::MediaClip& clip) {
    // Clipe sem expires_at gravado (criado fora do ExtractClipUseCase): o
    // contrato exige o campo, entao vale o prazo LGPD padrao a partir da gravacao.
    const auto expiresAt = clip.expiresAt().value_or(
        clip.createdAt() + domain::RetentionPolicy::kLgpdDefaultMaxAge);
    nlohmann::json payload;
    payload["clip_id"] = clip.clipId();
    payload["event_id"] = clip.eventId().empty() ? nlohmann::json(nullptr)
                                                 : nlohmann::json(clip.eventId());
    payload["media_type"] = domain::to_contract_name(clip.mediaKind());
    payload["media_uri"] = clip_media_uri(clip);
    payload["start_at"] = application::to_iso8601(clip.timeWindow().startTime());
    payload["end_at"] = application::to_iso8601(clip.timeWindow().endTime());
    payload["points_of_interest"] = nlohmann::json::array();
    payload["hash_sha256"] = clip.sha256Hash();
    payload["expires_at"] = application::to_iso8601(expiresAt);
    return payload;
}

} // namespace ods::s4::infrastructure
