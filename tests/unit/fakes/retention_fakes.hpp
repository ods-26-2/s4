#pragma once

// Dubles em memoria do S4.3: permitem testar prazo LGPD e cota de disco sem
// relogio real, sem disco real e sem esperar 7 dias.

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "s4/domain/errors/domain_error.hpp"
#include "s4/domain/repositories/file_storage.hpp"
#include "s4/domain/repositories/purge_log.hpp"
#include "s4/domain/services/clock.hpp"
#include "s4/domain/services/disk_usage_provider.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"

namespace ods::s4::test {

class FakeClock : public domain::IClock {
public:
    explicit FakeClock(TimePoint now) : m_now(now) {}

    [[nodiscard]] TimePoint now() const override { return m_now; }
    void advance(std::chrono::seconds delta) { m_now += delta; }

private:
    TimePoint m_now;
};

// Um "NVMe" de brinquedo: guarda so o TAMANHO de cada arquivo e responde a
// ocupacao a partir deles. Apagar um arquivo aqui libera espaco de verdade na
// ocupacao reportada — exatamente o laco que a cota precisa fechar.
class InMemoryMediaVolume : public domain::IFileStorage, public domain::IDiskUsageProvider {
public:
    InMemoryMediaVolume(std::uintmax_t capacityBytes, std::uintmax_t bytesUsedByOthers = 0)
        : m_capacityBytes(capacityBytes), m_bytesUsedByOthers(bytesUsedByOthers) {}

    void addFile(const std::string& path, std::uintmax_t sizeBytes) { m_sizes[path] = sizeBytes; }
    void failRemovalOf(const std::string& path) { m_undeletable.insert(path); }

    void write(const std::string& path, const std::vector<std::uint8_t>& data) override {
        m_sizes[path] = data.size();
    }

    [[nodiscard]] bool exists(const std::string& path) const override {
        return m_sizes.count(path) != 0;
    }

    [[nodiscard]] std::uintmax_t sizeOf(const std::string& path) const override {
        const auto it = m_sizes.find(path);
        if (it == m_sizes.end()) {
            throw domain::MediaFileNotFoundError("not in fake volume: " + path);
        }
        return it->second;
    }

    void remove(const std::string& path) override {
        if (m_undeletable.count(path) != 0) {
            throw domain::FileOperationError("permission denied: " + path);
        }
        m_sizes.erase(path);
    }

    [[nodiscard]] domain::DiskUsage currentUsage() const override {
        std::uintmax_t used = m_bytesUsedByOthers;
        for (const auto& [_, size] : m_sizes) {
            used += size;
        }
        return domain::DiskUsage(used, m_capacityBytes - used);
    }

private:
    std::uintmax_t m_capacityBytes;
    std::uintmax_t m_bytesUsedByOthers;
    std::map<std::string, std::uintmax_t> m_sizes;
    std::set<std::string> m_undeletable;
};

class InMemoryPurgeLog : public domain::IPurgeLog {
public:
    void record(const domain::PurgeLogEntry& entry) override {
        if (isFull) {
            throw domain::PurgeLogError("fake log is full");
        }
        entries.push_back(entry);
    }

    std::vector<domain::PurgeLogEntry> entries;
    bool isFull{false};
};

// Simula a corrida: a consulta de candidatos devolveu uma foto antiga (clipe
// destravado), mas no banco o clipe ja foi travado para auditoria.
class StaleCandidatesRepository : public infrastructure::InMemoryMediaClipRepository {
public:
    std::vector<domain::MediaClip> findPurgeCandidates() override { return staleCandidates; }

    std::vector<domain::MediaClip> staleCandidates;
};

} // namespace ods::s4::test
