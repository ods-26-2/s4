#pragma once

#include "s4/domain/errors/domain_error.hpp"
#include "s4/domain/value_objects/disk_usage.hpp"

namespace ods::s4::domain {

// Quando o expurgo EMERGENCIAL por espaco comeca e ate onde ele vai.
//
// Dois limiares (histerese) em vez de um: com gatilho = alvo = 85%, cada clipe
// novo empurraria o disco de volta acima do limite e cada varredura apagaria
// uma midia por vez, para sempre. Com alvo abaixo do gatilho o expurgo abre
// folga de uma vez e o disco volta a encher devagar. Gatilho = alvo continua
// permitido para quem quiser apagar o minimo possivel.
class QuotaPolicy {
public:
    static constexpr double kDefaultTriggerRatio = 0.85;
    static constexpr double kDefaultTargetRatio = 0.80;

    explicit QuotaPolicy(
        double triggerRatio = kDefaultTriggerRatio,
        double targetRatio = kDefaultTargetRatio
    ) : m_triggerRatio(triggerRatio), m_targetRatio(targetRatio) {
        if (!(targetRatio > 0.0 && targetRatio <= triggerRatio && triggerRatio <= 1.0)) {
            throw InvalidQuotaPolicyError();
        }
    }

    [[nodiscard]] double triggerRatio() const noexcept { return m_triggerRatio; }
    [[nodiscard]] double targetRatio() const noexcept { return m_targetRatio; }

    [[nodiscard]] bool isExceededBy(const DiskUsage& usage) const noexcept {
        return usage.usedRatio() >= m_triggerRatio;
    }

    [[nodiscard]] bool isSatisfiedBy(const DiskUsage& usage) const noexcept {
        return usage.usedRatio() < m_targetRatio;
    }

private:
    double m_triggerRatio;
    double m_targetRatio;
};

} // namespace ods::s4::domain
