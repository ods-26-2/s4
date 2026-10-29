#include "s4/domain/strategies/disk_quota_fifo_strategy.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ods::s4::domain {

namespace {

std::string percent(double ratio) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f%%", ratio * 100.0);
    return text;
}

} // namespace

std::vector<PurgeCandidate> DiskQuotaFifoStrategy::selectClipsToPurge(
    const std::vector<PurgeCandidate>& candidates,
    const RetentionContext& context
) const {
    if (!m_policy.isExceededBy(context.diskUsage)) {
        return {};
    }
    const std::vector<PurgeCandidate> oldestFirst = eligibleOldestFirst(candidates);
    if (oldestFirst.empty()) {
        throw DiskQuotaExceededError(
            "NVMe at " + percent(context.diskUsage.usedRatio()) + " (trigger " +
            percent(m_policy.triggerRatio()) + ") and no unlocked media left to purge"
        );
    }
    return takeUntilTargetIsReached(oldestFirst, context.diskUsage);
}

// stable_sort: clipes criados no mesmo milissegundo mantem a ordem recebida,
// o que deixa a escolha deterministica.
std::vector<PurgeCandidate> DiskQuotaFifoStrategy::eligibleOldestFirst(
    const std::vector<PurgeCandidate>& candidates
) {
    std::vector<PurgeCandidate> eligible;
    for (const auto& candidate : candidates) {
        if (is_eligible_for_purge(candidate.clip)) {
            eligible.push_back(candidate);
        }
    }
    std::stable_sort(eligible.begin(), eligible.end(),
                     [](const PurgeCandidate& left, const PurgeCandidate& right) {
                         return left.clip.createdAt() < right.clip.createdAt();
                     });
    return eligible;
}

// Se nem apagando tudo o alvo e alcancado, devolve todos: e o melhor possivel
// agora, e a proxima varredura (sem candidatos) sinaliza DiskQuotaExceededError.
std::vector<PurgeCandidate> DiskQuotaFifoStrategy::takeUntilTargetIsReached(
    const std::vector<PurgeCandidate>& oldestFirst,
    const DiskUsage& usage
) const {
    std::vector<PurgeCandidate> selected;
    DiskUsage projected = usage;
    for (const auto& candidate : oldestFirst) {
        if (m_policy.isSatisfiedBy(projected)) {
            break;
        }
        selected.push_back(candidate);
        projected = projected.afterFreeing(candidate.sizeBytes);
    }
    return selected;
}

} // namespace ods::s4::domain
