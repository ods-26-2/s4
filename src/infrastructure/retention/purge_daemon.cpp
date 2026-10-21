#include "s4/infrastructure/retention/purge_daemon.hpp"

#include <cstdio>
#include <exception>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

void print_report(const application::PurgeReport& report) {
    if (report.purged.empty() && report.failures.empty()) {
        return;
    }
    std::fprintf(stderr,
                 "[S4.3] sweep: %zu purged (%zu lgpd, %zu quota), %ju bytes freed, "
                 "disk %.1f%% -> %.1f%%, %zu failed\n",
                 report.purged.size(),
                 report.countPurgedBy(domain::PurgeReason::RetentionPeriodExpired),
                 report.countPurgedBy(domain::PurgeReason::DiskQuotaExceeded),
                 report.bytesFreed(),
                 report.diskUsageBefore.usedRatio() * 100.0,
                 report.diskUsageAfter.usedRatio() * 100.0,
                 report.failures.size());
    for (const auto& failure : report.failures) {
        std::fprintf(stderr, "[S4.3] purge failed for %s (%s): %s\n", failure.clipId.c_str(),
                     failure.filePath.c_str(), failure.message.c_str());
    }
}

} // namespace

PurgeDaemon::PurgeDaemon(
    std::shared_ptr<application::PurgeMediaUseCase> purgeMedia,
    std::shared_ptr<domain::IDiskUsageProvider> diskUsage,
    domain::QuotaPolicy quotaPolicy,
    PurgeDaemonConfig config
) : m_purgeMedia(std::move(purgeMedia)),
    m_diskUsage(std::move(diskUsage)),
    m_quotaPolicy(quotaPolicy),
    m_config(config) {}

PurgeDaemon::~PurgeDaemon() {
    stop();
}

// ------------------------------------------------------------- ciclo de vida

void PurgeDaemon::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isRunning.exchange(true)) {
        return;
    }
    m_isStopRequested = false;
    m_thread = std::thread([this] { run(); });
}

void PurgeDaemon::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isRunning.load()) {
            return;
        }
        m_isStopRequested = true;
    }
    m_wakeUp.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_isRunning.store(false);
}

void PurgeDaemon::requestSweep() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_isSweepRequested = true;
    }
    m_wakeUp.notify_all();
}

// ------------------------------------------------------------------- inspecao

std::uint64_t PurgeDaemon::sweepsCompleted() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sweepsCompleted;
}

bool PurgeDaemon::waitForSweeps(std::uint64_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_sweepFinished.wait_for(lock, timeout, [this, count] { return m_sweepsCompleted >= count; });
}

// -------------------------------------------------------------------- laco

void PurgeDaemon::run() {
    SteadyTime lastSweep{};
    bool hasSwept = false;
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_isStopRequested) {
        const bool wasRequested = std::exchange(m_isSweepRequested, false);
        lock.unlock();
        if (isSweepDue(wasRequested, lastSweep, hasSwept)) {
            sweep();
            lastSweep = std::chrono::steady_clock::now();
            hasSwept = true;
        }
        lock.lock();
        m_wakeUp.wait_for(lock, m_config.diskCheckInterval,
                          [this] { return m_isStopRequested || m_isSweepRequested; });
    }
}

// steady_clock para o agendamento: um ajuste do relogio de parede (NTP) nao
// pode adiantar nem atrasar a proxima varredura.
bool PurgeDaemon::isSweepDue(bool wasRequested, SteadyTime lastSweep, bool hasSwept) const {
    if (wasRequested || !hasSwept) {
        return true;
    }
    const bool isRetentionDue =
        std::chrono::steady_clock::now() - lastSweep >= m_config.retentionSweepInterval;
    return isRetentionDue || isDiskOverQuota();
}

bool PurgeDaemon::isDiskOverQuota() const {
    try {
        return m_quotaPolicy.isExceededBy(m_diskUsage->currentUsage());
    } catch (const domain::ODSBaseException& error) {
        std::fprintf(stderr, "[S4.3] disk check failed: %s\n", error.what());
        return false;
    }
}

// Ultima linha de defesa da thread: uma excecao que escapasse daqui chamaria
// std::terminate e derrubaria o servico inteiro, inclusive a API do S4.4.
void PurgeDaemon::sweep() {
    try {
        print_report(m_purgeMedia->execute());
    } catch (const domain::DiskQuotaExceededError& error) {
        std::fprintf(stderr, "[S4.3] ALERT: %s\n", error.what());
    } catch (const domain::ODSBaseException& error) {
        std::fprintf(stderr, "[S4.3] sweep failed: %s\n", error.what());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[S4.3] sweep failed unexpectedly: %s\n", error.what());
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_sweepsCompleted;
    }
    m_sweepFinished.notify_all();
}

} // namespace ods::s4::infrastructure
