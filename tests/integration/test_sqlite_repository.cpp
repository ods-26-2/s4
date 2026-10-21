#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"

using namespace ods::s4;

std::string make_temp_db_path() {
    const auto dir = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return (dir / ("s4_sqlite_test_" + std::to_string(stamp) + ".db")).string();
}

domain::MediaClip make_clip(
    const std::string& clipId,
    const std::string& eventId,
    bool isRetained,
    bool isLockedForAudit
) {
    const auto now = std::chrono::system_clock::now();
    return domain::MediaClip(
        clipId,
        eventId,
        "/nvme/clips/" + clipId + ".mp4",
        "sha256-" + clipId,
        domain::TimeWindow(now, now + std::chrono::seconds(10)),
        isRetained,
        isLockedForAudit,
        now,
        {domain::PointOfInterest{0.1, 0.2, "entrance"}, domain::PointOfInterest{0.9, 0.8, "checkout"}}
    );
}

void test_deve_persistir_e_recuperar_clipe_com_pois() {
    const std::string dbPath = make_temp_db_path();
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    const domain::MediaClip clip = make_clip("clip-1", "evt-1", true, false);
    repo.save(clip);

    const auto stored = repo.findById("clip-1");
    assert(stored.has_value());
    assert(stored->clipId() == "clip-1");
    assert(stored->eventId() == "evt-1");
    assert(stored->filePath() == "/nvme/clips/clip-1.mp4");
    assert(stored->sha256Hash() == "sha256-clip-1");
    assert(stored->isRetained());
    assert(!stored->isLockedForAudit());
    assert(stored->pointsOfInterest().size() == 2);
    assert(stored->pointsOfInterest()[0].label == "entrance");
    assert(stored->pointsOfInterest()[1].x == 0.9);

    std::filesystem::remove(dbPath);
}

void test_deve_buscar_por_evento() {
    const std::string dbPath = make_temp_db_path();
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    repo.save(make_clip("clip-evt-a-1", "evt-common", true, false));
    repo.save(make_clip("clip-evt-a-2", "evt-common", true, false));
    repo.save(make_clip("clip-other", "evt-other", true, false));

    const auto clips = repo.findByEventId("evt-common");
    assert(clips.size() == 2);

    std::filesystem::remove(dbPath);
}

void test_deve_listar_apenas_candidatos_nao_travados_e_nao_expurgados() {
    const std::string dbPath = make_temp_db_path();
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    repo.save(make_clip("clip-locked", "evt-lock", true, true));
    repo.save(make_clip("clip-purged", "evt-purged", false, false));
    repo.save(make_clip("clip-candidate", "evt-candidate", true, false));

    const auto candidates = repo.findPurgeCandidates();
    assert(candidates.size() == 1);
    assert(candidates[0].clipId() == "clip-candidate");

    std::filesystem::remove(dbPath);
}

void test_deve_atualizar_flags_e_remover_clipe() {
    const std::string dbPath = make_temp_db_path();
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    domain::MediaClip clip = make_clip("clip-update", "evt-u", true, false);
    repo.save(clip);

    auto stored = repo.findById("clip-update");
    assert(stored.has_value());
    stored->markAsPurged();
    stored->lockForAudit();
    repo.update(*stored);

    const auto after = repo.findById("clip-update");
    assert(after.has_value());
    assert(!after->isRetained());
    assert(after->isLockedForAudit());
    assert(repo.findPurgeCandidates().empty());

    repo.remove("clip-update");
    assert(!repo.findById("clip-update").has_value());

    std::filesystem::remove(dbPath);
}

void test_deve_recuperar_dados_apos_reabrir_database() {
    const std::string dbPath = make_temp_db_path();
    {
        infrastructure::SQLiteMediaClipRepository repo(dbPath);
        repo.save(make_clip("clip-reopen", "evt-reopen", true, false));
    }
    {
        infrastructure::SQLiteMediaClipRepository repo(dbPath);
        const auto stored = repo.findById("clip-reopen");
        assert(stored.has_value());
        assert(stored->filePath() == "/nvme/clips/clip-reopen.mp4");
    }
    std::filesystem::remove(dbPath);
}

int main() {
    std::cout << "Running S4 SQLite Repository Integration Tests...\n";
    test_deve_persistir_e_recuperar_clipe_com_pois();
    test_deve_buscar_por_evento();
    test_deve_listar_apenas_candidatos_nao_travados_e_nao_expurgados();
    test_deve_atualizar_flags_e_remover_clipe();
    test_deve_recuperar_dados_apos_reabrir_database();
    std::cout << "All S4 SQLite tests passed successfully!\n";
    return 0;
}