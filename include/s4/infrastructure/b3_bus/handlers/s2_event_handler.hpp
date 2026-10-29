#pragma once

#include <memory>
#include "s4/infrastructure/b3_bus/event_transport.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"

namespace ods::s4::infrastructure {

class S2EventHandler {
public:
    S2EventHandler(std::shared_ptr<application::ExtractClipUseCase> extractClip,
                   std::shared_ptr<domain::IMediaClipRepository> repository,
                   std::shared_ptr<IEventTransport> transport,
                   std::string sourceId,
                   std::string mediaDir);
    void operator()(const std::string& topic, const B3Envelope& envelope);

private:
    std::shared_ptr<application::ExtractClipUseCase> m_extractClip;
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
    std::shared_ptr<IEventTransport> m_transport;
    std::string m_sourceId;
    std::string m_mediaDir;
};

} // namespace ods::s4::infrastructure
