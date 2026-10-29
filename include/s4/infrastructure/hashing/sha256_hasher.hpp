#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ods::s4::infrastructure {

[[nodiscard]] std::string compute_sha256_hex(const std::vector<std::uint8_t>& data);

} // namespace ods::s4::infrastructure