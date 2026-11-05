// Contrato B1/6c dos clipes publicados pelo S4: captured_at e sequence
// presentes, session_id/clip_id em UUID e correlation_id referenciando o
// event_id (automatico) ou o message_id do comando (manual).
#include "tests/ods_check.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "s4/application/services/capture_buffer_reader.hpp"
#include "s4/application/services/media_muxer.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/b3_bus/envelope.hpp"
#include "s4/application/dtos/iso_time_utils.hpp"
#include "s4/infrastructure/b3_bus/handlers/capture_command_handler.hpp"
#include "s4/infrastructure/b3_bus/handlers/clip_query_handler.hpp"
#include "s4/infrastructure/b3_bus/handlers/s2_event_handler.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/gstreamer/mock_ram_buffer_facade.hpp"
#include "tests/unit/fakes/in_memory_file_storage.hpp"

using namespace ods::s4;
using domain::kNanosecondsPerSecond;
using domain::Nanoseconds;

namespace {

constexpr Nanoseconds kSecond = kNanosecondsPerSecond;

class RecordingTransport : public infrastructure::IEventTransport {
public:
    struct Published {
        std::string topic;
        infrastructure::B3Envelope envelope;
    };

    void subscribe(const std::string&, MessageHandler) override {}
    void publish(const std::string& topic, const infrastructure::B3Envelope& envelope) override {
        published.push_back({topic, envelope});
    }
    void start() override {}
    void stop() override {}

    const infrastructure::B3Envelope* find(const std::string& topic) const {
        for (const auto& item : published) {
            if (item.topic == topic) {
                return &item.envelope;
            }
        }
        return nullptr;
    }

    std::vector<Published> published;
};

class FakeCaptureBuffer : public application::ICaptureBufferReader {
public:
    explicit FakeCaptureBuffer(std::string session) : m_session(std::move(session)) {}

    application::ExtractedSegment extractCaptureWindow(
        const domain::CaptureWindow& window, double) override {
        application::ExtractedFrame frame;
        frame.sequence = 0;
        frame.captureTsNs = window.startNs();
        frame.isKeyframe = true;
        frame.data = {0x00, 0x00, 0x00, 0x01, 0x65};
        application::ExtractedSegment segment{window, m_session, {}, false, false};
        segment.frames.push_back(frame);
        return segment;
    }

    domain::BufferStats bufferStats() const override {
        domain::BufferStats stats;
        stats.hasFrames = true;
        stats.newestCaptureTsNs = 1000 * kSecond;
        stats.oldestCaptureTsNs = 990 * kSecond;
        stats.sessionId = m_session;
        return stats;
    }

    Nanoseconds newestCaptureTsNs() const override { return 1000 * kSecond; }

    std::chrono::system_clock::time_point toWallClock(Nanoseconds captureTsNs) const override {
        return std::chrono::system_clock::time_point(std::chrono::seconds(captureTsNs / kSecond));
    }

private:
    std::string m_session;
};

class FakeMuxer : public application::IMediaMuxer {
public:
    std::vector<std::uint8_t> mux(const application::ExtractedSegment& segment) override {
        return segment.asElementaryStream();
    }
    [[nodiscard]] const char* containerName() const override { return "h264"; }
};

struct Fixture {
    std::shared_ptr<infrastructure::InMemoryMediaClipRepository> repository{
        std::make_shared<infrastructure::InMemoryMediaClipRepository>()};
    std::shared_ptr<infrastructure::MockRAMBufferFacade> legacyBuffer{
        std::make_shared<infrastructure::MockRAMBufferFacade>()};
    std::shared_ptr<application::ExtractClipUseCase> useCase;
    std::shared_ptr<RecordingTransport> transport{std::make_shared<RecordingTransport>()};

