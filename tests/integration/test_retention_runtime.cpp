// S4.3 — testes de integracao: statvfs real, log de expurgo em arquivo real,
// SQLite + NVMe (sistema de arquivos) reais ponta a ponta, e o daemon com
// threads de verdade.
#include "tests/ods_check.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "s4/application/use_cases/purge_media.hpp"
#include "s4/domain/strategies/disk_quota_fifo_strategy.hpp"
#include "s4/domain/strategies/lgpd_retention_strategy.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/retention/json_lines_purge_log.hpp"
#include "s4/infrastructure/retention/purge_daemon.hpp"
#include "s4/infrastructure/retention/statvfs_disk_usage_provider.hpp"
#include "tests/unit/fakes/retention_fakes.hpp"

using namespace ods::s4;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::system_clock;
using std::chrono::hours;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr hours kDay{24};

template <typename Exception, typename Callable>
bool throws(Callable&& callable) {
    try {
        callable();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// Diretorio temporario proprio de cada teste, apagado no fim.
class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() /
                 (prefix + std::to_string(++counter) + "_" + std::to_string(stamp % 1000000));
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }

    [[nodiscard]] std::string file(const std::string& name) const { return (m_path / name).string(); }
    [[nodiscard]] std::string path() const { return m_path.string(); }

private:
    fs::path m_path;
};

void write_file(const std::string& path, std::size_t sizeBytes) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(sizeBytes, 'v');
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

domain::MediaClip clip_at(
    const std::string& id,
    const std::string& filePath,
    Clock::time_point createdAt,
    bool isLockedForAudit = false
) {
    return domain::MediaClip(id, "evt-" + id, filePath, "sha-" + id,
                             domain::TimeWindow(createdAt - seconds(5), createdAt), true,
                             isLockedForAudit, createdAt);
}

domain::PurgeLogEntry sample_entry(const std::string& clipId) {
    domain::PurgeLogEntry entry;
    entry.clipId = clipId;
    entry.eventId = "evt-\"aspas\"";
    entry.filePath = "/data/media/evt.mp4";
    entry.reason = domain::PurgeReason::RetentionPeriodExpired;
    entry.fileOutcome = domain::FileOutcome::Deleted;
    entry.bytesFreed = 1048576;
    entry.diskUsedRatio = 0.4210;
    entry.purgedAt = Clock::time_point(seconds(1790000000));
    return entry;
}

// Ocupacao ajustavel: o disco real da maquina de teste nao pode decidir se a
// cota dispara (em um host a 90% o teste mudaria de resultado).
class AdjustableDiskUsage : public domain::IDiskUsageProvider {
public:
    [[nodiscard]] domain::DiskUsage currentUsage() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return domain::DiskUsage(m_used, 100 - m_used);
    }
    void setUsedPercent(std::uintmax_t used) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_used = used;
    }

private:
    mutable std::mutex m_mutex;
    std::uintmax_t m_used{10};
};

class FailingRepository : public infrastructure::InMemoryMediaClipRepository {
public:
    std::vector<domain::MediaClip> findPurgeCandidates() override {
        ++calls;
        throw domain::SqliteStorageError("database is locked");
    }
    std::atomic<int> calls{0};
};

std::vector<std::shared_ptr<const domain::IRetentionStrategy>> default_strategies() {
    return {
        std::make_shared<domain::LgpdRetentionStrategy>(domain::RetentionPolicy()),
        std::make_shared<domain::DiskQuotaFifoStrategy>(domain::QuotaPolicy()),
    };
}

// Monta o S4.3 sobre dubles em memoria, para os testes do daemon.
struct DaemonFixture {
    DaemonFixture()
        : repository(std::make_shared<infrastructure::InMemoryMediaClipRepository>()),
          volume(std::make_shared<test::InMemoryMediaVolume>(1000)),
          disk(std::make_shared<AdjustableDiskUsage>()),
          clock(std::make_shared<test::FakeClock>(Clock::now())) {}

    std::shared_ptr<application::PurgeMediaUseCase> useCase(
        std::shared_ptr<domain::IMediaClipRepository> customRepository = nullptr
    ) {
        return std::make_shared<application::PurgeMediaUseCase>(
            customRepository ? customRepository : repository, volume, disk,
            std::make_shared<test::InMemoryPurgeLog>(), clock, default_strategies());
    }

    void storeExpired(const std::string& id) {
        const std::string path = "/nvme/" + id + ".mp4";
        repository->save(clip_at(id, path, clock->now() - 8 * kDay));
        volume->addFile(path, 10);
    }

    std::shared_ptr<infrastructure::InMemoryMediaClipRepository> repository;
    std::shared_ptr<test::InMemoryMediaVolume> volume;
    std::shared_ptr<AdjustableDiskUsage> disk;
    std::shared_ptr<test::FakeClock> clock;
};

