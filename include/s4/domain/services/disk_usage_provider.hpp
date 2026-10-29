#pragma once

#include "s4/domain/value_objects/disk_usage.hpp"

namespace ods::s4::domain {

// Porta de SAIDA: ocupacao atual do volume de midias (statvfs na Jetson).
class IDiskUsageProvider {
public:
    virtual ~IDiskUsageProvider() = default;

    [[nodiscard]] virtual DiskUsage currentUsage() const = 0;
};

} // namespace ods::s4::domain
