#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"
#include "tests/ods_check.hpp"

using namespace ods::s4;

namespace {

std::string expected_cors_origin() {
    const char* value = std::getenv("ODS_S4_CORS_ORIGIN");
    return (value == nullptr || *value == '\0') ? std::string{"*"} : std::string{value};
}

} // namespace

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

httplib::Result get_until(int port, const std::string& path, int status) {
    httplib::Client client("127.0.0.1", port);
    httplib::Result response;
    for (int attempt = 0; attempt < 40; ++attempt) {
        response = client.Get(path);
        if (response && response->status == status) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return response;
}

void test_deve_servir_media_do_clipe_quando_arquivo_existe() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    const auto directory = std::filesystem::temp_directory_path() / "s4-http-media-test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    const auto mediaPath = directory / "clip-media.mp4";
    {
        std::ofstream out(mediaPath, std::ios::binary);
        out << "fake-mp4-bytes";
    }
    const auto now = std::chrono::system_clock::now();
    repository->save(domain::MediaClip(
        "clip-media", "evt-media", mediaPath.string(), "sha-media",
        domain::TimeWindow(now, now + std::chrono::seconds(10)), true, false, now, {},
        domain::MediaKind::VideoClip, now + std::chrono::hours(1)));

    presentation::ClipDescriptorHttpServer server(repository, {}, directory.string());
    ODS_CHECK(server.bind(0) && server.start());

    const auto response = get_until(server.port(), "/api/v1/clips/clip-media/media", 200);
    ODS_CHECK(response && response->status == 200);
    ODS_CHECK(response->body == "fake-mp4-bytes");
    ODS_CHECK(response->get_header_value("Content-Type").find("video/mp4") != std::string::npos);
    ODS_CHECK(response->get_header_value("Access-Control-Allow-Origin") == expected_cors_origin());
    server.stop();
    std::filesystem::remove_all(directory, ec);
}

void test_deve_negar_media_quando_clipe_expirou_ou_arquivo_sumiu() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    const auto directory = std::filesystem::temp_directory_path() / "s4-http-media-expired";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    const auto now = std::chrono::system_clock::now();
    const domain::TimeWindow window(now - std::chrono::seconds(10), now);
    repository->save(domain::MediaClip(
        "clip-media-expired", "evt", (directory / "expired.mp4").string(), "sha", window,
        true, false, now, {}, domain::MediaKind::VideoClip, now - std::chrono::seconds(1)));
    repository->save(domain::MediaClip(
        "clip-media-missing", "evt", (directory / "missing.mp4").string(), "sha", window,
        true, false, now, {}, domain::MediaKind::VideoClip, now + std::chrono::hours(1)));

    presentation::ClipDescriptorHttpServer server(repository, {}, directory.string());
    ODS_CHECK(server.bind(0) && server.start());

    const auto expired = get_until(server.port(), "/api/v1/clips/clip-media-expired/media", 410);
    ODS_CHECK(expired && expired->status == 410);
    ODS_CHECK(expired->body.find("CLIP_EXPIRED") != std::string::npos);

    const auto missing = get_until(server.port(), "/api/v1/clips/clip-media-missing/media", 404);
    ODS_CHECK(missing && missing->status == 404);
    server.stop();
    std::filesystem::remove_all(directory, ec);
}

void test_deve_expor_cors_no_get_e_no_preflight() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    presentation::ClipDescriptorHttpServer server(repository);
    ODS_CHECK(server.bind(0) && server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto health = get_until(server.port(), "/healthz", 200);
    ODS_CHECK(health && health->status == 200);
    ODS_CHECK(health->get_header_value("Access-Control-Allow-Origin") == expected_cors_origin());

    const auto preflight = client.Options("/api/v1/events");
    ODS_CHECK(preflight && preflight->status == 204);
    ODS_CHECK(preflight->get_header_value("Access-Control-Allow-Origin") == expected_cors_origin());
    ODS_CHECK(preflight->get_header_value("Access-Control-Allow-Methods").find("POST") !=
              std::string::npos);
    server.stop();
}

void test_deve_expor_media_type_e_expires_at_quando_clipe_estiver_no_prazo() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    const auto now = std::chrono::system_clock::now();
    repository->save(domain::MediaClip(
        "clip-prazo", "evt", "/nvme/clips/clip-prazo.mp4", "sha",
        domain::TimeWindow(now, now + std::chrono::seconds(1)), true, false, now, {},
        domain::MediaKind::Snapshot, now + std::chrono::hours(24)));
    presentation::ClipDescriptorHttpServer server(repository);
    ODS_CHECK(server.bind(0) && server.start());

    const auto response = get_until(server.port(), "/api/v1/clips/clip-prazo", 200);
    ODS_CHECK(response && response->status == 200);
    ODS_CHECK(response->body.find("\"media_type\": \"SNAPSHOT\"") != std::string::npos);
    ODS_CHECK(response->body.find("\"expires_at\": \"") != std::string::npos);
    server.stop();
}

// 6c: depois de expires_at o media_uri nao resolve, mesmo antes da varredura.
void test_deve_responder_410_sem_caminho_quando_clipe_vencer_ou_for_expurgado() {
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    const auto now = std::chrono::system_clock::now();
    const domain::TimeWindow window(now - std::chrono::seconds(10), now);
    repository->save(domain::MediaClip(
        "clip-vencido", "evt", "/nvme/clips/clip-vencido.mp4", "sha", window, true, false,
        now - std::chrono::hours(48), {}, domain::MediaKind::VideoClip, now - std::chrono::seconds(1)));
    repository->save(domain::MediaClip(
        "clip-expurgado", "evt", "/nvme/clips/clip-expurgado.mp4", "sha", window, false, false,
        now, {}, domain::MediaKind::VideoClip, now + std::chrono::hours(1)));
    presentation::ClipDescriptorHttpServer server(repository);
    ODS_CHECK(server.bind(0) && server.start());

    for (const std::string id : {"clip-vencido", "clip-expurgado"}) {
        const auto response = get_until(server.port(), "/api/v1/clips/" + id, 410);
        ODS_CHECK(response && response->status == 410);
        ODS_CHECK(response->body.find("CLIP_EXPIRED") != std::string::npos);
        ODS_CHECK(response->body.find("/nvme/clips") == std::string::npos);
    }
    server.stop();
}

int main() {
    std::cout << "Running S4 HTTP Server Integration Tests...\n";
    test_deve_responder_200_com_json_dto_quando_clipe_existe();
    test_deve_responder_404_quando_clipe_nao_existe();
    test_deve_responder_200_no_healthz();
    test_deve_servir_media_do_clipe_quando_arquivo_existe();
    test_deve_negar_media_quando_clipe_expirou_ou_arquivo_sumiu();
    test_deve_expor_cors_no_get_e_no_preflight();
    test_deve_expor_media_type_e_expires_at_quando_clipe_estiver_no_prazo();
    test_deve_responder_410_sem_caminho_quando_clipe_vencer_ou_for_expurgado();
    std::cout << "All S4 HTTP tests passed successfully!\n";
    return 0;
}