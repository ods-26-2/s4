#pragma once

#include <cstdint>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

// Nanossegundos no relogio de CAPTURA de P4 (nao no relogio de parede).
using Nanoseconds = std::int64_t;

constexpr Nanoseconds kNanosecondsPerSecond = 1000000000LL;

// Janela no relogio de captura, usada DENTRO do ring buffer.
//
// Por que existir junto de TimeWindow?
//   * TimeWindow  -> std::chrono::system_clock (relogio de parede). E o que a
//     aplicacao e a API REST usam, e o que aparece no descritor do clipe.
//   * CaptureWindow -> inteiros em nanossegundos do relogio monotonico de
//     captura, que e a base que P4 carimba em cada quadro e que B2 propaga.
// Sao grandezas diferentes: o relogio de parede pode ser ajustado por NTP e
// retroceder; o de captura, nao. A conversao entre os dois acontece em um
// unico ponto (CaptureClockBridge), e nao espalhada pelo codigo.
class CaptureWindow {
public:
    CaptureWindow(Nanoseconds startNs, Nanoseconds endNs)
        : m_startNs(startNs), m_endNs(endNs) {
        if (m_startNs >= m_endNs) {
            throw InvalidTimeWindowError("startNs must be strictly before endNs");
        }
    }

    // Janela [T - pre, T + post] em torno do instante do evento.
    [[nodiscard]] static CaptureWindow around(
        Nanoseconds centerNs,
        double preSeconds,
        double postSeconds
    ) {
        if (preSeconds < 0.0 || postSeconds < 0.0) {
            throw InvalidTimeWindowError("preSeconds and postSeconds must not be negative");
        }
        const auto preNs = static_cast<Nanoseconds>(preSeconds * kNanosecondsPerSecond);
        const auto postNs = static_cast<Nanoseconds>(postSeconds * kNanosecondsPerSecond);
        return CaptureWindow(centerNs - preNs, centerNs + postNs);
    }

    [[nodiscard]] Nanoseconds startNs() const noexcept { return m_startNs; }
    [[nodiscard]] Nanoseconds endNs() const noexcept { return m_endNs; }

    [[nodiscard]] double durationSeconds() const noexcept {
        return static_cast<double>(m_endNs - m_startNs) / kNanosecondsPerSecond;
    }

    [[nodiscard]] bool contains(Nanoseconds timestampNs) const noexcept {
        return m_startNs <= timestampNs && timestampNs <= m_endNs;
    }

    [[nodiscard]] bool overlaps(const CaptureWindow& other) const noexcept {
        return m_startNs <= other.m_endNs && other.m_startNs <= m_endNs;
    }

private:
    Nanoseconds m_startNs;
    Nanoseconds m_endNs;
};

} // namespace ods::s4::domain
