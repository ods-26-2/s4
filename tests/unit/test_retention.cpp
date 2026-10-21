// S4.3 — testes de unidade de Retencao & Expurgo: dominio puro e caso de uso
// com dubles em memoria (sem disco, sem SQLite, sem relogio real).
#include "tests/ods_check.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "s4/application/use_cases/purge_media.hpp"
#include "s4/domain/strategies/disk_quota_fifo_strategy.hpp"
#include "s4/domain/strategies/lgpd_retention_strategy.hpp"
#include "s4/domain/value_objects/disk_usage.hpp"
#include "s4/domain/value_objects/quota_policy.hpp"
#include "s4/domain/value_objects/retention_policy.hpp"
#include "tests/unit/fakes/retention_fakes.hpp"

using namespace ods::s4;
using domain::DiskUsage;
using domain::FileOutcome;
using domain::PurgeCandidate;
using domain::PurgeReason;

namespace {

using Clock = std::chrono::system_clock;
using std::chrono::hours;
using std::chrono::seconds;

const Clock::time_point kNow = Clock::time_point(hours(24 * 20000));
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

struct ClipSpec {
    ClipSpec(
        std::string id,
        Clock::duration age = kDay,
        bool isLockedForAudit = false,
        bool isRetained = true,
        std::string extension = ".mp4",
        std::string eventId = ""
    ) : id(std::move(id)), age(age), isLockedForAudit(isLockedForAudit), isRetained(isRetained),
        extension(std::move(extension)), eventId(std::move(eventId)) {}

    std::string id;
    Clock::duration age;
    bool isLockedForAudit;
    bool isRetained;
    std::string extension;
    std::string eventId;
};

std::string path_of(const ClipSpec& spec) {
    return "/nvme/" + (spec.eventId.empty() ? spec.id : spec.eventId) + spec.extension;
}

domain::MediaClip make_clip(const ClipSpec& spec) {
    const auto createdAt = kNow - spec.age;
    return domain::MediaClip(
        spec.id,
        spec.eventId.empty() ? "evt-" + spec.id : spec.eventId,
        path_of(spec),
        "sha-" + spec.id,
        domain::TimeWindow(createdAt - seconds(10), createdAt),
        spec.isRetained,
        spec.isLockedForAudit,
        std::chrono::time_point_cast<Clock::duration>(createdAt)
    );
}

PurgeCandidate candidate(const ClipSpec& spec, std::uintmax_t sizeBytes = 10) {
    return PurgeCandidate{make_clip(spec), sizeBytes};
}

domain::RetentionContext context_with(DiskUsage usage) {
    return domain::RetentionContext{kNow, usage};
}

std::vector<std::string> ids_of(const std::vector<PurgeCandidate>& candidates) {
    std::vector<std::string> ids;
    for (const auto& item : candidates) {
        ids.push_back(item.clip.clipId());
    }
    return ids;
}

// Composicao padrao do S4.3 sobre dubles em memoria.
struct PurgeFixture {
    explicit PurgeFixture(std::uintmax_t bytesUsedByOthers = 0)
        : repository(std::make_shared<infrastructure::InMemoryMediaClipRepository>()),
          volume(std::make_shared<test::InMemoryMediaVolume>(1000, bytesUsedByOthers)),
          log(std::make_shared<test::InMemoryPurgeLog>()),
          clock(std::make_shared<test::FakeClock>(kNow)) {}

    application::PurgeMediaUseCase useCase(
        std::shared_ptr<domain::IMediaClipRepository> customRepository = nullptr
    ) {
        std::vector<std::shared_ptr<const domain::IRetentionStrategy>> strategies{
            std::make_shared<domain::LgpdRetentionStrategy>(domain::RetentionPolicy()),
            std::make_shared<domain::DiskQuotaFifoStrategy>(domain::QuotaPolicy(0.85, 0.80)),
        };
        return application::PurgeMediaUseCase(
            customRepository ? customRepository : repository, volume, volume, log, clock, strategies);
    }

    // Grava o clipe no "SQLite" e o arquivo no "NVMe".
    void store(const ClipSpec& spec, std::uintmax_t sizeBytes) {
        repository->save(make_clip(spec));
        volume->addFile(path_of(spec), sizeBytes);
    }

