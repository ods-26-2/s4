#include "s4/infrastructure/b3_bus/message_router.hpp"

#include <algorithm>

namespace ods::s4::infrastructure {

MessageRouter::MessageRouter(std::shared_ptr<IEventTransport> transport)
    : m_transport(std::move(transport)) {}

void MessageRouter::register_handler(const std::string& schema, IEventTransport::MessageHandler handler) {
    m_handlers[schema] = std::move(handler);
}

void MessageRouter::start() {
    if (!m_transport) return;
    m_transport->subscribe("ods.s2.event", [this](auto t, auto e){ on_message(t,e); });
    m_transport->subscribe("ods.session.started", [this](auto t, auto e){ on_message(t,e); });
    m_transport->subscribe("ods.s4.capture.command", [this](auto t, auto e){ on_message(t,e); });
    m_transport->subscribe("ods.s4.clip.query", [this](auto t, auto e){ on_message(t,e); });
    m_transport->start();
}

void MessageRouter::stop() {
    if (m_transport) m_transport->stop();
}

void MessageRouter::on_message(const std::string& topic, const B3Envelope& envelope) {
    (void)topic;
    if (is_duplicate(envelope.messageId)) return;
    auto it = m_handlers.find(envelope.schema);
    if (it != m_handlers.end()) {
        it->second(topic, envelope);
    }
}

bool MessageRouter::is_duplicate(const std::string& message_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_cleanup > std::chrono::minutes(10)) {
        m_seen.clear();
        m_last_cleanup = now;
    }
    if (m_seen.find(message_id) != m_seen.end()) return true;
    m_seen.insert(message_id);
    if (m_seen.size() > 100000) {
        m_seen.clear();
    }
    return false;
}

} // namespace ods::s4::infrastructure
