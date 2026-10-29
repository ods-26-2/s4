#pragma once

#include "s4/domain/strategies/retention_strategy.hpp"
#include "s4/domain/value_objects/quota_policy.hpp"

namespace ods::s4::domain {

// Expurgo EMERGENCIAL por espaco: com o NVMe no gatilho (85%), apaga as midias
// nao travadas da mais antiga para a mais nova (FIFO) ate a projecao de
// ocupacao cair abaixo do alvo.
//
// Se o disco esta no gatilho e nao sobra nenhuma midia apagavel (tudo travado
// para auditoria, ou disco tomado por outra coisa), lanca
// DiskQuotaExceededError: nao ha regra de retencao que resolva, e alguem
// precisa ser avisado.
class DiskQuotaFifoStrategy : public IRetentionStrategy {
public:
    explicit DiskQuotaFifoStrategy(QuotaPolicy policy) : m_policy(policy) {}

    [[nodiscard]] PurgeReason reason() const noexcept override {
        return PurgeReason::DiskQuotaExceeded;
    }

    [[nodiscard]] std::vector<PurgeCandidate> selectClipsToPurge(
        const std::vector<PurgeCandidate>& candidates,
        const RetentionContext& context
    ) const override;

private:
    [[nodiscard]] static std::vector<PurgeCandidate> eligibleOldestFirst(
        const std::vector<PurgeCandidate>& candidates
    );
    [[nodiscard]] std::vector<PurgeCandidate> takeUntilTargetIsReached(
        const std::vector<PurgeCandidate>& oldestFirst,
        const DiskUsage& usage
    ) const;

    QuotaPolicy m_policy;
};

} // namespace ods::s4::domain