    [[nodiscard]] bool isRetained(const std::string& clipId) {
        return repository->findById(clipId)->isRetained();
    }

    std::shared_ptr<infrastructure::InMemoryMediaClipRepository> repository;
    std::shared_ptr<test::InMemoryMediaVolume> volume;
    std::shared_ptr<test::InMemoryPurgeLog> log;
    std::shared_ptr<test::FakeClock> clock;
};

} // namespace

// ================================================================= DiskUsage

void test_deve_calcular_ocupacao_como_o_df_quando_houver_usado_e_disponivel() {
    const DiskUsage usage(85, 15);
    ODS_CHECK(usage.capacityBytes() == 100u);
    ODS_CHECK(usage.usedRatio() > 0.8499 && usage.usedRatio() < 0.8501);
}

void test_deve_lancar_excecao_quando_capacidade_reportada_for_zero() {
    ODS_CHECK(throws<domain::InvalidDiskUsageError>([] { DiskUsage(0, 0); }));
}

void test_deve_projetar_ocupacao_menor_quando_bytes_forem_liberados() {
    const DiskUsage projected = DiskUsage(85, 15).afterFreeing(25);
    ODS_CHECK(projected.usedBytes() == 60u);
    ODS_CHECK(projected.availableBytes() == 40u);
}

void test_nao_deve_ficar_negativo_quando_liberar_mais_que_o_usado() {
    const DiskUsage projected = DiskUsage(10, 90).afterFreeing(1000);
    ODS_CHECK(projected.usedBytes() == 0u);
    ODS_CHECK(projected.capacityBytes() == 100u);
}

// =============================================================== QuotaPolicy

void test_deve_considerar_cota_excedida_quando_ocupacao_atingir_exatamente_o_gatilho() {
    const domain::QuotaPolicy policy;
    ODS_CHECK(policy.isExceededBy(DiskUsage(85, 15)));
    ODS_CHECK(!policy.isExceededBy(DiskUsage(84, 16)));
}

void test_deve_usar_85_e_80_por_cento_quando_limiares_nao_forem_informados() {
    const domain::QuotaPolicy policy;
    ODS_CHECK(policy.triggerRatio() == 0.85);
    ODS_CHECK(policy.targetRatio() == 0.80);
}

void test_deve_lancar_excecao_quando_limiares_da_cota_forem_incoerentes() {
    ODS_CHECK(throws<domain::InvalidQuotaPolicyError>([] { domain::QuotaPolicy(0.80, 0.90); }));
    ODS_CHECK(throws<domain::InvalidQuotaPolicyError>([] { domain::QuotaPolicy(1.10, 0.80); }));
    ODS_CHECK(throws<domain::InvalidQuotaPolicyError>([] { domain::QuotaPolicy(0.85, 0.0); }));
}

void test_deve_aceitar_alvo_igual_ao_gatilho_quando_se_quiser_apagar_o_minimo() {
    const domain::QuotaPolicy policy(0.85, 0.85);
    ODS_CHECK(policy.isSatisfiedBy(DiskUsage(84, 16)));
    ODS_CHECK(!policy.isSatisfiedBy(DiskUsage(85, 15)));
}

// =========================================================== RetentionPolicy

void test_deve_usar_sete_dias_quando_prazo_nao_for_informado() {
    const domain::RetentionPolicy policy;
    ODS_CHECK(policy.maxAgeFor(domain::MediaKind::VideoClip) == hours(24 * 7));
    ODS_CHECK(policy.maxAgeFor(domain::MediaKind::Snapshot) == hours(24 * 7));
}

void test_deve_considerar_vencida_quando_idade_for_exatamente_o_prazo() {
    const domain::RetentionPolicy policy;
    ODS_CHECK(policy.hasExpired(make_clip({"a", 7 * kDay}), kNow));
    ODS_CHECK(!policy.hasExpired(make_clip({"b", 7 * kDay - seconds(1)}), kNow));
}

void test_deve_aplicar_prazo_de_snapshot_quando_arquivo_for_imagem() {
    const domain::RetentionPolicy policy(hours(24 * 7), hours(24));
    ODS_CHECK(policy.hasExpired(make_clip({"snap", 2 * kDay, false, true, ".jpg"}), kNow));
    ODS_CHECK(!policy.hasExpired(make_clip({"clip", 2 * kDay, false, true, ".mp4"}), kNow));
}

