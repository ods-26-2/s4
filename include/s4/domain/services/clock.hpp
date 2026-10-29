#pragma once

#include <chrono>

namespace ods::s4::domain {

// Porta de SAIDA: "que horas sao agora" no relogio de parede.
//
// Existe para que o prazo LGPD (7 dias) seja testavel sem esperar 7 dias: os
// testes injetam um relogio parado ou adiantado.
class IClock {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    virtual ~IClock() = default;

    [[nodiscard]] virtual TimePoint now() const = 0;
};

} // namespace ods::s4::domain
