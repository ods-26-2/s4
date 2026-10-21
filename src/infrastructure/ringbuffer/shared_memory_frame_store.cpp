#include "s4/infrastructure/ringbuffer/shared_memory_frame_store.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

// shm_open exige nome comecando por '/'.
std::string posix_name(const std::string& name) {
    return name.front() == '/' ? name : ("/" + name);
}

} // namespace

SharedMemoryFrameStore::SharedMemoryFrameStore(
    const std::string& name,
    std::size_t capacityBytes,
    bool create,
    bool replaceExisting
) : m_name(name), m_capacity(capacityBytes), m_ownsSegment(create) {
    if (capacityBytes == 0) {
        throw domain::InvalidCapacityError("capacityBytes must be positive");
    }

    const std::string shmName = posix_name(name);
    if (create && replaceExisting) {
        // Remove um segmento orfao deixado por um processo que morreu sem
        // limpar. shm_unlink em nome inexistente e inofensivo.
        ::shm_unlink(shmName.c_str());
    }
    const int flags = create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR;
    m_fd = ::shm_open(shmName.c_str(), flags, 0600);
    if (m_fd < 0) {
        const std::string reason = (create && errno == EEXIST)
            ? "' already exists (another instance running, or a stale segment: "
              "remove /dev/shm/" + name + " or enable replaceStaleSegment)"
            : "'";
        throw domain::FrameStoreError("cannot open shared memory segment '" + shmName + reason);
    }
    if (create && ::ftruncate(m_fd, static_cast<off_t>(capacityBytes)) != 0) {
        ::close(m_fd);
        ::shm_unlink(shmName.c_str());
        throw domain::FrameStoreError("cannot size shared memory segment '" + shmName + "'");
    }

    void* mapped = ::mmap(nullptr, capacityBytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(m_fd);
        if (create) {
            ::shm_unlink(shmName.c_str());
        }
        throw domain::FrameStoreError("cannot map shared memory segment '" + shmName + "'");
    }
    m_arena = static_cast<std::uint8_t*>(mapped);
}

SharedMemoryFrameStore::~SharedMemoryFrameStore() {
    close();
}

void SharedMemoryFrameStore::write(std::size_t offset, const std::uint8_t* data, std::size_t length) {
    rejectOutOfBounds(offset, offset + length);
    std::memcpy(m_arena + offset, data, length);
}

std::vector<std::uint8_t> SharedMemoryFrameStore::read(std::size_t offset, std::size_t length) const {
    rejectOutOfBounds(offset, offset + length);
    return std::vector<std::uint8_t>(m_arena + offset, m_arena + offset + length);
}

void SharedMemoryFrameStore::rejectOutOfBounds(std::size_t start, std::size_t end) const {
    if (m_isClosed || m_arena == nullptr) {
        throw domain::FrameStoreError("arena '" + m_name + "' is already closed");
    }
    if (end > m_capacity || end < start) {
        throw domain::FrameStoreError(
            "access outside the arena of " + std::to_string(m_capacity) + " bytes"
        );
    }
}

// Libera o mapeamento e, se este processo criou o segmento, o remove do
// sistema. Chamado pelo destrutor: RAII garante que o /dev/shm nao vaza nem
// quando uma excecao sobe pela pilha.
void SharedMemoryFrameStore::close() {
    if (m_isClosed) {
        return;
    }
    m_isClosed = true;
    if (m_arena != nullptr) {
        ::munmap(m_arena, m_capacity);
    }
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    if (m_ownsSegment) {
        ::shm_unlink(posix_name(m_name).c_str());
    }
    m_arena = nullptr;
}

std::string SharedMemoryFrameStore::path() const {
    return "/dev/shm/" + m_name;
}

} // namespace ods::s4::infrastructure
