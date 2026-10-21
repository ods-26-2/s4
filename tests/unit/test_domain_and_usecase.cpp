#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/gstreamer/mock_ram_buffer_facade.hpp"
#include "s4/presentation/http/clip_descriptor_controller.hpp"
#include "tests/unit/fakes/in_memory_file_storage.hpp"

using namespace ods::s4;

std::vector<std::uint8_t> to_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

void test_deve_calcular_duracao_corretamente_quando_janela_for_valida() {
    auto now = std::chrono::system_clock::now();
    auto later = now + std::chrono::seconds(30);
    domain::TimeWindow tw(now, later);
    assert(tw.durationSeconds() >= 29.9 && tw.durationSeconds() <= 30.1);
}

void test_deve_lancar_excecao_quando_start_time_for_maior_ou_igual_ao_end_time() {
    auto now = std::chrono::system_clock::now();
    auto earlier = now - std::chrono::seconds(1);
    bool threw = false;
    try {
        domain::TimeWindow tw(now, earlier);
    } catch (const domain::InvalidTimeWindowError&) {
        threw = true;
    }
    assert(threw);
}

void test_deve_extrair_clipe_e_consultar_via_controller() {
    auto repo = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    auto buffer = std::make_shared<infrastructure::MockRAMBufferFacade>();
    auto storage = std::make_shared<test::InMemoryFileStorage>();

    auto start = std::chrono::system_clock::now();
    buffer->feedFrame({to_bytes("FRAME_00"), start});
    buffer->feedFrame({to_bytes("FRAME_01"), start + std::chrono::milliseconds(10)});
    buffer->feedFrame({to_bytes("FRAME_02"), start + std::chrono::milliseconds(20)});
    domain::TimeWindow tw(start, start + std::chrono::milliseconds(30));

    application::ExtractClipUseCase useCase(repo, buffer, storage);
    const auto clip = useCase.execute("evt-123", tw, "/nvme/clips/evt-123.mp4");

    assert(clip.eventId() == "evt-123");
    assert(clip.isRetained());
    assert(!clip.clipId().empty());
    assert(clip.filePath() == "/nvme/clips/evt-123.mp4");
    assert(clip.sha256Hash() == "c5d94dd20a8429595a2d3232e6464179bba1e9c119d07d57e2494e56e230dd35");
    assert(storage->exists("/nvme/clips/evt-123.mp4"));

    presentation::ClipDescriptorController controller(repo);
    const auto desc = controller.getClipDescriptor(clip.clipId());
    assert(desc.has_value());
    assert(desc->clipId == clip.clipId());
    assert(desc->fileUri == "/nvme/clips/evt-123.mp4");
    assert(!desc->startTimeIso.empty());
    assert(!desc->endTimeIso.empty());
    assert(desc->sha256Hash == clip.sha256Hash());
    assert(desc->isRetained);
    assert(!desc->isLockedForAudit);
}

void test_deve_preencher_points_of_interest_quando_clipe_possuir_pois() {
    auto repo = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    auto now = std::chrono::system_clock::now();

    domain::MediaClip clip(
        "clip-poi-1", "evt-poi", "/nvme/clips/poi.mp4", "abc123",
        domain::TimeWindow(now, now + std::chrono::seconds(10)),
        true, false, now,
        {domain::PointOfInterest{0.25, 0.75, "entrance"},
         domain::PointOfInterest{0.5, 0.5, "checkout"}}
    );
    repo->save(clip);

    presentation::ClipDescriptorController controller(repo);
    const auto desc = controller.getClipDescriptor("clip-poi-1");
    assert(desc.has_value());
    assert(desc->pointsOfInterest.size() == 2);
    assert(desc->pointsOfInterest[0].x == 0.25);
    assert(desc->pointsOfInterest[0].y == 0.75);
    assert(desc->pointsOfInterest[0].label == "entrance");
    assert(desc->pointsOfInterest[1].label == "checkout");
}

void test_deve_retornar_nullopt_quando_clipe_nao_existe() {
    auto repo = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    presentation::ClipDescriptorController controller(repo);
    assert(!controller.getClipDescriptor("clip-inexistente").has_value());
}

void test_deve_lancar_media_buffer_empty_quando_janela_sem_frames() {
    auto repo = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    auto buffer = std::make_shared<infrastructure::MockRAMBufferFacade>();
    auto storage = std::make_shared<test::InMemoryFileStorage>();

    auto start = std::chrono::system_clock::now();
    domain::TimeWindow tw(start, start + std::chrono::seconds(10));
    application::ExtractClipUseCase useCase(repo, buffer, storage);

    bool threw = false;
    try {
        useCase.execute("evt-vazio", tw, "/nvme/clips/vazio.mp4");
    } catch (const domain::MediaBufferEmptyError&) {
        threw = true;
    }
    assert(threw);
    assert(!storage->exists("/nvme/clips/vazio.mp4"));
}

int main() {
    std::cout << "Running S4 Domain & Application Tests...\n";
    test_deve_calcular_duracao_corretamente_quando_janela_for_valida();
    test_deve_lancar_excecao_quando_start_time_for_maior_ou_igual_ao_end_time();
    test_deve_extrair_clipe_e_consultar_via_controller();
    test_deve_preencher_points_of_interest_quando_clipe_possuir_pois();
    test_deve_retornar_nullopt_quando_clipe_nao_existe();
    test_deve_lancar_media_buffer_empty_quando_janela_sem_frames();
    std::cout << "All S4 tests passed successfully!\n";
    return 0;
}