infrastructure::PurgeDaemonConfig slow_config() {
    infrastructure::PurgeDaemonConfig config;
    config.retentionSweepInterval = hours(1);
    config.diskCheckInterval = hours(1);
    return config;
}

} // namespace

// ================================================== StatvfsDiskUsageProvider

void test_deve_ler_ocupacao_real_quando_caminho_estiver_no_volume() {
    TempDir dir("s4_statvfs_");
    const infrastructure::StatvfsDiskUsageProvider provider(dir.path());

    const domain::DiskUsage usage = provider.currentUsage();

    ODS_CHECK(usage.capacityBytes() > 0u);
    ODS_CHECK(usage.usedRatio() >= 0.0 && usage.usedRatio() <= 1.0);
}

void test_deve_lancar_excecao_quando_caminho_do_volume_nao_existir() {
    const infrastructure::StatvfsDiskUsageProvider provider("/caminho/que/nao/existe/s4");
    ODS_CHECK(throws<domain::DiskUsageUnavailableError>([&] { (void)provider.currentUsage(); }));
}

// ================================================================ FileStorage

void test_deve_medir_tamanho_real_quando_arquivo_existir() {
    TempDir dir("s4_size_");
    write_file(dir.file("clip.mp4"), 4096);
    const infrastructure::FileStorage storage;

    ODS_CHECK(storage.sizeOf(dir.file("clip.mp4")) == 4096u);
    ODS_CHECK(throws<domain::MediaFileNotFoundError>([&] { (void)storage.sizeOf(dir.file("x.mp4")); }));
}

// ========================================================== JsonLinesPurgeLog

void test_deve_anexar_uma_linha_json_por_expurgo_quando_registrar() {
    TempDir dir("s4_log_");
    const std::string path = dir.file("purge_log.jsonl");
    {
        infrastructure::JsonLinesPurgeLog log(path);
        log.record(sample_entry("clip-1"));
        log.record(sample_entry("clip-2"));
    }
    infrastructure::JsonLinesPurgeLog reopened(path);
    reopened.record(sample_entry("clip-3"));

    const auto lines = read_lines(path);
    ODS_CHECK(lines.size() == 3u);
    ODS_CHECK(lines[0].find("\"clip_id\":\"clip-1\"") != std::string::npos);
    ODS_CHECK(lines[0].find("\"reason\":\"lgpd_retention_expired\"") != std::string::npos);
    ODS_CHECK(lines[0].find("\"file\":\"deleted\"") != std::string::npos);
    ODS_CHECK(lines[0].find("\"bytes_freed\":1048576") != std::string::npos);
    ODS_CHECK(lines[0].find("\"purged_at\":\"2026-09-21T") != std::string::npos);
    ODS_CHECK(lines[0].find("\"event_id\":\"evt-\\\"aspas\\\"\"") != std::string::npos);
    ODS_CHECK(lines[2].find("clip-3") != std::string::npos);
}

void test_deve_lancar_excecao_quando_log_nao_puder_ser_gravado() {
    infrastructure::JsonLinesPurgeLog log("/caminho/que/nao/existe/purge_log.jsonl");
    ODS_CHECK(throws<domain::PurgeLogError>([&] { log.record(sample_entry("clip-1")); }));
}

// ================================= Ponta a ponta: SQLite + arquivos reais

void test_deve_apagar_do_nvme_e_atualizar_sqlite_quando_varredura_real_executar() {
    TempDir dir("s4_purge_e2e_");
    const auto now = Clock::now();
    auto repository = std::make_shared<infrastructure::SQLiteMediaClipRepository>(dir.file("s4.db"));
    repository->save(clip_at("vencido", dir.file("vencido.mp4"), now - 8 * kDay));
    repository->save(clip_at("recente", dir.file("recente.mp4"), now - 1 * kDay));
    repository->save(clip_at("auditoria", dir.file("auditoria.mp4"), now - 30 * kDay, true));
    write_file(dir.file("vencido.mp4"), 2048);
    write_file(dir.file("recente.mp4"), 2048);
    write_file(dir.file("auditoria.mp4"), 2048);

    application::PurgeMediaUseCase purgeMedia(
        repository, std::make_shared<infrastructure::FileStorage>(),
        std::make_shared<AdjustableDiskUsage>(),
        std::make_shared<infrastructure::JsonLinesPurgeLog>(dir.file("purge_log.jsonl")),
        std::make_shared<test::FakeClock>(now), default_strategies());

    const auto report = purgeMedia.execute();

    ODS_CHECK(report.purged.size() == 1u && report.failures.empty());
    ODS_CHECK(!fs::exists(dir.file("vencido.mp4")));
    ODS_CHECK(fs::exists(dir.file("recente.mp4")) && fs::exists(dir.file("auditoria.mp4")));
    ODS_CHECK(!repository->findById("vencido")->isRetained());
    ODS_CHECK(repository->findById("recente")->isRetained());
    ODS_CHECK(repository->findById("auditoria")->isRetained());
    const auto remaining = repository->findPurgeCandidates();
    ODS_CHECK(remaining.size() == 1u && remaining.front().clipId() == "recente");
    const auto lines = read_lines(dir.file("purge_log.jsonl"));
    ODS_CHECK(lines.size() == 1u);
    ODS_CHECK(lines[0].find("\"bytes_freed\":2048") != std::string::npos);
}

