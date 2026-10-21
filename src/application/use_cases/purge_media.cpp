#include "s4/application/use_cases/purge_media.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::application {

namespace {

void remove_candidate(std::vector<domain::PurgeCandidate>& candidates, const std::string& clipId) {
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [&clipId](const domain::PurgeCandidate& candidate) {
                           return candidate.clip.clipId() == clipId;
                       }),
        candidates.end());
}

domain::PurgeLogEntry make_entry(
    const domain::MediaClip& clip,
    domain::PurgeReason reason,
    domain::FileOutcome outcome,
    std::uintmax_t bytesFreed,
    const domain::RetentionContext& context
) {
    domain::PurgeLogEntry entry;
    entry.clipId = clip.clipId();
    entry.eventId = clip.eventId();
    entry.filePath = clip.filePath();
    entry.reason = reason;
    entry.fileOutcome = outcome;
    entry.bytesFreed = bytesFreed;
    entry.diskUsedRatio = context.diskUsage.usedRatio();
    entry.purgedAt = context.now;
    return entry;
}

} // namespace

PurgeMediaUseCase::PurgeMediaUseCase(
    std::shared_ptr<domain::IMediaClipRepository> repository,
    std::shared_ptr<domain::IFileStorage> fileStorage,
    std::shared_ptr<domain::IDiskUsageProvider> diskUsage,
    std::shared_ptr<domain::IPurgeLog> purgeLog,
    std::shared_ptr<domain::IClock> clock,
    std::vector<std::shared_ptr<const domain::IRetentionStrategy>> strategies
) : m_repository(std::move(repository)),
    m_fileStorage(std::move(fileStorage)),
    m_diskUsage(std::move(diskUsage)),
    m_purgeLog(std::move(purgeLog)),
    m_clock(std::move(clock)),
    m_strategies(std::move(strategies)) {}

PurgeReport PurgeMediaUseCase::execute() {
    const domain::DiskUsage usageBefore = m_diskUsage->currentUsage();
    PurgeReport report{m_clock->now(), usageBefore, usageBefore, {}, {}, 0};
    std::vector<domain::PurgeCandidate> remaining = loadCandidates();
    for (const auto& strategy : m_strategies) {
        applyStrategy(*strategy, remaining, report);
    }
    report.diskUsageAfter = m_diskUsage->currentUsage();
    return report;
}

std::vector<domain::PurgeCandidate> PurgeMediaUseCase::loadCandidates() const {
    std::vector<domain::PurgeCandidate> candidates;
    for (auto& clip : m_repository->findPurgeCandidates()) {
        const std::uintmax_t size = measuredSize(clip.filePath());
        candidates.push_back(domain::PurgeCandidate{std::move(clip), size});
    }
    return candidates;
}

// Arquivo ausente ou ilegivel pesa 0: a estrategia de cota nao conta com um
// espaco que apaga-lo nao liberaria.
std::uintmax_t PurgeMediaUseCase::measuredSize(const std::string& path) const {
    try {
        return m_fileStorage->exists(path) ? m_fileStorage->sizeOf(path) : 0;
    } catch (const domain::ApplicationError&) {
        return 0;
    }
}

// O disco e medido de novo a cada estrategia: o que a anterior apagou ja
// liberou espaco, e a cota precisa decidir sobre o disco de agora.
void PurgeMediaUseCase::applyStrategy(
    const domain::IRetentionStrategy& strategy,
    std::vector<domain::PurgeCandidate>& remaining,
    PurgeReport& report
) {
    const domain::RetentionContext context{m_clock->now(), m_diskUsage->currentUsage()};
    const std::vector<domain::PurgeCandidate> selected = strategy.selectClipsToPurge(remaining, context);
    for (const auto& candidate : selected) {
        purge(candidate, strategy.reason(), context, report);
        remove_candidate(remaining, candidate.clip.clipId());
    }
}

void PurgeMediaUseCase::purge(
    const domain::PurgeCandidate& candidate,
    domain::PurgeReason reason,
    const domain::RetentionContext& context,
    PurgeReport& report
) {
    // Relido AGORA: o clipe pode ter sido travado para auditoria depois da
    // consulta de candidatos, e evidencia travada nunca e apagada.
    const std::optional<domain::MediaClip> current = m_repository->findById(candidate.clip.clipId());
    if (!current.has_value() || !domain::is_eligible_for_purge(*current)) {
        ++report.skippedNoLongerEligible;
        return;
    }
    try {
        const domain::FileOutcome outcome = releaseFile(*current);
        domain::MediaClip purged = *current;
        purged.markAsPurged();
        m_repository->update(purged);
        const std::uintmax_t bytesFreed = outcome == domain::FileOutcome::Deleted ? candidate.sizeBytes : 0;
        const domain::PurgeLogEntry entry = make_entry(purged, reason, outcome, bytesFreed, context);
        report.purged.push_back(entry);
        recordInLog(entry, report);
    } catch (const domain::ApplicationError& error) {
        report.failures.push_back(PurgeFailure{current->clipId(), current->filePath(), error.what()});
    }
}

domain::FileOutcome PurgeMediaUseCase::releaseFile(const domain::MediaClip& clip) {
    if (!m_fileStorage->exists(clip.filePath())) {
        return domain::FileOutcome::AlreadyMissing;
    }
    if (isFileSharedWithRetainedClip(clip)) {
        return domain::FileOutcome::KeptForOtherClip;
    }
    m_fileStorage->remove(clip.filePath());
    return domain::FileOutcome::Deleted;
}

// O S4.2 nomeia o arquivo pelo event_id: o mesmo evento enviado duas vezes gera
// dois clipes apontando para UM arquivo. Apagar pelo clipe antigo destruiria a
// midia do novo — que pode estar travado para auditoria.
bool PurgeMediaUseCase::isFileSharedWithRetainedClip(const domain::MediaClip& clip) const {
    for (const auto& sibling : m_repository->findByEventId(clip.eventId())) {
        const bool isOtherClip = sibling.clipId() != clip.clipId();
        if (isOtherClip && sibling.isRetained() && sibling.filePath() == clip.filePath()) {
            return true;
        }
    }
    return false;
}

// O expurgo ja aconteceu; se a linha do log nao pode ser gravada (ex.: disco
// cheio), isso vira uma falha visivel no relatorio, nunca um silencio.
void PurgeMediaUseCase::recordInLog(const domain::PurgeLogEntry& entry, PurgeReport& report) {
    try {
        m_purgeLog->record(entry);
    } catch (const domain::PurgeLogError& error) {
        report.failures.push_back(PurgeFailure{
            entry.clipId, entry.filePath, std::string("purged, but not logged: ") + error.what()
        });
    }
}

} // namespace ods::s4::application
