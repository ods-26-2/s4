#include "s4/infrastructure/b3_bus/handlers/s2_event_handler.hpp"
#include "s4/domain/value_objects/capture_window.hpp"
#include "s4/domain/errors/domain_error.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#include <rpc.h>
#else
#include <uuid/uuid.h>
#endif

namespace ods::s4::infrastructure {

namespace {
std::string generate_clip_id_uuid() {
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

S2EventHandler::S2EventHandler(std::shared_ptr<application::ExtractClipUseCase> extractClip,
                               std::shared_ptr<domain::IMediaClipRepository> repository,
                               std::shared_ptr<IEventTransport> transport,
                               std::string sourceId,
                               std::string mediaDir)
    : m_extractClip(std::move(extractClip)),
      m_repository(std::move(repository)),
      m_transport(std::move(transport)),
      m_sourceId(std::move(sourceId)),
      m_mediaDir(std::move(mediaDir)) {}

void S2EventHandler::operator()(const std::string& topic, const B3Envelope& envelope) {
    (void)topic;
    try {
        const auto& p = envelope.payload;
        const std::string status = p.value("status", "");
        const std::string phase = p.value("phase", "");
        if ((status != "CONFIRMED" && status != "UNCERTAIN") || phase != "STARTED") {
            std::cerr << "S2 event ignored: status=" << status << " phase=" << phase << std::endl;
            return;
        }
        if (!m_extractClip || !m_extractClip->supportsCaptureWindows()) {
            std::cerr << "S2 event cannot be captured: capture windows are not supported" << std::endl;
            return;
        }
        auto cb = m_extractClip->captureBuffer();
        if (!cb) {
            std::cerr << "S2 event cannot be captured: no capture buffer" << std::endl;
            return;
        }
        domain::Nanoseconds eventTs = cb->newestCaptureTsNs();
        if (eventTs == 0) {
            std::cerr << "S2 event cannot be captured: buffer clock not started" << std::endl;
            return;
        }
        const std::string eventId = p.value("event_id", "");
        if (!eventId.empty() && m_repository &&
            !m_repository->findByEventId(eventId).empty()) {
            std::cerr << "S2 event duplicate skipped: event_id=" << eventId << std::endl;
            return;
        }
        std::cerr << "S2 event accepted: event_id=" << eventId
                  << " capture_ts=" << eventTs << std::endl;
        domain::CaptureWindow window = domain::CaptureWindow::around(eventTs, 5.0, 5.0);
        std::string clipId = generate_clip_id_uuid();
        std::filesystem::create_directories(m_mediaDir);
        std::string out = (std::filesystem::path(m_mediaDir) / (clipId + ".mp4")).string();
        try {
            auto clip = m_extractClip->executeCaptureWindow(eventId.empty() ? clipId : eventId, window, out, 20.0);
            std::cerr << "S2 event clip written: " << out << std::endl;
            B3Envelope outEnv;
            outEnv.messageType = "event";
            outEnv.messageId = generate_message_id();
            outEnv.schema = "ods.s4.clip";
            outEnv.schemaVersion = "1.0";
            outEnv.producer = "S4";
            outEnv.publishedAt = now_iso8601_utc();
            if (envelope.sessionId) outEnv.sessionId = envelope.sessionId;
            if (envelope.sourceId) outEnv.sourceId = envelope.sourceId; else outEnv.sourceId = m_sourceId;
            nlohmann::json payload;
            payload["clip_id"] = clip.clipId();
            payload["event_id"] = eventId.empty() ? nullptr : nlohmann::json(eventId);
            payload["media_type"] = "CLIP";
            payload["media_uri"] =
                "s4://clips/" +
                std::filesystem::path(clip.filePath()).filename().string();
            payload["start_at"] = now_iso8601_utc();
            payload["end_at"] = now_iso8601_utc();
            payload["points_of_interest"] = nlohmann::json::array();
            payload["hash_sha256"] = clip.sha256Hash();
            payload["expires_at"] = now_iso8601_utc();
            outEnv.payload = payload;
            if (m_transport) m_transport->publish("ods.s4.clip", outEnv);
            std::cerr << "S2 event clip published: clip_id=" << clip.clipId() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "S2 event clip failed: " << e.what() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "S2 event handler failed: " << e.what() << std::endl;
    }
}

} // namespace ods::s4::infrastructure