// ================================================================ PurgeDaemon

void test_deve_varrer_ao_iniciar_quando_daemon_subir() {
    DaemonFixture fixture;
    fixture.storeExpired("atrasado");
    infrastructure::PurgeDaemon daemon(fixture.useCase(), fixture.disk, domain::QuotaPolicy(), slow_config());

    daemon.start();

    ODS_CHECK(daemon.waitForSweeps(1, seconds(5)));
    ODS_CHECK(!fixture.repository->findById("atrasado")->isRetained());
    daemon.stop();
}

void test_deve_varrer_na_hora_quando_expurgo_for_solicitado() {
    DaemonFixture fixture;
    infrastructure::PurgeDaemon daemon(fixture.useCase(), fixture.disk, domain::QuotaPolicy(), slow_config());
    daemon.start();
    ODS_CHECK(daemon.waitForSweeps(1, seconds(5)));
    fixture.storeExpired("novo_vencido");

    daemon.requestSweep();

    ODS_CHECK(daemon.waitForSweeps(2, seconds(5)));
    ODS_CHECK(!fixture.repository->findById("novo_vencido")->isRetained());
    daemon.stop();
}

void test_deve_varrer_sem_esperar_o_intervalo_quando_disco_atingir_o_gatilho() {
    DaemonFixture fixture;
    infrastructure::PurgeDaemonConfig config;
    config.retentionSweepInterval = hours(1);
    config.diskCheckInterval = milliseconds(20);
    infrastructure::PurgeDaemon daemon(fixture.useCase(), fixture.disk, domain::QuotaPolicy(), config);
    daemon.start();
    ODS_CHECK(daemon.waitForSweeps(1, seconds(5)));

    fixture.disk->setUsedPercent(90);

    ODS_CHECK(daemon.waitForSweeps(2, seconds(5)));
    daemon.stop();
}

void test_deve_parar_rapidamente_quando_daemon_estiver_ocioso() {
    DaemonFixture fixture;
    infrastructure::PurgeDaemon daemon(fixture.useCase(), fixture.disk, domain::QuotaPolicy(), slow_config());
    daemon.start();
    ODS_CHECK(daemon.waitForSweeps(1, seconds(5)));

    const auto begin = std::chrono::steady_clock::now();
    daemon.stop();

    ODS_CHECK(std::chrono::steady_clock::now() - begin < seconds(1));
    ODS_CHECK(!daemon.isRunning());
}

void test_deve_continuar_vivo_quando_varredura_falhar() {
    DaemonFixture fixture;
    auto failing = std::make_shared<FailingRepository>();
    infrastructure::PurgeDaemon daemon(fixture.useCase(failing), fixture.disk, domain::QuotaPolicy(), slow_config());
    daemon.start();
    ODS_CHECK(daemon.waitForSweeps(1, seconds(5)));

    daemon.requestSweep();

    ODS_CHECK(daemon.waitForSweeps(2, seconds(5)));
    ODS_CHECK(failing->calls.load() == 2);
    ODS_CHECK(daemon.isRunning());
    daemon.stop();
}

int main() {
    test_deve_ler_ocupacao_real_quando_caminho_estiver_no_volume();
    test_deve_lancar_excecao_quando_caminho_do_volume_nao_existir();
    test_deve_medir_tamanho_real_quando_arquivo_existir();
    test_deve_anexar_uma_linha_json_por_expurgo_quando_registrar();
    test_deve_lancar_excecao_quando_log_nao_puder_ser_gravado();
    test_deve_apagar_do_nvme_e_atualizar_sqlite_quando_varredura_real_executar();

    test_deve_varrer_ao_iniciar_quando_daemon_subir();
    test_deve_varrer_na_hora_quando_expurgo_for_solicitado();
    test_deve_varrer_sem_esperar_o_intervalo_quando_disco_atingir_o_gatilho();
    test_deve_parar_rapidamente_quando_daemon_estiver_ocioso();
    test_deve_continuar_vivo_quando_varredura_falhar();

    std::cout << "test_s4_retention_runtime: all tests passed\n";
    return 0;
}
