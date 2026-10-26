// S4.1 item 6 — OUT_OF_BUFFER tipado na API REST.
//
// POST /api/v1/events pelo relogio de captura (capture_ts_ns) com o buffer REAL
// (RingBufferRAMFacade) e um muxer falso: janela ja expulsa e janela com fim
// alem do post_s maximo respondem 422 com o erro no formato comum de reply da
// 6c ({"status":"ERROR","error":{"code":"OUT_OF_BUFFER",...}}), em vez de 500.
// O caminho legado (start_ms/end_ms) nao muda: continua coberto, com 500, pelos
// testes do S4.2/S4.4.
#include "tests/ods_check.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

#include "s4/application/ring_buffer_config.hpp"
#include "s4/application/services/media_muxer.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"
#include "tests/unit/fakes/in_memory_file_storage.hpp"

using namespace ods::s4;
using domain::kNanosecondsPerSecond;
using domain::Nanoseconds;

namespace {

constexpr Nanoseconds S = kNanosecondsPerSecond;

// Fonte controlada pelo teste.
class ManualFrameSource : public domain::IFrameSource {
public:
    void start(domain::FrameCallback onFrame) override { m_onFrame = std::move(onFrame); }
    void stop() override {}
    [[nodiscard]] bool isRunning() const override { return static_cast<bool>(m_onFrame); }

    void emit(Nanoseconds captureTsNs) {
        static const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01, 0x65};
        domain::CapturedFrame frame;
        frame.captureTsNs = captureTsNs;
        frame.data = payload.data();
        frame.length = payload.size();
        frame.isKeyframe = true;
        frame.sessionId = "sessao-http";
        m_onFrame(frame);
    }

private:
    domain::FrameCallback m_onFrame;
};

// O container nao importa aqui: o que se testa e a decisao de aceitar a janela.
class ConcatenatingMuxer : public application::IMediaMuxer {
public:
    std::vector<std::uint8_t> mux(const application::ExtractedSegment& segment) override {
        return segment.asElementaryStream();
    }
    [[nodiscard]] const char* containerName() const override { return "h264"; }
};

struct Fixture {
    ManualFrameSource* source{nullptr};
    std::shared_ptr<infrastructure::RingBufferRAMFacade> buffer;
    std::unique_ptr<presentation::ClipDescriptorHttpServer> server;

    Fixture() {
        application::RingBufferConfig config;
        config.cameraId = "http_oob_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
        config.windowSeconds = 30.0;
        config.bitrateBps = 134400;
        config.maxPostSeconds = 10.0;
        auto owned = std::make_unique<ManualFrameSource>();
        source = owned.get();
        buffer = std::make_shared<infrastructure::RingBufferRAMFacade>(config, std::move(owned));
        buffer->startCapture("manual");

        auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
        auto useCase = std::make_shared<application::ExtractClipUseCase>(
            repository, buffer, std::make_shared<test::InMemoryFileStorage>(), buffer,
            std::make_shared<ConcatenatingMuxer>());
        server = std::make_unique<presentation::ClipDescriptorHttpServer>(
            repository, useCase, "/tmp/s4_oob_media");
        ODS_CHECK(server->bind(0));
        ODS_CHECK(server->start());
    }

    ~Fixture() {
        server->stop();
        buffer->stopCapture();
    }

    httplib::Result post_capture_event(const std::string& eventId, Nanoseconds captureTsNs,
                                       const std::string& pre, const std::string& post) {
        httplib::Client client("127.0.0.1", server->port());
        client.set_read_timeout(std::chrono::seconds(15));
        const std::string body = R"({"event_id":")" + eventId + R"(","capture_ts_ns":)" +
                                 std::to_string(captureTsNs) + R"(,"pre_window_seconds":)" + pre +
                                 R"(,"post_window_seconds":)" + post + "}";
        httplib::Result response;
        for (int attempt = 0; attempt < 40 && !response; ++attempt) {
            response = client.Post("/api/v1/events", body, "application/json");
            if (!response) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
        return response;
    }
};

bool is_typed_out_of_buffer(const httplib::Result& response) {
    return response && response->status == 422 &&
           response->body.find(R"("status":"ERROR")") != std::string::npos &&
           response->body.find(R"("code":"OUT_OF_BUFFER")") != std::string::npos;
}

} // namespace

void test_deve_responder_out_of_buffer_quando_janela_ja_tiver_saido_do_buffer() {
    Fixture fixture;
    for (int second = 100; second <= 120; ++second) {
        fixture.source->emit(second * S);
    }

    const auto response = fixture.post_capture_event("evt-passado", 10 * S, "1.0", "1.0");

    ODS_CHECK(is_typed_out_of_buffer(response));
}

void test_deve_responder_out_of_buffer_quando_fim_da_janela_passar_do_post_maximo() {
    Fixture fixture;
    for (int second = 100; second <= 120; ++second) {
        fixture.source->emit(second * S);
    }

    const auto begin = std::chrono::steady_clock::now();
    const auto response = fixture.post_capture_event("evt-futuro", 120 * S, "1.0", "30.0");

    ODS_CHECK(is_typed_out_of_buffer(response));
    // Recusado na hora, sem prender a thread do servidor no teto de espera.
    ODS_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
}

void test_deve_criar_clipe_quando_janela_estiver_no_buffer() {
    Fixture fixture;
    for (int second = 100; second <= 120; ++second) {
        fixture.source->emit(second * S);
    }

    const auto response = fixture.post_capture_event("evt-ok", 110 * S, "2.0", "1.0");

    ODS_CHECK(response);
    ODS_CHECK(response->status == 201);
    ODS_CHECK(response->body.find("clip_id") != std::string::npos);
}

int main() {
    test_deve_responder_out_of_buffer_quando_janela_ja_tiver_saido_do_buffer();
    test_deve_responder_out_of_buffer_quando_fim_da_janela_passar_do_post_maximo();
    test_deve_criar_clipe_quando_janela_estiver_no_buffer();
    std::cout << "test_s4_out_of_buffer_http: all tests passed\n";
    return 0;
}
