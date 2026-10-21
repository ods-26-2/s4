#pragma once

#include "s4/domain/repositories/media_clip_repository.hpp"
#include <string>
#include <memory>

struct sqlite3;

namespace ods::s4::infrastructure {

class SQLiteMediaClipRepository : public domain::IMediaClipRepository {
public:
    explicit SQLiteMediaClipRepository(const std::string& dbPath);
    ~SQLiteMediaClipRepository() override;

    void save(const domain::MediaClip& clip) override;
    std::optional<domain::MediaClip> findById(const std::string& clipId) override;
    std::vector<domain::MediaClip> findByEventId(const std::string& eventId) override;
    std::vector<domain::MediaClip> findPurgeCandidates() override;
    void update(const domain::MediaClip& clip) override;
    void remove(const std::string& clipId) override;

private:
    void initSchema();

    std::string m_dbPath;
    sqlite3* m_db{nullptr};
};

} // namespace ods::s4::infrastructure
