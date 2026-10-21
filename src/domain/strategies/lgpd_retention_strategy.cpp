#include "s4/domain/strategies/lgpd_retention_strategy.hpp"

namespace ods::s4::domain {

std::vector<PurgeCandidate> LgpdRetentionStrategy::selectClipsToPurge(
    const std::vector<PurgeCandidate>& candidates,
    const RetentionContext& context
) const {
    std::vector<PurgeCandidate> expired;
    for (const auto& candidate : candidates) {
        if (is_eligible_for_purge(candidate.clip) && m_policy.hasExpired(candidate.clip, context.now)) {
            expired.push_back(candidate);
        }
    }
    return expired;
}

} // namespace ods::s4::domain
