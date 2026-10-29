#include "s4/infrastructure/b3_bus/handlers/capture_command_handler.hpp"
#include "s4/domain/value_objects/capture_window.hpp"
#include "s4/domain/errors/domain_error.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#ifdef _WIN32
#include <rpc.h>
#else
#include <uuid/uuid.h>
#endif

namespace ods::s4::infrastructure {

namespace {
std::string gen_clip_id() {
#ifdef _WIN32
    UUID uuid;
    UuidCreate(&uuid);
    RPC_CSTR str;
    UuidToStringA(&uuid, &str);
    std::string s(reinterpret_cast<char*>(str));
    RpcStringFreeA(&str);
    return s;
#else
    uuid_t uuid;
    char buffer[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, buffer);
    return std::string(buffer);
#endif
}
}

CaptureCommandHandler::CaptureCommandHandler(std::shared_ptr<application::ExtractClipUseCase> extractClip,
                                             std::shared_ptr<domain::IMediaClipRepository> repository,
                                             std::shared_ptr<IEventTransport> transport,
                                             std::string sourceId,
                                             std::string mediaDir)
    : m_extractClip(std::move(extractClip)),
      m_repository(std::move(repository)),
      m_transport(std::move(transport)),
      m_sourceId(std::move(sourceId)),
      m_mediaDir(std::move(mediaDir)) {}

void CaptureCommandHandler::operator()(const std::string& topic, const B3Envelope& envelope) {
    (void)topic;
    if (envelope.messageType != "command") return;
    try {
        const auto& p = envelope.payload;
        const std::string sourceId = p.value("source_id", m_sourceId);
        const std::string mediaType = p.value("media_type", "CLIP");
        std::string eventId;
        if (p.contains("event_id") && !p["event_id"].is_null()) eventId = p["event_id"].get<std::string>();
        std::string clipId = gen_clip_id();

        // Reply immediately
        if (m_transport) {
            B3Envelope reply;
            reply.messageType = "reply";
            reply.messageId = generate_message_id();
            reply.schema = "ods.s4.capture.reply";
            reply.schemaVersion = "1.0";
            reply.producer = "S4";
            reply.publishedAt = now_iso8601_utc();
            if (envelope.messageId.size()) reply.correlationId = envelope.messageId;
            nlohmann::json rp;
            rp["status"] = "OK";
            rp["clip_id"] = clipId;
            reply.payload = rp;
            m_transport->publish("ods.s4.capture.reply", reply);
        }

        if (!m_extractClip || !m_extractClip->supportsCaptureWindows()) return;
        auto cb = m_extractClip->captureBuffer();
        if (!cb) return;
        std::filesystem::create_directories(m_mediaDir);
        std::string out = (std::filesystem::path(m_mediaDir) / (clipId + ".mp4")).string();

        try {
            domain::Nanoseconds ts = cb->newestCaptureTsNs();
            domain::CaptureWindow window = domain::CaptureWindow::around(ts, 0.5, 0.5);
            if (!(mediaType == "SNAPSHOT")) {
                window = domain::CaptureWindow::around(ts, 5.0, 5.0);
            }
            auto clip = m_extractClip->executeCaptureWindow(eventId.empty() ? clipId : eventId, window, out, 10.0);
            B3Envelope outEnv;
            outEnv.messageType = "event";
            outEnv.messageId = generate_message_id();
            outEnv.schema = "ods.s4.clip";
            outEnv.schemaVersion = "1.0";
            outEnv.producer = "S4";
            outEnv.publishedAt = now_iso8601_utc();
            outEnv.sourceId = sourceId;
            nlohmann::json payload;
            payload["clip_id"] = clip.clipId();
            payload["event_id"] = eventId.empty() ? nullptr : nlohmann::json(eventId);
            payload["media_type"] = mediaType;
            payload["media_uri"] = "s4://clips/" + clipId + ".mp4";
            payload["start_at"] = now_iso8601_utc();
            payload["end_at"] = now_iso8601_utc();
            payload["points_of_interest"] = nlohmann::json::array();
            payload["hash_sha256"] = clip.sha256Hash();
            payload["expires_at"] = now_iso8601_utc();
            outEnv.payload = payload;
            if (m_transport) m_transport->publish("ods.s4.clip", outEnv);
        } catch (const domain::OutOfBufferError& e) {
            if (m_transport && envelope.messageId.size()) {
                B3Envelope replyErr;
                replyErr.messageType = "reply";
                replyErr.messageId = generate_message_id();
                replyErr.schema = "ods.s4.capture.reply";
                replyErr.schemaVersion = "1.0";
                replyErr.producer = "S4";
                replyErr.publishedAt = now_iso8601_utc();
                replyErr.correlationId = envelope.messageId;
                nlohmann::json rp;
                rp["status"] = "ERROR";
                rp["error"] = {{"code", "OUT_OF_BUFFER"}, {"message", e.what()}};
                replyErr.payload = rp;
                m_transport->publish("ods.s4.capture.reply", replyErr);
            }
        } catch (const std::exception& e) {
            if (m_transport && envelope.messageId.size()) {
                B3Envelope replyErr;
                replyErr.messageType = "reply";
                replyErr.messageId = generate_message_id();
                replyErr.schema = "ods.s4.capture.reply";
                replyErr.schemaVersion = "1.0";
                replyErr.producer = "S4";
                replyErr.publishedAt = now_iso8601_utc();
                replyErr.correlationId = envelope.messageId;
                nlohmann::json rp;
                rp["status"] = "ERROR";
                rp["error"] = {{"code", "DISK_FULL"}, {"message", e.what()}};
                replyErr.payload = rp;
                m_transport->publish("ods.s4.capture.reply", replyErr);
            }
        }
    } catch (...) {
    }
}

} // namespace ods::s4::infrastructure