void test_deve_classificar_tipo_de_midia_quando_extensao_tiver_maiusculas() {
    ODS_CHECK(domain::media_kind_of("/data/media/Evt.JPG") == domain::MediaKind::Snapshot);
    ODS_CHECK(domain::media_kind_of("/data/media/evt.jpeg") == domain::MediaKind::Snapshot);
    ODS_CHECK(domain::media_kind_of("/data/media/evt.mp4") == domain::MediaKind::VideoClip);
    ODS_CHECK(domain::media_kind_of("/data/media/sem_extensao") == domain::MediaKind::VideoClip);
}

void test_deve_lancar_excecao_quando_prazo_de_retencao_nao_for_positivo() {
    ODS_CHECK(throws<domain::InvalidRetentionPolicyError>([] { domain::RetentionPolicy(seconds(0)); }));
    ODS_CHECK(throws<domain::InvalidRetentionPolicyError>(
        [] { domain::RetentionPolicy(hours(1), seconds(-1)); }));
}

void test_nao_deve_vencer_quando_created_at_estiver_no_futuro() {
    const domain::RetentionPolicy policy;
    ODS_CHECK(!policy.hasExpired(make_clip({"futuro", -kDay}), kNow));
}

// ===================================================== LgpdRetentionStrategy

void test_deve_selecionar_apenas_midias_vencidas_quando_houver_mistura() {
    const domain::LgpdRetentionStrategy strategy{domain::RetentionPolicy()};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"velho", 8 * kDay}), candidate({"novo", 1 * kDay}), candidate({"antigo", 30 * kDay})},
        context_with(DiskUsage(10, 90)));

    ODS_CHECK((ids_of(selected) == std::vector<std::string>{"velho", "antigo"}));
    ODS_CHECK(strategy.reason() == PurgeReason::RetentionPeriodExpired);
}

void test_nao_deve_selecionar_midia_travada_para_auditoria_quando_prazo_vencer() {
    const domain::LgpdRetentionStrategy strategy{domain::RetentionPolicy()};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"travado", 30 * kDay, true})}, context_with(DiskUsage(99, 1)));
    ODS_CHECK(selected.empty());
}

void test_nao_deve_selecionar_midia_ja_expurgada_quando_prazo_vencer() {
    const domain::LgpdRetentionStrategy strategy{domain::RetentionPolicy()};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"expurgado", 30 * kDay, false, false})}, context_with(DiskUsage(10, 90)));
    ODS_CHECK(selected.empty());
}

// ===================================================== DiskQuotaFifoStrategy

void test_nao_deve_selecionar_nada_quando_disco_estiver_abaixo_do_gatilho() {
    const domain::DiskQuotaFifoStrategy strategy{domain::QuotaPolicy()};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"a", 5 * kDay})}, context_with(DiskUsage(84, 16)));
    ODS_CHECK(selected.empty());
}

void test_deve_selecionar_do_mais_antigo_ao_mais_novo_ate_o_alvo_quando_disco_atingir_o_gatilho() {
    const domain::DiskQuotaFifoStrategy strategy{domain::QuotaPolicy(0.85, 0.80)};
    // 90% usado; cada clipe libera 6%: 90 -> 84 -> 78 (< 80), entao dois saem.
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"novo", 1 * kDay}, 6), candidate({"mais_velho", 3 * kDay}, 6),
         candidate({"meio", 2 * kDay}, 6)},
        context_with(DiskUsage(90, 10)));

    ODS_CHECK((ids_of(selected) == std::vector<std::string>{"mais_velho", "meio"}));
    ODS_CHECK(strategy.reason() == PurgeReason::DiskQuotaExceeded);
}

void test_deve_pular_midia_travada_quando_expurgo_fifo_for_emergencial() {
    const domain::DiskQuotaFifoStrategy strategy{domain::QuotaPolicy(0.85, 0.80)};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"mais_velho_travado", 9 * kDay, true}, 50), candidate({"meio", 2 * kDay}, 20),
         candidate({"novo", 1 * kDay}, 20)},
        context_with(DiskUsage(90, 10)));

    ODS_CHECK((ids_of(selected) == std::vector<std::string>{"meio"}));
}

