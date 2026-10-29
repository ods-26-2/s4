#pragma once

#include <string>
#include <functional>
#include <memory>
#include "s4/infrastructure/b3_bus/envelope.hpp"

namespace ods::s4::infrastructure {

class IEventTransport {
public:
    using MessageHandler = std::function<void(const std::string& topic, const B3Envelope& envelope)>;
    virtual ~IEventTransport() = default;
    virtual void subscribe(const std::string& topic, MessageHandler handler) = 0;
    virtual void publish(const std::string& topic, const B3Envelope& envelope) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

} // namespace ods::s4::infrastructure
