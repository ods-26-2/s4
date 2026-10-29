#include "s4/infrastructure/filesystem/file_storage.hpp"

#include "s4/domain/errors/domain_error.hpp"

#include <filesystem>
#include <fstream>

namespace ods::s4::infrastructure {

void FileStorage::write(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw domain::FileOperationError("cannot open file for writing: " + path);
    }
    out.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size())
    );
    if (!out) {
        throw domain::FileOperationError("failed writing file: " + path);
    }
}

bool FileStorage::exists(const std::string& path) const {
    std::error_code ec;
    const bool result = std::filesystem::exists(path, ec);
    return !ec && result;
}

std::uintmax_t FileStorage::sizeOf(const std::string& path) const {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        throw domain::MediaFileNotFoundError("media file not found: " + path);
    }
    if (ec) {
        throw domain::FileOperationError("cannot read size of file: " + path);
    }
    return size;
}

void FileStorage::remove(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        throw domain::FileOperationError("cannot remove file: " + path);
    }
}

} // namespace ods::s4::infrastructure