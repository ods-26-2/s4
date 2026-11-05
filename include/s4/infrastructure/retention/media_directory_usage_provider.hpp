#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "s4/domain/services/disk_usage_provider.hpp"

namespace ods::s4::infrastructure {

// Cota propria do S4 (opcional, --media-quota-bytes).
//
// O statvfs mede o NVMe inteiro, que e dividido com as outras aplicacoes da
// Jetson: o S4 pode ser obrigado a apagar evidencia porque OUTRO processo
// encheu o disco, e pode encher o disco sozinho sem passar do gatilho se os
// outros estiverem vazios. Esta porta mede so o diretorio de midias do S4
// contra um teto em bytes: "usado" e a soma dos arquivos regulares do
// diretorio, a capacidade e o teto (o usado pode passar dele: 1200/1000 e
// 120%, e a projecao da cota mede contra os 1000).
class MediaDirectoryUsageProvider : public domain::IDiskUsageProvider {
public:
    MediaDirectoryUsageProvider(std::string mediaDir, std::uintmax_t quotaBytes);

    [[nodiscard]] domain::DiskUsage currentUsage() const override;

private:
    std::string m_mediaDir;
    std::uintmax_t m_quotaBytes;
};

// Com as duas medidas ligadas, vale a mais apertada: a cota do S4 nao pode
// servir de desculpa para deixar o NVMe encher, nem o contrario.
class MostPressingDiskUsageProvider : public domain::IDiskUsageProvider {
public:
    explicit MostPressingDiskUsageProvider(
        std::vector<std::shared_ptr<const domain::IDiskUsageProvider>> providers);

    [[nodiscard]] domain::DiskUsage currentUsage() const override;

private:
    std::vector<std::shared_ptr<const domain::IDiskUsageProvider>> m_providers;
};

} // namespace ods::s4::infrastructure
