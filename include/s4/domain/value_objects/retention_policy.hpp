#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

// A atividade pede "definir por quanto tempo CADA TIPO de midia e guardado":
// um snapshot (.jpg) e um clipe (.mp4) podem ter prazos diferentes.
enum class MediaKind { VideoClip, Snapshot };

// O tipo sai da extensao do arquivo, que e como o S4.2 ja os distingue
// (.mp4 para clipe, .jpg para snapshot).
inline MediaKind media_kind_of(const std::string& filePath) {
    const std::size_t dot = filePath.rfind('.');
    if (dot == std::string::npos) {
        return MediaKind::VideoClip;
    }
    std::string extension = filePath.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isImage = extension == "jpg" || extension == "jpeg" || extension == "png";
    return isImage ? MediaKind::Snapshot : MediaKind::VideoClip;
}

// Prazo de guarda (LGPD) por tipo de midia.
//
// A idade conta a partir de createdAt, o instante em que a evidencia foi
// gravada no NVMe. Um clipe com exatamente o prazo de idade ja esta vencido:
// "apos 7 dias" significa que no setimo dia ele deixa de existir.
class RetentionPolicy {
public:
    using Duration = std::chrono::seconds;
    using TimePoint = std::chrono::system_clock::time_point;

    static constexpr Duration kLgpdDefaultMaxAge = std::chrono::hours(24 * 7);

    explicit RetentionPolicy(
        Duration clipMaxAge = kLgpdDefaultMaxAge,
        Duration snapshotMaxAge = kLgpdDefaultMaxAge
    ) : m_clipMaxAge(clipMaxAge), m_snapshotMaxAge(snapshotMaxAge) {
        if (clipMaxAge <= Duration::zero() || snapshotMaxAge <= Duration::zero()) {
            throw InvalidRetentionPolicyError();
        }
    }

    [[nodiscard]] Duration maxAgeFor(MediaKind kind) const noexcept {
        return kind == MediaKind::Snapshot ? m_snapshotMaxAge : m_clipMaxAge;
    }

    [[nodiscard]] bool hasExpired(const MediaClip& clip, TimePoint now) const {
        return now - clip.createdAt() >= maxAgeFor(media_kind_of(clip.filePath()));
    }

private:
    Duration m_clipMaxAge;
    Duration m_snapshotMaxAge;
};

} // namespace ods::s4::domain
