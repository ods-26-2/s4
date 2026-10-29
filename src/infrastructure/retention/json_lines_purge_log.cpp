#include "s4/infrastructure/retention/json_lines_purge_log.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "s4/application/dtos/iso_time_utils.hpp"
#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string to_json_line(const domain::PurgeLogEntry& entry) {
    char ratio[16];
    std::snprintf(ratio, sizeof(ratio), "%.4f", entry.diskUsedRatio);
    std::ostringstream line;
    line << "{\"purged_at\":" << json_string(application::to_iso8601(entry.purgedAt))
         << ",\"clip_id\":" << json_string(entry.clipId)
         << ",\"event_id\":" << json_string(entry.eventId)
         << ",\"file_path\":" << json_string(entry.filePath)
         << ",\"reason\":" << json_string(domain::to_string(entry.reason))
         << ",\"file\":" << json_string(domain::to_string(entry.fileOutcome))
         << ",\"bytes_freed\":" << entry.bytesFreed
         << ",\"disk_used_ratio\":" << ratio << "}\n";
    return line.str();
}

} // namespace

JsonLinesPurgeLog::JsonLinesPurgeLog(std::string path) : m_path(std::move(path)) {}

// Abre, anexa e fecha a cada registro: expurgos sao raros, e assim o arquivo
// pode ser rotacionado (logrotate) sem reiniciar o servico.
void JsonLinesPurgeLog::record(const domain::PurgeLogEntry& entry) {
    const std::string line = to_json_line(entry);
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream out(m_path, std::ios::app);
    out << line;
    out.flush();
    if (!out) {
        throw domain::PurgeLogError("cannot append to purge log " + m_path);
    }
}

} // namespace ods::s4::infrastructure