void test_deve_selecionar_todas_quando_nem_apagando_tudo_o_alvo_for_alcancado() {
    const domain::DiskQuotaFifoStrategy strategy{domain::QuotaPolicy()};
    const auto selected = strategy.selectClipsToPurge(
        {candidate({"a", 3 * kDay}, 1), candidate({"b", 2 * kDay}, 1), candidate({"c", 1 * kDay}, 1)},
        context_with(DiskUsage(95, 5)));
    ODS_CHECK(selected.size() == 3u);
}

void test_deve_lancar_disk_quota_exceeded_quando_disco_estiver_cheio_e_so_houver_midia_travada() {
    const domain::DiskQuotaFifoStrategy strategy{domain::QuotaPolicy()};
    ODS_CHECK(throws<domain::DiskQuotaExceededError>([&] {
        (void)strategy.selectClipsToPurge({candidate({"travado", 3 * kDay, true}, 50)},
                                          context_with(DiskUsage(95, 5)));
    }));
    ODS_CHECK(throws<domain::DiskQuotaExceededError>(
        [&] { (void)strategy.selectClipsToPurge({}, context_with(DiskUsage(95, 5))); }));
}

// ========================================================= PurgeMediaUseCase

void test_deve_apagar_arquivo_e_marcar_como_nao_retido_quando_prazo_lgpd_vencer() {
    PurgeFixture fixture;
    fixture.store({"vencido", 8 * kDay}, 100);

    const auto report = fixture.useCase().execute();

    ODS_CHECK(!fixture.volume->exists("/nvme/vencido.mp4"));
    ODS_CHECK(!fixture.isRetained("vencido"));
    ODS_CHECK(report.purged.size() == 1u);
    ODS_CHECK(fixture.log->entries.size() == 1u);
    const auto& entry = fixture.log->entries.front();
    ODS_CHECK(entry.clipId == "vencido");
    ODS_CHECK(entry.eventId == "evt-vencido");
    ODS_CHECK(entry.reason == PurgeReason::RetentionPeriodExpired);
    ODS_CHECK(entry.fileOutcome == FileOutcome::Deleted);
    ODS_CHECK(entry.bytesFreed == 100u);
    ODS_CHECK(entry.purgedAt == kNow);
}

void test_deve_manter_midia_quando_prazo_ainda_nao_venceu() {
    PurgeFixture fixture;
    fixture.store({"recente", 6 * kDay}, 100);

    const auto report = fixture.useCase().execute();

    ODS_CHECK(fixture.volume->exists("/nvme/recente.mp4"));
    ODS_CHECK(fixture.isRetained("recente"));
    ODS_CHECK(report.purged.empty() && fixture.log->entries.empty());
}

void test_deve_manter_midia_travada_para_auditoria_quando_prazo_vencer() {
    PurgeFixture fixture;
    fixture.store({"prova", 30 * kDay, true}, 100);

    (void)fixture.useCase().execute();

    ODS_CHECK(fixture.volume->exists("/nvme/prova.mp4"));
    ODS_CHECK(fixture.isRetained("prova"));
}

void test_deve_expurgar_fifo_ate_o_alvo_quando_disco_atingir_o_gatilho() {
    PurgeFixture fixture(700);
    fixture.store({"d4", 4 * kDay}, 50);
    fixture.store({"d3", 3 * kDay}, 50);
    fixture.store({"d2", 2 * kDay}, 50);
    fixture.store({"d1", 1 * kDay}, 50);  // 900/1000 = 90%

    const auto report = fixture.useCase().execute();

    // 90% -> 85% -> 80% -> 75% (< 80%): os tres mais antigos saem.
    ODS_CHECK(!fixture.isRetained("d4") && !fixture.isRetained("d3") && !fixture.isRetained("d2"));
    ODS_CHECK(fixture.isRetained("d1"));
    ODS_CHECK(report.countPurgedBy(PurgeReason::DiskQuotaExceeded) == 3u);
    ODS_CHECK(report.diskUsageBefore.usedBytes() == 900u);
    ODS_CHECK(report.diskUsageAfter.usedBytes() == 750u);
    ODS_CHECK(fixture.log->entries.front().clipId == "d4");
}

