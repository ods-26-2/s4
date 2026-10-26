#pragma once

#include <chrono>
#include <mutex>

#include "s4/domain/services/frame_source.hpp"
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
// (relogio de parede, capture_ts daquele quadro). A partir dai a conversao e
// uma soma. Ancorar por sessao e o que faz o replay de P6 funcionar: uma sessao
// nova reancora, sem misturar as bases.
//
// O relogio de parede da ancora depende da base de tempo do quadro:
//   * CapturedAtUtc (ods.p4.frame): o capture_ts JA E o captured_at em UTC, a
//     base oficial de B2. A ancora e o proprio instante do quadro e a traducao
//     vira identidade — o started_at de um evento do S2 e o instante do quadro
//     no indice sao o mesmo numero, sem a latencia de entrega no meio.
//   * SourceClock (fontes sem metadados): o relogio da fonte nao tem relacao
//     com UTC, entao a ancora e o "agora" do S4 ao receber o primeiro quadro.
class CaptureClockBridge {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    // Registra o quadro corrente; so o primeiro de cada sessao ancora (ou o
    // primeiro de uma base de tempo diferente da ancorada).
    void anchorIfNeeded(
        domain::Nanoseconds captureTsNs,
        domain::CaptureTimeBase timeBase = domain::CaptureTimeBase::SourceClock
    ) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_anchored && m_timeBase == timeBase) {
            return;
        }
        m_timeBase = timeBase;
        m_anchorCaptureTsNs = captureTsNs;
        m_anchorWallClock = timeBase == domain::CaptureTimeBase::CapturedAtUtc
            ? TimePoint(std::chrono::duration_cast<TimePoint::duration>(
                  std::chrono::nanoseconds(captureTsNs)))
            : std::chrono::system_clock::now();
        m_anchored = true;
    }

    [[nodiscard]] domain::CaptureTimeBase timeBase() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_timeBase;
    }

    // Sessao nova (replay, busca, reinicio): descarta a ancora antiga. Com a
    // base oficial a ancora e identidade e vale para qualquer sessao: mante-la
    // evita que um descritor montado logo apos um SEEK fique sem traducao ate
    // o primeiro quadro da sessao nova chegar.
    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_timeBase != domain::CaptureTimeBase::CapturedAtUtc) {
            m_anchored = false;
        }
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
    domain::CaptureTimeBase m_timeBase{domain::CaptureTimeBase::SourceClock};
    TimePoint m_anchorWallClock{};
    domain::Nanoseconds m_anchorCaptureTsNs{0};
};

} // namespace ods::s4::infrastructure
