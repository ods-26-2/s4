#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "s4/application/use_cases/purge_media.hpp"
#include "s4/domain/services/disk_usage_provider.hpp"
#include "s4/domain/value_objects/quota_policy.hpp"

namespace ods::s4::infrastructure {

// Frequencia da rotina de limpeza (topico de discussao do guia).
//
// Duas cadencias, porque as duas regras envelhecem em ritmos diferentes:
//   * o prazo LGPD e medido em dias — varrer de hora em hora sobra;
//   * o disco pode encher em minutos se varias aplicacoes gravarem clipes ao
//     mesmo tempo — por isso um statvfs barato a cada 30 s, que dispara a
//     varredura NA HORA quando o gatilho de cota e atingido.
struct PurgeDaemonConfig {
    std::chrono::milliseconds retentionSweepInterval{std::chrono::hours(1)};
    std::chrono::milliseconds diskCheckInterval{std::chrono::seconds(30)};
};

// S4.3 — Purge Manager Daemon (Daemon Worker do guia).
//
// Uma thread que decide QUANDO varrer e delega O QUE apagar ao
// PurgeMediaUseCase. Varre:
//   * ao iniciar (recupera o atraso de quando o servico estava parado);
//   * a cada retentionSweepInterval;
//   * assim que o disco atinge o gatilho da QuotaPolicy;
//   * quando alguem chama requestSweep() (ex.: logo apos gravar um clipe).
// Nenhuma excecao derruba a thread: falhas viram linhas "[S4.3]" no stderr e a
// proxima varredura tenta de novo.
class PurgeDaemon {
public:
    PurgeDaemon(
        std::shared_ptr<application::PurgeMediaUseCase> purgeMedia,
        std::shared_ptr<domain::IDiskUsageProvider> diskUsage,
        domain::QuotaPolicy quotaPolicy,
        PurgeDaemonConfig config = {}
    );
    ~PurgeDaemon();

    PurgeDaemon(const PurgeDaemon&) = delete;
    PurgeDaemon& operator=(const PurgeDaemon&) = delete;

    void start();
    // Espera a varredura em curso terminar; nunca interrompe um expurgo no meio.
    void stop();
    // Gatilho por evento: varre na proxima oportunidade, sem esperar o intervalo.
    void requestSweep();

    [[nodiscard]] bool isRunning() const noexcept { return m_isRunning.load(); }
    [[nodiscard]] std::uint64_t sweepsCompleted() const;
    // Bloqueia ate "count" varreduras terminarem ou o timeout vencer.
    [[nodiscard]] bool waitForSweeps(std::uint64_t count, std::chrono::milliseconds timeout) const;

private:
    using SteadyTime = std::chrono::steady_clock::time_point;

    void run();
    [[nodiscard]] bool isSweepDue(bool wasRequested, SteadyTime lastSweep, bool hasSwept) const;
    [[nodiscard]] bool isDiskOverQuota() const;
    void sweep();

    std::shared_ptr<application::PurgeMediaUseCase> m_purgeMedia;
    std::shared_ptr<domain::IDiskUsageProvider> m_diskUsage;
    domain::QuotaPolicy m_quotaPolicy;
    PurgeDaemonConfig m_config;

    mutable std::mutex m_mutex;
    std::condition_variable m_wakeUp;
    mutable std::condition_variable m_sweepFinished;
    bool m_isStopRequested{false};
    bool m_isSweepRequested{false};
    std::uint64_t m_sweepsCompleted{0};
    std::atomic<bool> m_isRunning{false};
    std::thread m_thread;
};

} // namespace ods::s4::infrastructure