void test_deve_descontar_o_espaco_liberado_pelo_lgpd_antes_de_aplicar_a_cota() {
    PurgeFixture fixture(720);
    fixture.store({"vencido", 8 * kDay}, 100);
    fixture.store({"novo_a", 2 * kDay}, 20);
    fixture.store({"novo_b", 1 * kDay}, 20);  // 860/1000 = 86%

    const auto report = fixture.useCase().execute();

    // O LGPD sozinho leva o disco a 76%: a cota nao precisa apagar nada novo.
    ODS_CHECK(report.countPurgedBy(PurgeReason::RetentionPeriodExpired) == 1u);
    ODS_CHECK(report.countPurgedBy(PurgeReason::DiskQuotaExceeded) == 0u);
    ODS_CHECK(fixture.isRetained("novo_a") && fixture.isRetained("novo_b"));
}

void test_deve_concluir_expurgo_quando_arquivo_ja_nao_existir() {
    PurgeFixture fixture;
    fixture.repository->save(make_clip({"sem_arquivo", 8 * kDay}));

    const auto report = fixture.useCase().execute();

    ODS_CHECK(!fixture.isRetained("sem_arquivo"));
    ODS_CHECK(report.purged.front().fileOutcome == FileOutcome::AlreadyMissing);
    ODS_CHECK(report.purged.front().bytesFreed == 0u);
}

void test_deve_registrar_falha_e_seguir_quando_arquivo_nao_puder_ser_apagado() {
    PurgeFixture fixture;
    fixture.store({"protegido", 9 * kDay}, 100);
    fixture.store({"comum", 8 * kDay}, 100);
    fixture.volume->failRemovalOf("/nvme/protegido.mp4");

    const auto report = fixture.useCase().execute();

    ODS_CHECK(report.failures.size() == 1u);
    ODS_CHECK(report.failures.front().clipId == "protegido");
    ODS_CHECK(fixture.isRetained("protegido"));
    ODS_CHECK(!fixture.isRetained("comum"));
    ODS_CHECK(fixture.log->entries.size() == 1u);
}

void test_nao_deve_apagar_quando_clipe_foi_travado_entre_a_consulta_e_o_expurgo() {
    PurgeFixture fixture;
    auto repository = std::make_shared<test::StaleCandidatesRepository>();
    repository->save(make_clip({"alvo", 8 * kDay, true}));
    repository->staleCandidates = {make_clip({"alvo", 8 * kDay, false})};
    fixture.volume->addFile("/nvme/alvo.mp4", 100);

    const auto report = fixture.useCase(repository).execute();

    ODS_CHECK(report.skippedNoLongerEligible == 1u);
    ODS_CHECK(report.purged.empty());
    ODS_CHECK(fixture.volume->exists("/nvme/alvo.mp4"));
}

void test_nao_deve_apagar_arquivo_quando_outro_clipe_retido_apontar_para_ele() {
    PurgeFixture fixture;
    fixture.store({"antigo", 8 * kDay, false, true, ".mp4", "evt-repetido"}, 100);
    fixture.repository->save(make_clip({"reenvio", 1 * kDay, true, true, ".mp4", "evt-repetido"}));

    const auto report = fixture.useCase().execute();

    ODS_CHECK(!fixture.isRetained("antigo"));
    ODS_CHECK(report.purged.front().fileOutcome == FileOutcome::KeptForOtherClip);
    ODS_CHECK(report.purged.front().bytesFreed == 0u);
    ODS_CHECK(fixture.volume->exists("/nvme/evt-repetido.mp4"));
}

void test_nao_deve_expurgar_de_novo_quando_varredura_for_repetida() {
    PurgeFixture fixture;
    fixture.store({"vencido", 8 * kDay}, 100);
    auto useCase = fixture.useCase();

    (void)useCase.execute();
    const auto second = useCase.execute();

    ODS_CHECK(second.purged.empty());
    ODS_CHECK(fixture.log->entries.size() == 1u);
}

void test_deve_lancar_disk_quota_exceeded_depois_de_expurgar_o_vencido_quando_so_restar_midia_travada() {
    PurgeFixture fixture(900);
    fixture.store({"vencido", 8 * kDay}, 10);
    fixture.store({"travado", 1 * kDay, true}, 50);  // 960/1000

    ODS_CHECK(throws<domain::DiskQuotaExceededError>([&] { (void)fixture.useCase().execute(); }));

    ODS_CHECK(!fixture.volume->exists("/nvme/vencido.mp4"));
    ODS_CHECK(fixture.log->entries.size() == 1u);
    ODS_CHECK(fixture.volume->exists("/nvme/travado.mp4"));
}