    Fixture() {
        auto capture = std::make_shared<FakeCaptureBuffer>("exec-demo-1");
        useCase = std::make_shared<application::ExtractClipUseCase>(
            repository,
            legacyBuffer,
            std::make_shared<test::InMemoryFileStorage>(),
            capture,
            std::make_shared<FakeMuxer>());
    }
};

infrastructure::B3Envelope s2_event(const std::string& eventId) {
    infrastructure::B3Envelope envelope;
    envelope.messageType = "event";
    envelope.messageId = infrastructure::generate_message_id();
    envelope.schema = "ods.s2.event";
    envelope.schemaVersion = "1.0";
    envelope.producer = "S2";
    envelope.publishedAt = infrastructure::now_iso8601_utc();
    envelope.sessionId = "exec-demo-1";
    envelope.capturedAt = "2026-10-29T12:00:00.000Z";
    envelope.payload = {
        {"event_id", eventId}, {"status", "CONFIRMED"}, {"phase", "STARTED"}};
    return envelope;
}

void test_clip_automatico_deve_ter_campos_obrigatorios_do_envelope() {
    Fixture fixture;
    infrastructure::S2EventHandler handler(
        fixture.useCase, fixture.repository, fixture.transport, "cam0", "/tmp/s4_handler_test");
    handler("ods.s2.event", s2_event("evt-123"));

    const auto* clip = fixture.transport->find("ods.s4.clip");
    ODS_CHECK(clip != nullptr);
    ODS_CHECK(clip->capturedAt.has_value());
    ODS_CHECK(*clip->capturedAt == "2026-10-29T12:00:00.000Z");
    ODS_CHECK(clip->sequence.has_value());
    ODS_CHECK(clip->sessionId.has_value());
    ODS_CHECK(infrastructure::is_uuid(*clip->sessionId));
    ODS_CHECK(clip->correlationId.has_value());
    ODS_CHECK(*clip->correlationId == "evt-123");

    const std::string clipId = clip->payload.at("clip_id").get<std::string>();
    ODS_CHECK(infrastructure::is_uuid(clipId));
    ODS_CHECK(clipId.find("clip-") == std::string::npos);
}

void test_clip_manual_deve_correlacionar_com_message_id_do_comando() {
    Fixture fixture;
    infrastructure::CaptureCommandHandler handler(
        fixture.useCase, fixture.repository, fixture.transport, "cam0", "/tmp/s4_handler_test");

    infrastructure::B3Envelope command;
    command.messageType = "command";
    command.messageId = "cmd-message-id";
    command.schema = "ods.s4.capture.command";
    command.schemaVersion = "1.0";
    command.producer = "B6";
    command.publishedAt = infrastructure::now_iso8601_utc();
    command.sessionId = "exec-demo-1";
    command.capturedAt = "2026-10-29T12:00:01.000Z";
    command.payload = {{"media_type", "CLIP"}};
    handler("ods.s4.capture.command", command);

    const auto* clip = fixture.transport->find("ods.s4.clip");
    ODS_CHECK(clip != nullptr);
    ODS_CHECK(clip->correlationId.has_value());
    ODS_CHECK(*clip->correlationId == "cmd-message-id");
    ODS_CHECK(clip->capturedAt.has_value());
    ODS_CHECK(clip->sequence.has_value());
    ODS_CHECK(clip->sessionId.has_value());
    ODS_CHECK(infrastructure::is_uuid(*clip->sessionId));

    const auto* reply = fixture.transport->find("ods.s4.capture.reply");
    ODS_CHECK(reply != nullptr);
    ODS_CHECK(reply->payload.at("clip_id").get<std::string>() ==
              clip->payload.at("clip_id").get<std::string>());
    ODS_CHECK(infrastructure::is_uuid(clip->payload.at("clip_id").get<std::string>()));
}

// ------------------------------------------------------------- S4.3 / 6c

infrastructure::B3Envelope capture_command(const std::string& mediaType) {
    infrastructure::B3Envelope command;
    command.messageType = "command";
    command.messageId = "cmd-" + mediaType;
    command.schema = "ods.s4.capture.command";
    command.schemaVersion = "1.0";
    command.producer = "B6";
    command.publishedAt = infrastructure::now_iso8601_utc();
    command.sessionId = "exec-demo-1";
    command.payload = {{"media_type", mediaType}};
    return command;
}

infrastructure::B3Envelope clip_query(const nlohmann::json& payload) {
    infrastructure::B3Envelope query;
    query.messageType = "query";
    query.messageId = "query-1";
    query.schema = "ods.s4.clip.query";
    query.schemaVersion = "1.0";
    query.producer = "B6";
    query.publishedAt = infrastructure::now_iso8601_utc();
    query.payload = payload;
    return query;
}

void test_clip_publicado_deve_trazer_expires_at_da_politica_e_nao_o_instante_atual() {
    Fixture fixture;
    fixture.useCase->setRetentionPolicy(domain::RetentionPolicy(std::chrono::hours(24 * 7)));
    infrastructure::S2EventHandler handler(
        fixture.useCase, fixture.repository, fixture.transport, "cam0", "/tmp/s4_handler_test");
    handler("ods.s2.event", s2_event("evt-prazo"));

    const auto* clip = fixture.transport->find("ods.s4.clip");
    ODS_CHECK(clip != nullptr);
    const auto stored = fixture.repository->findByEventId("evt-prazo");
    ODS_CHECK(stored.size() == 1u && stored.front().expiresAt().has_value());
    ODS_CHECK(*stored.front().expiresAt() - stored.front().createdAt() == std::chrono::hours(24 * 7));
    ODS_CHECK(clip->payload.at("expires_at").get<std::string>() ==
              application::to_iso8601(*stored.front().expiresAt()));
    ODS_CHECK(clip->payload.at("media_type").get<std::string>() == "CLIP");
    ODS_CHECK(clip->payload.at("media_uri").get<std::string>().rfind("s4://clips/", 0) == 0);
}

void test_snapshot_deve_ser_gravado_como_snapshot_e_usar_o_prazo_de_snapshot() {
    Fixture fixture;
    fixture.useCase->setRetentionPolicy(
        domain::RetentionPolicy(std::chrono::hours(24 * 7), std::chrono::hours(24)));
    infrastructure::CaptureCommandHandler handler(
        fixture.useCase, fixture.repository, fixture.transport, "cam0", "/tmp/s4_handler_test");
    handler("ods.s4.capture.command", capture_command("SNAPSHOT"));

    const auto* clip = fixture.transport->find("ods.s4.clip");
    ODS_CHECK(clip != nullptr);
    ODS_CHECK(clip->payload.at("media_type").get<std::string>() == "SNAPSHOT");
    const auto stored = fixture.repository->findById(clip->payload.at("clip_id").get<std::string>());
    ODS_CHECK(stored.has_value());
    ODS_CHECK(stored->mediaKind() == domain::MediaKind::Snapshot);
    ODS_CHECK(*stored->expiresAt() - stored->createdAt() == std::chrono::hours(24));
}

void test_clip_query_deve_responder_o_descritor_gravado_quando_clipe_estiver_no_prazo() {
    Fixture fixture;
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    fixture.repository->save(domain::MediaClip(
        "clip-ok", "evt-ok", "/data/media/clip-ok.mp4", "sha-ok",
        domain::TimeWindow(now - std::chrono::seconds(10), now), true, false, now, {},
        domain::MediaKind::VideoClip, now + std::chrono::hours(1)));
    infrastructure::ClipQueryHandler handler(fixture.repository, fixture.transport);
    handler("ods.s4.clip.query", clip_query({{"clip_id", "clip-ok"}}));

    const auto* reply = fixture.transport->find("ods.s4.clip.query.reply");
    ODS_CHECK(reply != nullptr);
    const auto& clips = reply->payload.at("clips");
    ODS_CHECK(clips.size() == 1u);
    ODS_CHECK(clips[0].at("expires_at").get<std::string>() ==
              application::to_iso8601(now + std::chrono::hours(1)));
    ODS_CHECK(clips[0].at("start_at").get<std::string>() ==
              application::to_iso8601(now - std::chrono::seconds(10)));
    ODS_CHECK(clips[0].at("media_uri").get<std::string>() == "s4://clips/clip-ok.mp4");
}

void test_clip_query_deve_responder_lista_vazia_quando_clipe_expirou_ou_foi_expurgado() {
    Fixture fixture;
    const auto now = std::chrono::system_clock::now();
    const domain::TimeWindow window(now - std::chrono::seconds(10), now);
    // Vencido e ainda nao varrido pelo daemon.
    fixture.repository->save(domain::MediaClip(
        "vencido", "evt-velho", "/data/media/a.mp4", "h", window, true, false,
        now - std::chrono::hours(48), {}, domain::MediaKind::VideoClip, now - std::chrono::hours(1)));
    // Ja expurgado.
    fixture.repository->save(domain::MediaClip(
        "expurgado", "evt-velho", "/data/media/b.mp4", "h", window, false, false,
        now - std::chrono::hours(48), {}, domain::MediaKind::VideoClip, now + std::chrono::hours(1)));
    infrastructure::ClipQueryHandler handler(fixture.repository, fixture.transport);

    handler("ods.s4.clip.query", clip_query({{"event_id", "evt-velho"}}));
    const auto* byEvent = fixture.transport->find("ods.s4.clip.query.reply");
    ODS_CHECK(byEvent != nullptr);
    ODS_CHECK(byEvent->payload.at("status").get<std::string>() == "OK");
    ODS_CHECK(byEvent->payload.at("clips").empty());

    fixture.transport->published.clear();
    handler("ods.s4.clip.query", clip_query({{"clip_id", "vencido"}}));
    ODS_CHECK(fixture.transport->find("ods.s4.clip.query.reply")->payload.at("clips").empty());
}

void test_deve_avisar_o_expurgo_quando_um_clipe_novo_for_salvo() {
    Fixture fixture;
    int notified = 0;
    fixture.useCase->setOnClipSaved([&notified](const domain::MediaClip&) { ++notified; });
    infrastructure::S2EventHandler handler(
        fixture.useCase, fixture.repository, fixture.transport, "cam0", "/tmp/s4_handler_test");
    handler("ods.s2.event", s2_event("evt-aviso"));
    ODS_CHECK(notified == 1);

    // Um aviso que falha nao desfaz o clipe ja gravado.
    fixture.useCase->setOnClipSaved([](const domain::MediaClip&) { throw std::runtime_error("x"); });
    handler("ods.s2.event", s2_event("evt-aviso-2"));
    ODS_CHECK(fixture.repository->findByEventId("evt-aviso-2").size() == 1u);
}

} // namespace

int main() {
    std::cout << "Running S4 B3 handler tests...\n";
    test_clip_automatico_deve_ter_campos_obrigatorios_do_envelope();
    test_clip_manual_deve_correlacionar_com_message_id_do_comando();
    test_clip_publicado_deve_trazer_expires_at_da_politica_e_nao_o_instante_atual();
    test_snapshot_deve_ser_gravado_como_snapshot_e_usar_o_prazo_de_snapshot();
    test_clip_query_deve_responder_o_descritor_gravado_quando_clipe_estiver_no_prazo();
    test_clip_query_deve_responder_lista_vazia_quando_clipe_expirou_ou_foi_expurgado();
    test_deve_avisar_o_expurgo_quando_um_clipe_novo_for_salvo();
    std::cout << "All S4 B3 handler tests passed successfully!\n";
    return 0;
}
