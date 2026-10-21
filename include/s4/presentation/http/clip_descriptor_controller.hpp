#pragma once

#include <memory>
#include <string>
#include <optional>
#include "s4/application/dtos/clip_descriptor_dto.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"

namespace ods::s4::presentation {

class ClipDescriptorController {
public:
    explicit ClipDescriptorController(std::shared_ptr<domain::IMediaClipRepository> repository)
        : m_repository(std::move(repository)) {}

    [[nodiscard]] std::optional<application::ClipDescriptorDTO> getClipDescriptor(const std::string& clipId) const;

private:
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
};

} // namespace ods::s4::presentation
