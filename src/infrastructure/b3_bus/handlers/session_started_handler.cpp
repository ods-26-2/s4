#include "s4/infrastructure/b3_bus/handlers/session_started_handler.hpp"
#include "s4/domain/value_objects/session_started.hpp"
#include "s4/domain/value_objects/capture_window.hpp"
#include <nlohmann/json.hpp>

namespace ods::s4::infrastructure {

SessionStartedHandler::SessionStartedHandler(std::shared_ptr<RingBufferRAMFacade> buffer)
    : m_buffer(std::move(buffer)) {}

void SessionStartedHandler::operator()(const std::string& topic, const B3Envelope& envelope) {
    (void)topic;
    try {
        const auto& p = envelope.payload;
        ods::s4::domain::SessionStarted marker;
        if (envelope.sessionId) marker.sessionId = *envelope.sessionId;
        else marker.sessionId = p.value("session_id", "");
        const std::string mode = p.value("mode", "LIVE");
        marker.mode = (mode == "REPLAY") ? ods::s4::domain::SessionMode::Replay : ods::s4::domain::SessionMode::Live;
        const std::string reason = p.value("reason", "UNSPECIFIED");
        if (reason == "START") marker.reason = ods::s4::domain::SessionChangeReason::Start;
        else if (reason == "SEEK") marker.reason = ods::s4::domain::SessionChangeReason::Seek;
        else if (reason == "LOOP") marker.reason = ods::s4::domain::SessionChangeReason::Loop;
        else if (reason == "RESTART") marker.reason = ods::s4::domain::SessionChangeReason::Restart;
        else if (reason == "RECONNECT") marker.reason = ods::s4::domain::SessionChangeReason::Reconnect;
        else marker.reason = ods::s4::domain::SessionChangeReason::Unspecified;
        if (p.contains("sources") && p["sources"].is_array()) {
            for (const auto& s : p["sources"]) {
                if (s.is_string()) marker.sources.push_back(s.get<std::string>());
            }
        }
        if (p.contains("started_at")) {
            // Parse ISO8601 roughly - for now store as 0, buffer can use current time
            (void)p["started_at"];
        }
        if (m_buffer) {
            m_buffer->onSessionStarted(marker);
        }
    } catch (...) {
    }
}

} // namespace ods::s4::infrastructure
