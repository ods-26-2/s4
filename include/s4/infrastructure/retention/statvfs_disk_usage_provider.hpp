#pragma once

#include <string>

#include "s4/domain/services/disk_usage_provider.hpp"

namespace ods::s4::infrastructure {

// Ocupacao do volume via statvfs(2) — a entrada "Estado de ocupacao do NVMe"
// do guia. Basta um caminho qualquer DENTRO do volume (o diretorio de midias):
// o statvfs responde pelo sistema de arquivos inteiro que o contem.
//
// Uma chamada custa microssegundos e nao toca o disco, por isso o daemon pode
// consulta-la a cada poucos segundos.
class StatvfsDiskUsageProvider : public domain::IDiskUsageProvider {
public:
    explicit StatvfsDiskUsageProvider(std::string pathInsideVolume);

    [[nodiscard]] domain::DiskUsage currentUsage() const override;

private:
    std::string m_path;
};

} // namespace ods::s4::infrastructure
