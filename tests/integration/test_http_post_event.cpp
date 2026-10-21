#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

#include "tests/ods_check.hpp"

#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/mock_ram_buffer_facade.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

using namespace ods::s4;

namespace {

std::vector<std::uint8_t> to_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::filesystem::path make_temp_dir(const std::string& suffix) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("s4_post_event_" + std::to_string(stamp) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

long long epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        point.time_since_epoch()
    ).count();
}

struct Fixture {
    std::shared_ptr<infrastructure::InMemoryMediaClipRepository> repository;
    std::shared_ptr<infrastructure::MockRAMBufferFacade> buffer;
    std::shared_ptr<infrastructure::FileStorage> storage;
    std::shared_ptr<application::ExtractClipUseCase> useCase;
    std::filesystem::path mediaDir;

    Fixture()
        : repository(std::make_shared<infrastructure::InMemoryMediaClipRepository>()),
          buffer(std::make_shared<infrastructure::MockRAMBufferFacade>()),
          storage(std::make_shared<infrastructure::FileStorage>()),
          mediaDir(make_temp_dir("media")) {
        useCase = std::make_shared<application::ExtractClipUseCase>(repository, buffer, storage);
    }

    ~Fixture() {
        std::error_code errorCode;
        std::filesystem::remove_all(mediaDir, errorCode);
    }
};

httplib::Result post_event(httplib::Client& client, const std::string& body) {
    httplib::Result response;
    for (int attempt = 0; attempt < 40; ++attempt) {
        response = client.Post("/api/v1/events", httplib::Headers{}, body, "application/json");
        if (response) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return response;
}

std::string response_clip_id(const std::string& body) {
    const std::string marker = "\"clip_id\": \"";
    const std::size_t begin = body.find(marker);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t valueBegin = begin + marker.size();
    const std::size_t valueEnd = body.find('"', valueBegin);
    if (valueEnd == std::string::npos) {
        return {};
    }
    return body.substr(valueBegin, valueEnd - valueBegin);
}

void test_deve_criar_clipe_atraves_de_post() {
    const auto start = std::chrono::system_clock::now();
    Fixture fixture;
    fixture.buffer->feedFrame({to_bytes("FRAME_00"), start});
    fixture.buffer->feedFrame({to_bytes("FRAME_01"), start + std::chrono::milliseconds(10)});
    fixture.buffer->feedFrame({to_bytes("FRAME_02"), start + std::chrono::milliseconds(20)});

    presentation::ClipDescriptorHttpServer server(
        fixture.repository, fixture.useCase, fixture.mediaDir.string()
    );
    ODS_CHECK(server.bind(0));
    ODS_CHECK(server.start());
    const int port = server.port();
    ODS_CHECK(port > 0);

    httplib::Client client("127.0.0.1", port);
    const auto endMs = std::to_string(epoch_ms(start) + 30);
    const std::string body =
        "{\"event_id\":\"evt-post-001\",\"start_ms\":" + std::to_string(epoch_ms(start)) +
        ",\"end_ms\":" + endMs + "}";
    const auto response = post_event(client, body);

    ODS_CHECK(response);
    ODS_CHECK(response->status == 201);
    const std::string& result = response->body;
    ODS_CHECK(result.find("\"clip_id\": \"clip-") != std::string::npos);
    ODS_CHECK(result.find("\"file_uri\": \"") != std::string::npos);
    ODS_CHECK(result.find("evt-post-001.mp4") != std::string::npos);
    ODS_CHECK(result.find("\"sha256_hash\": \"") != std::string::npos);

    const std::string clipId = response_clip_id(result);
    ODS_CHECK(!clipId.empty());

    httplib::Result get;
    for (int attempt = 0; attempt < 40; ++attempt) {
        get = client.Get("/api/v1/clips/" + clipId);
        if (get && get->status == 200) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ODS_CHECK(get);
    ODS_CHECK(get->status == 200);

    const std::filesystem::path clipFile = fixture.mediaDir / "evt-post-001.mp4";
    ODS_CHECK(fixture.storage->exists(clipFile.string()));

    server.stop();
}

void test_deve_rejeitar_event_id_invalido() {
    const auto start = std::chrono::system_clock::now();
    Fixture fixture;
    fixture.buffer->feedFrame({to_bytes("FRAME_00"), start});

    presentation::ClipDescriptorHttpServer server(
        fixture.repository, fixture.useCase, fixture.mediaDir.string()
    );
    ODS_CHECK(server.bind(0));
    ODS_CHECK(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = post_event(client, R"({"event_id":"../evil"})");

    ODS_CHECK(response);
    ODS_CHECK(response->status == 400);

    server.stop();
}

void test_deve_rejeitar_janela_invertida() {
    Fixture fixture;

    presentation::ClipDescriptorHttpServer server(
        fixture.repository, fixture.useCase, fixture.mediaDir.string()
    );
    ODS_CHECK(server.bind(0));
    ODS_CHECK(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = post_event(
        client, R"({"event_id":"evt-bad-window","start_ms":2000,"end_ms":1000})"
    );

    ODS_CHECK(response);
    ODS_CHECK(response->status == 400);

    server.stop();
}

void test_deve_responder_500_quando_janela_sem_frames() {
    Fixture fixture;

    presentation::ClipDescriptorHttpServer server(
        fixture.repository, fixture.useCase, fixture.mediaDir.string()
    );
    ODS_CHECK(server.bind(0));
    ODS_CHECK(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = post_event(client, R"({"event_id":"evt-empty"})");

    ODS_CHECK(response);
    ODS_CHECK(response->status == 500);
    ODS_CHECK(response->body.find("error") != std::string::npos);

    server.stop();
}

} // namespace

int main() {
    std::cout << "Running S4 POST /api/v1/events Integration Tests...\n";
    test_deve_criar_clipe_atraves_de_post();
    test_deve_rejeitar_event_id_invalido();
    test_deve_rejeitar_janela_invertida();
    test_deve_responder_500_quando_janela_sem_frames();
    std::cout << "All S4 POST event tests passed successfully!\n";
    return 0;
}