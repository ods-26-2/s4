#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

using namespace ods::s4;

void test_deve_responder_200_com_json_dto_quando_clipe_existe() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    const auto now = std::chrono::system_clock::now();
    domain::MediaClip clip(
        "clip-http-1", "evt-http", "/nvme/clips/clip-http-1.mp4", "sha256-http",
        domain::TimeWindow(now, now + std::chrono::seconds(10)),
        true, false, now,
        {domain::PointOfInterest{0.4, 0.6, "zone-a"}}
    );
    repository->save(clip);

    presentation::ClipDescriptorHttpServer server(repository);
    assert(server.bind(0));
    assert(server.start());
    const int port = server.port();
    assert(port > 0);

    httplib::Client client("127.0.0.1", port);
    httplib::Result response;
    for (int attempt = 0; attempt < 40; ++attempt) {
        response = client.Get("/api/v1/clips/clip-http-1");
        if (response && response->status == 200) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    assert(response);
    assert(response->status == 200);
    const std::string& body = response->body;
    (void)body;
    assert(body.find("\"clip_id\": \"clip-http-1\"") != std::string::npos);
    assert(body.find("\"file_uri\": \"/nvme/clips/clip-http-1.mp4\"") != std::string::npos);
    assert(body.find("\"sha256_hash\": \"sha256-http\"") != std::string::npos);
    assert(body.find("\"start_time\": \"") != std::string::npos);
    assert(body.find("\"points_of_interest\"") != std::string::npos);
    assert(body.find("\"label\": \"zone-a\"") != std::string::npos);
    assert(body.find("\"is_retained\": true") != std::string::npos);

    server.stop();
}

void test_deve_responder_404_quando_clipe_nao_existe() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    presentation::ClipDescriptorHttpServer server(repository);
    assert(server.bind(0));
    assert(server.start());
    const int port = server.port();

    httplib::Client client("127.0.0.1", port);
    httplib::Result response;
    for (int attempt = 0; attempt < 40; ++attempt) {
        response = client.Get("/api/v1/clips/clip-nao-existe");
        if (response && response->status == 404) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    assert(response);
    assert(response->status == 404);

    server.stop();
}

void test_deve_responder_200_no_healthz() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    presentation::ClipDescriptorHttpServer server(repository);
    assert(server.bind(0));
    assert(server.start());
    const int port = server.port();

    httplib::Client client("127.0.0.1", port);
    httplib::Result response;
    for (int attempt = 0; attempt < 40; ++attempt) {
        response = client.Get("/healthz");
        if (response && response->status == 200) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    assert(response);
    assert(response->status == 200);
    assert(response->body.find("\"status\": \"ok\"") != std::string::npos);

    server.stop();
}

int main() {
    std::cout << "Running S4 HTTP Server Integration Tests...\n";
    test_deve_responder_200_com_json_dto_quando_clipe_existe();
    test_deve_responder_404_quando_clipe_nao_existe();
    test_deve_responder_200_no_healthz();
    std::cout << "All S4 HTTP tests passed successfully!\n";
    return 0;
}