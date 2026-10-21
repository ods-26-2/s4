#pragma once

#include <chrono>
#include <mutex>

#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::infrastructure {

// Traduz entre os dois relogios do S4 — e o UNICO lugar do componente onde
// essa conversao acontece.
//
// Por que existem dois relogios:
//   * Dentro do buffer, o tempo e o de CAPTURA (monotonico, em ns), carimbado
//     por P4 em cada quadro. E o que B2 propaga e o unico que nao retrocede.
//   * Na fronteira com o S4.2 e com a API REST, o tempo e std::chrono::
//     system_clock (relogio de parede), que e o que aparece para o usuario e
//     no descritor do clipe.
//
// Como a traducao e feita: no PRIMEIRO quadro de cada sessao guardamos o par
// (relogio de parede agora, capture_ts daquele quadro). A partir dai a
// conversao e uma soma. Ancorar por sessao e o que faz o replay de P6
// funcionar: uma sessao nova reancora, sem misturar as bases.
//
// Quando TRA-1/TRA-2 publicarem a base de tempo oficial de B2, e esta classe
// que muda — nada mais.
class CaptureClockBridge {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    // Registra o quadro corrente; so o primeiro de cada sessao ancora.
    void anchorIfNeeded(domain::Nanoseconds captureTsNs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_anchored) {
            return;
        }
        m_anchorWallClock = std::chrono::system_clock::now();
        m_anchorCaptureTsNs = captureTsNs;
        m_anchored = true;
    }

    // Sessao nova (replay, busca, reinicio): descarta a ancora antiga.
    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_anchored = false;
    }

    [[nodiscard]] bool isAnchored() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_anchored;
    }

    [[nodiscard]] domain::Nanoseconds toCaptureTsNs(TimePoint wallClock) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(
            wallClock - m_anchorWallClock
        ).count();
        return m_anchorCaptureTsNs + static_cast<domain::Nanoseconds>(delta);
    }

    [[nodiscard]] TimePoint toWallClock(domain::Nanoseconds captureTsNs) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto delta = std::chrono::nanoseconds(captureTsNs - m_anchorCaptureTsNs);
        return m_anchorWallClock + std::chrono::duration_cast<TimePoint::duration>(delta);
    }

private:
    mutable std::mutex m_mutex;
    bool m_anchored{false};
    TimePoint m_anchorWallClock{};
    domain::Nanoseconds m_anchorCaptureTsNs{0};
};

} // namespace ods::s4::infrastructure
