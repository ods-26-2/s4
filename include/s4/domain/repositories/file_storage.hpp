#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ods::s4::domain {

class IFileStorage {
public:
    virtual ~IFileStorage() = default;

    virtual void write(const std::string& path, const std::vector<std::uint8_t>& data) = 0;
    [[nodiscard]] virtual bool exists(const std::string& path) const = 0;
    // Bytes ocupados pelo arquivo (S4.3 projeta o espaco liberado pelo
    // expurgo). Lanca MediaFileNotFoundError se o arquivo nao existir.
    [[nodiscard]] virtual std::uintmax_t sizeOf(const std::string& path) const = 0;
    virtual void remove(const std::string& path) = 0;
};

} // namespace ods::s4::domain
