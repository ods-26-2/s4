#pragma once

#include <algorithm>
#include <chrono>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/errors/domain_error.hpp"
#include "s4/domain/value_objects/media_kind.hpp"

namespace ods::s4::domain {

// Prazo de guarda (LGPD) por tipo de midia.
//
// A idade conta a partir de createdAt, o instante em que a evidencia foi
// gravada no NVMe. Um clipe com exatamente o prazo de idade ja esta vencido:
// "apos 7 dias" significa que no setimo dia ele deixa de existir.
//
// O clipe guarda o expires_at que foi PUBLICADO quando nasceu. O vencimento
// efetivo e o menor entre esse e o da politica de agora: se o operador
// encurtar o prazo, o clipe antigo vence pelo prazo novo (a LGPD manda guardar
// o minimo); se alongar, a promessa publicada continua valendo, porque o
// contrato diz que depois de expires_at o media_uri nao resolve.
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

    // Vencimento de um clipe que esta nascendo agora.
    [[nodiscard]] TimePoint expiryFor(MediaKind kind, TimePoint createdAt) const noexcept {
        return createdAt + maxAgeFor(kind);
    }

    [[nodiscard]] TimePoint expiryOf(const MediaClip& clip) const noexcept {
        const TimePoint byPolicy = expiryFor(clip.mediaKind(), clip.createdAt());
        return clip.expiresAt().has_value() ? std::min(byPolicy, *clip.expiresAt()) : byPolicy;
    }

    [[nodiscard]] bool hasExpired(const MediaClip& clip, TimePoint now) const {
        return now >= expiryOf(clip);
    }

private:
    Duration m_clipMaxAge;
    Duration m_snapshotMaxAge;
};

} // namespace ods::s4::domain
