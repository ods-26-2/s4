#pragma once

#include "s4/domain/repositories/media_clip_repository.hpp"
#include <unordered_map>
#include <mutex>

namespace ods::s4::infrastructure {

class InMemoryMediaClipRepository : public domain::IMediaClipRepository {
public:
    void save(const domain::MediaClip& clip) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storage.insert_or_assign(clip.clipId(), clip);
    }

    std::optional<domain::MediaClip> findById(const std::string& clipId) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_storage.find(clipId);
        if (it != m_storage.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<domain::MediaClip> findByEventId(const std::string& eventId) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<domain::MediaClip> results;
        for (const auto& [_, clip] : m_storage) {
            if (clip.eventId() == eventId) {
                results.push_back(clip);
            }
        }
        return results;
    }

    std::vector<domain::MediaClip> findPurgeCandidates() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<domain::MediaClip> candidates;
        for (const auto& [_, clip] : m_storage) {
            if (clip.isRetained() && !clip.isLockedForAudit()) {
                candidates.push_back(clip);
            }
        }
        return candidates;
    }

    void update(const domain::MediaClip& clip) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storage.insert_or_assign(clip.clipId(), clip);
    }

    void remove(const std::string& clipId) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storage.erase(clipId);
    }

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, domain::MediaClip> m_storage;
};

} // namespace ods::s4::infrastructure
