#pragma once

#include <chrono>
#include <vector>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/disk_usage.hpp"
#include "s4/domain/value_objects/purge_candidate.hpp"
#include "s4/domain/value_objects/purge_log_entry.hpp"

namespace ods::s4::domain {

// O que uma regra de expurgo precisa saber do mundo para decidir.
struct RetentionContext {
    std::chrono::system_clock::time_point now;
    DiskUsage diskUsage;
};

// Trava de auditoria e expurgo anterior valem para TODA estrategia: uma midia
// travada nunca e apagada, qualquer que seja o motivo.
inline bool is_eligible_for_purge(const MediaClip& clip) noexcept {
    return clip.isRetained() && !clip.isLockedForAudit();
}

// Padrao GoF Strategy (guia §6): troca o algoritmo de expurgo — prazo LGPD ou
// cota de disco FIFO — sem mudar o caso de uso que executa a exclusao.
//
// Estrategias so DECIDEM (dominio puro, sem I/O); apagar arquivo e atualizar o
// SQLite e trabalho do PurgeMediaUseCase.
class IRetentionStrategy {
public:
    virtual ~IRetentionStrategy() = default;

    [[nodiscard]] virtual PurgeReason reason() const noexcept = 0;

    // Subconjunto de "candidates" que deve ser expurgado agora, na ordem em
    // que deve ser apagado. Nunca inclui midia travada ou ja expurgada.
    [[nodiscard]] virtual std::vector<PurgeCandidate> selectClipsToPurge(
        const std::vector<PurgeCandidate>& candidates,
        const RetentionContext& context
    ) const = 0;
};

} // namespace ods::s4::domain
