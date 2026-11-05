#include "s4/infrastructure/b3_bus/handlers/clip_query_handler.hpp"
#include "s4/infrastructure/b3_bus/clip_payload.hpp"
#include <nlohmann/json.hpp>
#include <chrono>

namespace ods::s4::infrastructure {

ClipQueryHandler::ClipQueryHandler(std::shared_ptr<domain::IMediaClipRepository> repository,
                                   std::shared_ptr<IEventTransport> transport)
    : m_repository(std::move(repository)), m_transport(std::move(transport)) {}

void ClipQueryHandler::operator()(const std::string& topic, const B3Envelope& envelope) {
    (void)topic;
    if (envelope.messageType != "query") return;
    try {
        const auto& p = envelope.payload;
        bool hasClipId = p.contains("clip_id") && !p["clip_id"].is_null();
        bool hasEventId = p.contains("event_id") && !p["event_id"].is_null();
        if (!hasClipId && !hasEventId) {
            if (m_transport && envelope.messageId.size()) {
                B3Envelope replyErr;
                replyErr.messageType = "reply";
                replyErr.messageId = generate_message_id();
                replyErr.schema = "ods.s4.clip.query.reply";
                replyErr.schemaVersion = "1.0";
                replyErr.producer = "S4";
                replyErr.publishedAt = now_iso8601_utc();
                replyErr.correlationId = envelope.messageId;
                nlohmann::json rp;
                rp["status"] = "ERROR";
                rp["error"] = {{"code", "INVALID_QUERY"}, {"message", "missing clip_id or event_id"}};
                replyErr.payload = rp;
                m_transport->publish("ods.s4.clip.query.reply", replyErr);
            }
            return;
        }
        // "Lista vazia se expirou" (6c): clipe expurgado, ou vencido e ainda
        // nao varrido pelo daemon, nao aparece na resposta.
        const auto now = std::chrono::system_clock::now();
        nlohmann::json clips = nlohmann::json::array();
        if (hasClipId && m_repository) {
            auto c = m_repository->findById(p["clip_id"].get<std::string>());
            if (c && c->isAvailableAt(now)) {
                clips.push_back(clip_payload(*c));
            }
        } else if (hasEventId && m_repository) {
            for (const auto& c : m_repository->findByEventId(p["event_id"].get<std::string>())) {
                if (c.isAvailableAt(now)) {
                    clips.push_back(clip_payload(c));
                }
            }
        }
        if (m_transport && envelope.messageId.size()) {
            B3Envelope reply;
            reply.messageType = "reply";
            reply.messageId = generate_message_id();
            reply.schema = "ods.s4.clip.query.reply";
            reply.schemaVersion = "1.0";
            reply.producer = "S4";
            reply.publishedAt = now_iso8601_utc();
            reply.correlationId = envelope.messageId;
            nlohmann::json rp;
            rp["status"] = "OK";
            rp["clips"] = clips;
            reply.payload = rp;
            m_transport->publish("ods.s4.clip.query.reply", reply);
        }
    } catch (const std::exception& e) {
        if (m_transport && envelope.messageId.size()) {
            B3Envelope replyErr;
            replyErr.messageType = "reply";
            replyErr.messageId = generate_message_id();
            replyErr.schema = "ods.s4.clip.query.reply";
            replyErr.schemaVersion = "1.0";
            replyErr.producer = "S4";
            replyErr.publishedAt = now_iso8601_utc();
            replyErr.correlationId = envelope.messageId;
            nlohmann::json rp;
            rp["status"] = "ERROR";
            rp["error"] = {{"code", "INVALID_QUERY"}, {"message", e.what()}};
            replyErr.payload = rp;
            m_transport->publish("ods.s4.clip.query.reply", replyErr);
        }
    }
}

} // namespace ods::s4::infrastructure
