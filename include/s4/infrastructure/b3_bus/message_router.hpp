#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include "s4/infrastructure/b3_bus/event_transport.hpp"

namespace ods::s4::infrastructure {

class MessageRouter {
public:
    explicit MessageRouter(std::shared_ptr<IEventTransport> transport);
    void register_handler(const std::string& schema, IEventTransport::MessageHandler handler);
    void start();
    void stop();

private:
    void on_message(const std::string& topic, const B3Envelope& envelope);
    bool is_duplicate(const std::string& message_id);

    std::shared_ptr<IEventTransport> m_transport;
    std::unordered_map<std::string, IEventTransport::MessageHandler> m_handlers;
    std::unordered_set<std::string> m_seen;
    std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_last_cleanup{std::chrono::steady_clock::now()};
};

} // namespace ods::s4::infrastructure
