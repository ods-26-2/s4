#pragma once

#include "s4/domain/strategies/retention_strategy.hpp"
#include "s4/domain/value_objects/retention_policy.hpp"

namespace ods::s4::domain {

// Expurgo por prazo (LGPD, minimizacao de dados): toda midia nao travada cujo
// prazo de guarda venceu, independentemente de quanto disco sobra.
class LgpdRetentionStrategy : public IRetentionStrategy {
public:
    explicit LgpdRetentionStrategy(RetentionPolicy policy) : m_policy(policy) {}

    [[nodiscard]] PurgeReason reason() const noexcept override {
        return PurgeReason::RetentionPeriodExpired;
    }

    [[nodiscard]] std::vector<PurgeCandidate> selectClipsToPurge(
        const std::vector<PurgeCandidate>& candidates,
        const RetentionContext& context
    ) const override;

private:
    RetentionPolicy m_policy;
};

} // namespace ods::s4::domain
