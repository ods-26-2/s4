#pragma once

#include <mutex>
#include <string>

#include "s4/domain/repositories/purge_log.hpp"

namespace ods::s4::infrastructure {

// Log de Expurgo em JSON Lines: um objeto JSON por linha, so anexado.
//
// Por que um arquivo e nao uma tabela no SQLite do S4.2: o daemon escreveria
// por uma segunda conexao no mesmo banco, e uma escrita dele na hora errada
// devolveria SQLITE_BUSY para o POST /api/v1/events. Um arquivo proprio nao
// disputa trava com ninguem, e `grep`/`jq` bastam para auditar.
//
// Exemplo de linha:
//   {"purged_at":"2026-09-29T03:00:00.000Z","clip_id":"clip-...","event_id":"evt-1",
//    "file_path":"/data/media/evt-1.mp4","reason":"lgpd_retention_expired",
//    "file":"deleted","bytes_freed":1048576,"disk_used_ratio":0.4210}
class JsonLinesPurgeLog : public domain::IPurgeLog {
public:
    explicit JsonLinesPurgeLog(std::string path);

    void record(const domain::PurgeLogEntry& entry) override;

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    std::mutex m_mutex;
};

} // namespace ods::s4::infrastructure
