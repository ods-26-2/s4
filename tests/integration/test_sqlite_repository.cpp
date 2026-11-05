#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "tests/ods_check.hpp"

#include <sqlite3.h>

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

void test_deve_persistir_tipo_e_expires_at_quando_clipe_for_salvo() {
    const std::string dbPath = make_temp_db_path() + ".kind";
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const auto expiresAt = now + std::chrono::hours(24);
    {
        infrastructure::SQLiteMediaClipRepository repo(dbPath);
        repo.save(domain::MediaClip("snap-1", "evt-snap", "/nvme/clips/snap-1.mp4", "sha",
                                    domain::TimeWindow(now, now + std::chrono::seconds(1)),
                                    true, false, now, {}, domain::MediaKind::Snapshot, expiresAt));
    }
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    const auto stored = repo.findById("snap-1");
    ODS_CHECK(stored.has_value());
    ODS_CHECK(stored->mediaKind() == domain::MediaKind::Snapshot);  // apesar do .mp4
    ODS_CHECK(stored->expiresAt().has_value());
    ODS_CHECK(*stored->expiresAt() == expiresAt);
    std::filesystem::remove(dbPath);
}

// Banco gravado antes do S4.3 completo: sem media_type e sem expires_at_ms.
void test_deve_migrar_banco_antigo_preenchendo_tipo_e_prazo_quando_abrir() {
    const std::string dbPath = make_temp_db_path() + ".legacy";
    constexpr long long kCreatedMs = 1790000000000LL;
    {
        sqlite3* db = nullptr;
        ODS_CHECK(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
        const char* legacy =
            "CREATE TABLE media_clips (clip_id TEXT PRIMARY KEY, event_id TEXT NOT NULL,"
            " file_path TEXT NOT NULL, sha256_hash TEXT NOT NULL, start_time_ms INTEGER NOT NULL,"
            " end_time_ms INTEGER NOT NULL, is_retained INTEGER NOT NULL,"
            " is_locked_for_audit INTEGER NOT NULL, created_at_ms INTEGER NOT NULL,"
            " points_of_interest TEXT NOT NULL DEFAULT '');"
            "INSERT INTO media_clips VALUES ('velho-clip','evt','/nvme/a.mp4','h',1,2,1,0,1790000000000,'');"
            "INSERT INTO media_clips VALUES ('velho-foto','evt','/nvme/a.JPG','h',1,2,1,0,1790000000000,'');";
        ODS_CHECK(sqlite3_exec(db, legacy, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    infrastructure::SQLiteMediaClipRepository repo(dbPath);
    const auto clip = repo.findById("velho-clip");
    const auto photo = repo.findById("velho-foto");
    ODS_CHECK(clip.has_value() && photo.has_value());
    ODS_CHECK(clip->mediaKind() == domain::MediaKind::VideoClip);
    ODS_CHECK(photo->mediaKind() == domain::MediaKind::Snapshot);
    const auto expected = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(kCreatedMs) + std::chrono::hours(24 * 7));
    ODS_CHECK(clip->expiresAt().has_value() && *clip->expiresAt() == expected);
    ODS_CHECK(repo.findPurgeCandidates().size() == 2u);
    // Reabrir de novo nao repete o ALTER TABLE nem falha.
    infrastructure::SQLiteMediaClipRepository again(dbPath);
    ODS_CHECK(again.findById("velho-clip").has_value());
    std::filesystem::remove(dbPath);
}

int main() {
    std::cout << "Running S4 SQLite Repository Integration Tests...\n";
    test_deve_persistir_e_recuperar_clipe_com_pois();
    test_deve_buscar_por_evento();
    test_deve_listar_apenas_candidatos_nao_travados_e_nao_expurgados();
    test_deve_atualizar_flags_e_remover_clipe();
    test_deve_recuperar_dados_apos_reabrir_database();
    test_deve_persistir_tipo_e_expires_at_quando_clipe_for_salvo();
    test_deve_migrar_banco_antigo_preenchendo_tipo_e_prazo_quando_abrir();
    std::cout << "All S4 SQLite tests passed successfully!\n";
    return 0;
}