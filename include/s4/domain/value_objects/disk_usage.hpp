#pragma once

#include <algorithm>
#include <cstdint>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

// Ocupacao do volume NVMe onde as midias ficam, como o statvfs a reporta.
//
// "Capacidade" aqui e usado + disponivel, e nao o tamanho bruto do disco: e a
// mesma conta do `df`, que ignora os blocos reservados ao root. Assim 85% neste
// objeto e 85% no `df` que o operador ve na Jetson.
class DiskUsage {
public:
    DiskUsage(std::uintmax_t usedBytes, std::uintmax_t availableBytes)
        : DiskUsage(usedBytes, availableBytes, usedBytes + availableBytes) {}

    // Cota com teto fixo (--media-quota-bytes): o usado pode PASSAR da
    // capacidade, e a projecao precisa continuar medindo contra o teto — com
    // (usado, 0) a capacidade viraria o proprio usado e a cota apagaria menos
    // do que precisa.
    static DiskUsage ofQuota(std::uintmax_t usedBytes, std::uintmax_t capacityBytes) {
        return DiskUsage(usedBytes, usedBytes < capacityBytes ? capacityBytes - usedBytes : 0,
                         capacityBytes);
    }

    [[nodiscard]] std::uintmax_t usedBytes() const noexcept { return m_usedBytes; }
    [[nodiscard]] std::uintmax_t availableBytes() const noexcept { return m_availableBytes; }
    [[nodiscard]] std::uintmax_t capacityBytes() const noexcept { return m_capacityBytes; }

    // Pode passar de 1.0 numa cota estourada.
    [[nodiscard]] double usedRatio() const noexcept {
        return static_cast<double>(m_usedBytes) / static_cast<double>(m_capacityBytes);
    }

    // Projecao de como o volume fica depois de apagar "bytes": e o que permite
    // a estrategia de cota decidir QUANTAS midias apagar antes de apagar.
    [[nodiscard]] DiskUsage afterFreeing(std::uintmax_t bytes) const {
        const std::uintmax_t freed = std::min(bytes, m_usedBytes);
        return ofQuota(m_usedBytes - freed, m_capacityBytes);
    }

private:
    DiskUsage(std::uintmax_t usedBytes, std::uintmax_t availableBytes, std::uintmax_t capacityBytes)
        : m_usedBytes(usedBytes), m_availableBytes(availableBytes), m_capacityBytes(capacityBytes) {
        if (capacityBytes == 0) {
            throw InvalidDiskUsageError();
        }
    }

    std::uintmax_t m_usedBytes;
    std::uintmax_t m_availableBytes;
    std::uintmax_t m_capacityBytes;
};

} // namespace ods::s4::domain
