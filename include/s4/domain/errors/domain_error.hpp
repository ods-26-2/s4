#pragma once

#include <string>
#include <stdexcept>

namespace ods::s4::domain {

class ODSBaseException : public std::runtime_error {
public:
    explicit ODSBaseException(const std::string& message)
        : std::runtime_error(message) {}
};

class DomainError : public ODSBaseException {
public:
    explicit DomainError(const std::string& message)
        : ODSBaseException(message) {}
};

class InvalidTimeWindowError : public DomainError {
public:
    explicit InvalidTimeWindowError(const std::string& message = "start_time must be strictly before end_time")
        : DomainError(message) {}
};

class DiskQuotaExceededError : public DomainError {
public:
    explicit DiskQuotaExceededError(const std::string& message = "NVMe disk quota exceeded with no purgeable media")
        : DomainError(message) {}
};

// --- S4.1 Ring Buffer: violacoes de regra do buffer circular ----------------

class InvalidCapacityError : public DomainError {
public:
    explicit InvalidCapacityError(const std::string& message = "Ring buffer capacity must be positive")
        : DomainError(message) {}
};

class FrameTooLargeError : public DomainError {
public:
    explicit FrameTooLargeError(const std::string& message = "Frame does not fit in the ring buffer arena")
        : DomainError(message) {}
};

// B2: busca, laco e reinicio nunca fazem o relogio retroceder silenciosamente.
class NonMonotonicTimestampError : public DomainError {
public:
    explicit NonMonotonicTimestampError(const std::string& message = "Capture timestamp went backwards within the same session")
        : DomainError(message) {}
};

class WindowNotInBufferError : public DomainError {
public:
    explicit WindowNotInBufferError(const std::string& message = "Requested window is no longer (or not yet) in the ring buffer")
        : DomainError(message) {}
};

// A janela existe no buffer, mas nao ha keyframe que permita decodifica-la.
class NoDecodableStartError : public DomainError {
public:
    explicit NoDecodableStartError(const std::string& message = "No keyframe available to decode the requested window")
        : DomainError(message) {}
};

// --- S4.3 Retencao & Expurgo: politicas invalidas ---------------------------

class InvalidRetentionPolicyError : public DomainError {
public:
    explicit InvalidRetentionPolicyError(const std::string& message = "Retention period must be positive")
        : DomainError(message) {}
};

class InvalidQuotaPolicyError : public DomainError {
public:
    explicit InvalidQuotaPolicyError(const std::string& message = "Quota ratios must satisfy 0 < target <= trigger <= 1")
        : DomainError(message) {}
};

class InvalidDiskUsageError : public DomainError {
public:
    explicit InvalidDiskUsageError(const std::string& message = "Disk usage must report a positive capacity")
        : DomainError(message) {}
};

class ApplicationError : public ODSBaseException {
public:
    explicit ApplicationError(const std::string& message)
        : ODSBaseException(message) {}
};

class MediaFileNotFoundError : public ApplicationError {
public:
    explicit MediaFileNotFoundError(const std::string& message = "Media file not found on storage")
        : ApplicationError(message) {}
};

class MediaBufferEmptyError : public ApplicationError {
public:
    explicit MediaBufferEmptyError(const std::string& message = "No media frames available in the requested time window")
        : ApplicationError(message) {}
};

class HashingError : public ApplicationError {
public:
    explicit HashingError(const std::string& message = "Failed to compute cryptographic hash")
        : ApplicationError(message) {}
};

class BufferNotRunningError : public ApplicationError {
public:
    explicit BufferNotRunningError(const std::string& message = "Ring buffer capture has not been started")
        : ApplicationError(message) {}
};

class FrameSourceError : public ApplicationError {
public:
    explicit FrameSourceError(const std::string& message = "Video frame source failed")
        : ApplicationError(message) {}
};

class FrameStoreError : public ApplicationError {
public:
    explicit FrameStoreError(const std::string& message = "Shared memory arena failed")
        : ApplicationError(message) {}
};

class SqliteStorageError : public ApplicationError {
public:
    explicit SqliteStorageError(const std::string& message = "SQLite storage error")
        : ApplicationError(message) {}
};

class FileOperationError : public ApplicationError {
public:
    explicit FileOperationError(const std::string& message = "File operation failed")
        : ApplicationError(message) {}
};

// --- S4.3 Retencao & Expurgo: falhas de infraestrutura ----------------------

class DiskUsageUnavailableError : public ApplicationError {
public:
    explicit DiskUsageUnavailableError(const std::string& message = "Cannot read NVMe disk usage")
        : ApplicationError(message) {}
};

class PurgeLogError : public ApplicationError {
public:
    explicit PurgeLogError(const std::string& message = "Cannot append to the purge log")
        : ApplicationError(message) {}
};

} // namespace ods::s4::domain
