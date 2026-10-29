#pragma once

#include "s4/domain/errors/domain_error.hpp"
#include "s4/domain/repositories/file_storage.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ods::s4::test {

class InMemoryFileStorage : public domain::IFileStorage {
public:
    void write(const std::string& path, const std::vector<std::uint8_t>& data) override {
        m_files[path] = data;
    }

    [[nodiscard]] bool exists(const std::string& path) const override {
        return m_files.find(path) != m_files.end();
    }

    [[nodiscard]] std::uintmax_t sizeOf(const std::string& path) const override {
        const auto it = m_files.find(path);
        if (it == m_files.end()) {
            throw domain::MediaFileNotFoundError("file not stored in fake: " + path);
        }
        return it->second.size();
    }

    void remove(const std::string& path) override {
        m_files.erase(path);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& contents(const std::string& path) const {
        const auto it = m_files.find(path);
        if (it == m_files.end()) {
            throw domain::FileOperationError("file not stored in fake: " + path);
        }
        return it->second;
    }

private:
    std::unordered_map<std::string, std::vector<std::uint8_t>> m_files;
};

} // namespace ods::s4::test