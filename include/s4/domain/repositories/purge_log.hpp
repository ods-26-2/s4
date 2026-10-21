#pragma once

#include "s4/domain/value_objects/purge_log_entry.hpp"

namespace ods::s4::domain {

// Porta de SAIDA: o "Log de Expurgo". Somente anexa — uma linha registrada
// nunca e alterada nem apagada, porque e a prova de que o expurgo aconteceu.
class IPurgeLog {
public:
    virtual ~IPurgeLog() = default;

    virtual void record(const PurgeLogEntry& entry) = 0;
};

} // namespace ods::s4::domain
