#pragma once

#include <optional>
#include <string>
#include <vector>
#include "s4/domain/entities/media_clip.hpp"

namespace ods::s4::domain {

class IMediaClipRepository {
public:
    virtual ~IMediaClipRepository() = default;

    virtual void save(const MediaClip& clip) = 0;
    virtual std::optional<MediaClip> findById(const std::string& clipId) = 0;
    virtual std::vector<MediaClip> findByEventId(const std::string& eventId) = 0;
    virtual std::vector<MediaClip> findPurgeCandidates() = 0;
    virtual void update(const MediaClip& clip) = 0;
    virtual void remove(const std::string& clipId) = 0;
};

} // namespace ods::s4::domain
