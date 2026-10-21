#include "s4/infrastructure/retention/statvfs_disk_usage_provider.hpp"

#include <sys/statvfs.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

StatvfsDiskUsageProvider::StatvfsDiskUsageProvider(std::string pathInsideVolume)
    : m_path(std::move(pathInsideVolume)) {}

// Mesma conta do `df`: usado = blocos - livres; disponivel = livres para
// usuario comum (f_bavail), que exclui a reserva do root.
domain::DiskUsage StatvfsDiskUsageProvider::currentUsage() const {
    struct statvfs volume {};
    if (::statvfs(m_path.c_str(), &volume) != 0) {
        throw domain::DiskUsageUnavailableError(
            "statvfs('" + m_path + "') failed: " + std::strerror(errno)
        );
    }
    const std::uintmax_t blockSize = volume.f_frsize != 0 ? volume.f_frsize : volume.f_bsize;
    const std::uintmax_t usedBytes =
        static_cast<std::uintmax_t>(volume.f_blocks - volume.f_bfree) * blockSize;
    const std::uintmax_t availableBytes = static_cast<std::uintmax_t>(volume.f_bavail) * blockSize;
    return domain::DiskUsage(usedBytes, availableBytes);
}

} // namespace ods::s4::infrastructure
