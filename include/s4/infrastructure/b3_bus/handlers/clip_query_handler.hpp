#pragma once

#include <memory>
#include "s4/infrastructure/b3_bus/event_transport.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"

namespace ods::s4::infrastructure {

class ClipQueryHandler {
public:
    ClipQueryHandler(std::shared_ptr<domain::IMediaClipRepository> repository,
                     std::shared_ptr<IEventTransport> transport);
    void operator()(const std::string& topic, const B3Envelope& envelope);

private:
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
    std::shared_ptr<IEventTransport> m_transport;
};

} // namespace ods::s4::infrastructure
