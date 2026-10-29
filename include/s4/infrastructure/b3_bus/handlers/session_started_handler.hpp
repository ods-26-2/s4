#pragma once

#include <memory>
#include "s4/infrastructure/b3_bus/event_transport.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"

namespace ods::s4::infrastructure {

class SessionStartedHandler {
public:
    SessionStartedHandler(std::shared_ptr<RingBufferRAMFacade> buffer);
    void operator()(const std::string& topic, const B3Envelope& envelope);

private:
    std::shared_ptr<RingBufferRAMFacade> m_buffer;
};

} // namespace ods::s4::infrastructure
