#pragma once

#include "s4/domain/repositories/file_storage.hpp"

namespace ods::s4::infrastructure {

class FileStorage : public domain::IFileStorage {
public:
    void write(const std::string& path, const std::vector<std::uint8_t>& data) override;
    [[nodiscard]] bool exists(const std::string& path) const override;
    [[nodiscard]] std::uintmax_t sizeOf(const std::string& path) const override;
    void remove(const std::string& path) override;
};

} // namespace ods::s4::infrastructure