#pragma once

#include <string>
#include <functional>

namespace ods::s4::infrastructure {

struct B3EventEnvelope {
    std::string eventId;
    std::string eventType;
    std::string origin;
    std::string timestampIso;
    std::string payloadJson;
};

class B3EventConsumer {
public:
    using EventHandler = std::function<void(const B3EventEnvelope&)>;

    virtual ~B3EventConsumer() = default;

    virtual void subscribe(const std::string& topic, EventHandler handler) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};

} // namespace ods::s4::infrastructure