void test_deve_reportar_falha_quando_log_de_expurgo_nao_puder_ser_gravado() {
    PurgeFixture fixture;
    fixture.store({"vencido", 8 * kDay}, 100);
    fixture.log->isFull = true;

    const auto report = fixture.useCase().execute();

    ODS_CHECK(!fixture.isRetained("vencido"));
    ODS_CHECK(report.purged.size() == 1u);
    ODS_CHECK(report.failures.size() == 1u);
    ODS_CHECK(report.failures.front().message.find("not logged") != std::string::npos);
}

int main() {
    test_deve_calcular_ocupacao_como_o_df_quando_houver_usado_e_disponivel();
    test_deve_lancar_excecao_quando_capacidade_reportada_for_zero();
    test_deve_projetar_ocupacao_menor_quando_bytes_forem_liberados();
    test_nao_deve_ficar_negativo_quando_liberar_mais_que_o_usado();

    test_deve_considerar_cota_excedida_quando_ocupacao_atingir_exatamente_o_gatilho();
    test_deve_usar_85_e_80_por_cento_quando_limiares_nao_forem_informados();
    test_deve_lancar_excecao_quando_limiares_da_cota_forem_incoerentes();
    test_deve_aceitar_alvo_igual_ao_gatilho_quando_se_quiser_apagar_o_minimo();

    test_deve_usar_sete_dias_quando_prazo_nao_for_informado();
    test_deve_considerar_vencida_quando_idade_for_exatamente_o_prazo();
    test_deve_aplicar_prazo_de_snapshot_quando_arquivo_for_imagem();
    test_deve_classificar_tipo_de_midia_quando_extensao_tiver_maiusculas();
    test_deve_lancar_excecao_quando_prazo_de_retencao_nao_for_positivo();
    test_nao_deve_vencer_quando_created_at_estiver_no_futuro();

    test_deve_selecionar_apenas_midias_vencidas_quando_houver_mistura();
    test_nao_deve_selecionar_midia_travada_para_auditoria_quando_prazo_vencer();
    test_nao_deve_selecionar_midia_ja_expurgada_quando_prazo_vencer();

    test_nao_deve_selecionar_nada_quando_disco_estiver_abaixo_do_gatilho();
    test_deve_selecionar_do_mais_antigo_ao_mais_novo_ate_o_alvo_quando_disco_atingir_o_gatilho();
    test_deve_pular_midia_travada_quando_expurgo_fifo_for_emergencial();
    test_deve_selecionar_todas_quando_nem_apagando_tudo_o_alvo_for_alcancado();
    test_deve_lancar_disk_quota_exceeded_quando_disco_estiver_cheio_e_so_houver_midia_travada();

    test_deve_apagar_arquivo_e_marcar_como_nao_retido_quando_prazo_lgpd_vencer();
    test_deve_manter_midia_quando_prazo_ainda_nao_venceu();
    test_deve_manter_midia_travada_para_auditoria_quando_prazo_vencer();
    test_deve_expurgar_fifo_ate_o_alvo_quando_disco_atingir_o_gatilho();
    test_deve_descontar_o_espaco_liberado_pelo_lgpd_antes_de_aplicar_a_cota();
    test_deve_concluir_expurgo_quando_arquivo_ja_nao_existir();
    test_deve_registrar_falha_e_seguir_quando_arquivo_nao_puder_ser_apagado();
    test_nao_deve_apagar_quando_clipe_foi_travado_entre_a_consulta_e_o_expurgo();
    test_nao_deve_apagar_arquivo_quando_outro_clipe_retido_apontar_para_ele();
    test_nao_deve_expurgar_de_novo_quando_varredura_for_repetida();
    test_deve_lancar_disk_quota_exceeded_depois_de_expurgar_o_vencido_quando_so_restar_midia_travada();
    test_deve_reportar_falha_quando_log_de_expurgo_nao_puder_ser_gravado();

    std::cout << "test_s4_retention: all tests passed\n";
    return 0;
}
