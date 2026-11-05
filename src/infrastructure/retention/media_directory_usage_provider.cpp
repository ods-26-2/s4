#include "s4/infrastructure/retention/media_directory_usage_provider.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace fs = std::filesystem;

MediaDirectoryUsageProvider::MediaDirectoryUsageProvider(std::string mediaDir, std::uintmax_t quotaBytes)
    : m_mediaDir(std::move(mediaDir)), m_quotaBytes(quotaBytes) {
    if (m_quotaBytes == 0) {
        throw domain::InvalidDiskUsageError();
    }
}

// Arquivos que somem no meio da varredura (expurgo ou escrita em curso) sao
// ignorados: a proxima medida os conta de novo.
domain::DiskUsage MediaDirectoryUsageProvider::currentUsage() const {
    std::error_code error;
    std::uintmax_t usedBytes = 0;
    if (fs::exists(m_mediaDir, error)) {
        for (fs::recursive_directory_iterator it(m_mediaDir, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code entryError;
            if (it->is_regular_file(entryError)) {
                const std::uintmax_t size = it->file_size(entryError);
                if (!entryError) usedBytes += size;
            }
        }
        if (error) {
            throw domain::DiskUsageUnavailableError(
                "cannot measure '" + m_mediaDir + "': " + error.message());
        }
    }
    return domain::DiskUsage::ofQuota(usedBytes, m_quotaBytes);
}

MostPressingDiskUsageProvider::MostPressingDiskUsageProvider(
    std::vector<std::shared_ptr<const domain::IDiskUsageProvider>> providers)
    : m_providers(std::move(providers)) {
    if (m_providers.empty()) {
        throw domain::InvalidDiskUsageError();
    }
}

domain::DiskUsage MostPressingDiskUsageProvider::currentUsage() const {
    domain::DiskUsage worst = m_providers.front()->currentUsage();
    for (std::size_t i = 1; i < m_providers.size(); ++i) {
        const domain::DiskUsage usage = m_providers[i]->currentUsage();
        if (usage.usedRatio() > worst.usedRatio()) worst = usage;
    }
    return worst;
}

} // namespace ods::s4::infrastructure
