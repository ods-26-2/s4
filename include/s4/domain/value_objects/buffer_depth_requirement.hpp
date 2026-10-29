#pragma once

#include <string>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

// Quanto passado o ring buffer precisa segurar para que nenhum clipe chegue sem
// o seu comeco.
//
// Por que nao basta "pre-rolagem": o evento do S2 so e publicado DEPOIS da
// persistencia minima da regra. No exemplo da 6c (luva), a condicao comeca as
// 14:29:52 e o evento sai por volta das 14:30:00; com 5 s de pre-rolagem o
// clipe comeca as 14:29:47 — 13 s antes de o S4 saber que o evento existe.
//
// E por que somar o post_s: a extracao so acontece quando o pos-evento ja foi
// capturado. Enquanto o S4 espera esse "depois", o buffer continua expulsando
// o mais antigo, e o comeco da janela precisa sobreviver ate la.
//
//   profundidade >= pre-rolagem + persistencia da regra mais longa
//                   + latencia de entrega do evento + post_s maximo + 1 GOP
//
// O GOP entra porque o trecho recua ate o keyframe anterior ao inicio pedido.
class BufferDepthRequirement {
public:
    BufferDepthRequirement(
        double preRollSeconds,
        double longestRulePersistenceSeconds,
        double eventDeliveryLatencySeconds,
        double maxPostSeconds,
        double gopSeconds
    ) : m_preRollSeconds(preRollSeconds),
        m_longestRulePersistenceSeconds(longestRulePersistenceSeconds),
        m_eventDeliveryLatencySeconds(eventDeliveryLatencySeconds),
        m_maxPostSeconds(maxPostSeconds),
        m_gopSeconds(gopSeconds) {
        if (preRollSeconds < 0.0 || longestRulePersistenceSeconds < 0.0 ||
            eventDeliveryLatencySeconds < 0.0 || maxPostSeconds < 0.0 || gopSeconds < 0.0) {
            throw InvalidBufferDepthError();
        }
    }

    [[nodiscard]] double requiredSeconds() const noexcept {
        return m_preRollSeconds + m_longestRulePersistenceSeconds + m_eventDeliveryLatencySeconds +
               m_maxPostSeconds + m_gopSeconds;
    }

    [[nodiscard]] bool isSatisfiedBy(double windowSeconds) const noexcept {
        return windowSeconds >= requiredSeconds();
    }

    // Termo a termo, para a mensagem de erro dizer QUAL parcela pesou.
    [[nodiscard]] std::string describe() const {
        return std::to_string(m_preRollSeconds) + " s pre-roll + " +
               std::to_string(m_longestRulePersistenceSeconds) + " s rule persistence + " +
               std::to_string(m_eventDeliveryLatencySeconds) + " s event latency + " +
               std::to_string(m_maxPostSeconds) + " s max post + " +
               std::to_string(m_gopSeconds) + " s GOP = " +
               std::to_string(requiredSeconds()) + " s";
    }

    [[nodiscard]] double preRollSeconds() const noexcept { return m_preRollSeconds; }
    [[nodiscard]] double longestRulePersistenceSeconds() const noexcept {
        return m_longestRulePersistenceSeconds;
    }
    [[nodiscard]] double eventDeliveryLatencySeconds() const noexcept {
        return m_eventDeliveryLatencySeconds;
    }
    [[nodiscard]] double maxPostSeconds() const noexcept { return m_maxPostSeconds; }
    [[nodiscard]] double gopSeconds() const noexcept { return m_gopSeconds; }

private:
    double m_preRollSeconds;
    double m_longestRulePersistenceSeconds;
    double m_eventDeliveryLatencySeconds;
    double m_maxPostSeconds;
    double m_gopSeconds;
};

} // namespace ods::s4::domain
