#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ods::s4::domain {

// Por que uma midia foi expurgada. Vai para o log: e a resposta a "por que
// esta evidencia nao existe mais?" numa auditoria.
enum class PurgeReason {
    RetentionPeriodExpired,  // prazo LGPD vencido
    DiskQuotaExceeded,       // emergencia de disco, descarte FIFO
};

// O que aconteceu com o ARQUIVO; o registro no SQLite sempre fica, marcado com
// is_retained = false, para que o evento continue dizendo que teve midia.
enum class FileOutcome {
    Deleted,             // arquivo apagado do NVMe
    AlreadyMissing,      // arquivo ja nao existia (ex.: queda no meio de um expurgo)
    KeptForOtherClip,    // outro clipe retido aponta para o mesmo arquivo
};

inline const char* to_string(PurgeReason reason) noexcept {
    return reason == PurgeReason::RetentionPeriodExpired ? "lgpd_retention_expired" : "disk_quota_fifo";
}

inline const char* to_string(FileOutcome outcome) noexcept {
    switch (outcome) {
        case FileOutcome::Deleted:
            return "deleted";
        case FileOutcome::AlreadyMissing:
            return "already_missing";
        case FileOutcome::KeptForOtherClip:
            return "kept_for_other_clip";
    }
    return "unknown";
}

// Uma linha do "Log de Expurgo" (saida do S4.3 no guia).
struct PurgeLogEntry {
    std::string clipId;
    std::string eventId;
    std::string filePath;
    PurgeReason reason{PurgeReason::RetentionPeriodExpired};
    FileOutcome fileOutcome{FileOutcome::Deleted};
    std::uintmax_t bytesFreed{0};
    // Ocupacao do disco no momento em que a estrategia decidiu expurgar.
    double diskUsedRatio{0.0};
    std::chrono::system_clock::time_point purgedAt{};
};

} // namespace ods::s4::domain
