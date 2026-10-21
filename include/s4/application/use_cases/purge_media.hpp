#pragma once

#include <memory>
#include <string>
#include <vector>

#include "s4/application/dtos/purge_report.hpp"
#include "s4/domain/repositories/file_storage.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"
#include "s4/domain/repositories/purge_log.hpp"
#include "s4/domain/services/clock.hpp"
#include "s4/domain/services/disk_usage_provider.hpp"
#include "s4/domain/strategies/retention_strategy.hpp"

namespace ods::s4::application {

// S4.3 — caso de uso: uma varredura de expurgo.
//
// As estrategias decidem O QUE sai; este caso de uso faz sair, em uma ordem
// que sobrevive a queda no meio do caminho:
//   1. apaga o arquivo do NVMe;
//   2. marca o clipe como expurgado no SQLite (is_retained = false);
//   3. anexa a linha no log de expurgo.
// Se o processo cair entre 1 e 2, a proxima varredura encontra o registro sem
// arquivo e so conclui os passos 2 e 3. Na ordem inversa sobraria um arquivo
// que nenhum registro aponta — uma midia orfa que nunca mais seria apagada.
class PurgeMediaUseCase {
public:
    // As estrategias rodam NA ORDEM dada. A composicao padrao e prazo LGPD
    // primeiro e cota depois: o que ja venceu sai antes, e a cota so toca em
    // midia dentro do prazo se ainda faltar espaco.
    PurgeMediaUseCase(
        std::shared_ptr<domain::IMediaClipRepository> repository,
        std::shared_ptr<domain::IFileStorage> fileStorage,
        std::shared_ptr<domain::IDiskUsageProvider> diskUsage,
        std::shared_ptr<domain::IPurgeLog> purgeLog,
        std::shared_ptr<domain::IClock> clock,
        std::vector<std::shared_ptr<const domain::IRetentionStrategy>> strategies
    );

    // Lanca DiskQuotaExceededError quando o disco segue no gatilho sem nada
    // apagavel — depois de ja ter expurgado (e registrado no log) o que as
    // estrategias anteriores escolheram.
    PurgeReport execute();

private:
    [[nodiscard]] std::vector<domain::PurgeCandidate> loadCandidates() const;
    [[nodiscard]] std::uintmax_t measuredSize(const std::string& path) const;
    void applyStrategy(
        const domain::IRetentionStrategy& strategy,
        std::vector<domain::PurgeCandidate>& remaining,
        PurgeReport& report
    );
    void purge(
        const domain::PurgeCandidate& candidate,
        domain::PurgeReason reason,
        const domain::RetentionContext& context,
        PurgeReport& report
    );
    [[nodiscard]] domain::FileOutcome releaseFile(const domain::MediaClip& clip);
    [[nodiscard]] bool isFileSharedWithRetainedClip(const domain::MediaClip& clip) const;
    void recordInLog(const domain::PurgeLogEntry& entry, PurgeReport& report);

    std::shared_ptr<domain::IMediaClipRepository> m_repository;
    std::shared_ptr<domain::IFileStorage> m_fileStorage;
    std::shared_ptr<domain::IDiskUsageProvider> m_diskUsage;
    std::shared_ptr<domain::IPurgeLog> m_purgeLog;
    std::shared_ptr<domain::IClock> m_clock;
    std::vector<std::shared_ptr<const domain::IRetentionStrategy>> m_strategies;
};

} // namespace ods::s4::application
