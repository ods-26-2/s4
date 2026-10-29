#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "s4/domain/value_objects/disk_usage.hpp"
#include "s4/domain/value_objects/purge_log_entry.hpp"

namespace ods::s4::application {

// Uma midia que devia ter sido expurgada e nao foi (arquivo sem permissao,
// SQLite indisponivel...). As demais continuam sendo expurgadas.
struct PurgeFailure {
    std::string clipId;
    std::string filePath;
    std::string message;
};

// Resultado de UMA varredura do PurgeMediaUseCase.
struct PurgeReport {
    std::chrono::system_clock::time_point startedAt;
    domain::DiskUsage diskUsageBefore;
    domain::DiskUsage diskUsageAfter;
    std::vector<domain::PurgeLogEntry> purged;
    std::vector<PurgeFailure> failures;
    // Escolhidos pela estrategia, mas travados para auditoria (ou expurgados
    // por outro caminho) entre a consulta de candidatos e a exclusao.
    std::size_t skippedNoLongerEligible{0};

    [[nodiscard]] std::uintmax_t bytesFreed() const {
        std::uintmax_t total = 0;
        for (const auto& entry : purged) {
            total += entry.bytesFreed;
        }
        return total;
    }

    [[nodiscard]] std::size_t countPurgedBy(domain::PurgeReason reason) const {
        std::size_t count = 0;
        for (const auto& entry : purged) {
            count += entry.reason == reason ? 1 : 0;
        }
        return count;
    }
};

} // namespace ods::s4::application
