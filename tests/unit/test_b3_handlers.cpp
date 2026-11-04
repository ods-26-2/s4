// Contrato B1/6c dos clipes publicados pelo S4: captured_at e sequence
// presentes, session_id/clip_id em UUID e correlation_id referenciando o
// event_id (automatico) ou o message_id do comando (manual).
#include "tests/ods_check.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "s4/application/services/capture_buffer_reader.hpp"
#include "s4/application/services/media_muxer.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/b3_bus/envelope.hpp"
#include "s4/infrastructure/b3_bus/handlers/capture_command_handler.hpp"
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

} // namespace

int main() {
    std::cout << "Running S4 B3 handler tests...\n";
    test_clip_automatico_deve_ter_campos_obrigatorios_do_envelope();
    test_clip_manual_deve_correlacionar_com_message_id_do_comando();
    std::cout << "All S4 B3 handler tests passed successfully!\n";
    return 0;
}
