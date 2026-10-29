#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ods::s4::domain {

// Porta de SAIDA: a arena de bytes onde os quadros realmente ficam.
//
// O dominio so precisa de duas operacoes: "escreva estes bytes neste offset" e
// "leia N bytes daquele offset". Se a arena e /dev/shm, um vetor de teste ou um
// mmap de arquivo, o RingBuffer nao muda uma linha — e o que permite testar
// toda a logica circular sem sistema operacional no caminho.
class IFrameStore {
public:
    virtual ~IFrameStore() = default;

    [[nodiscard]] virtual std::size_t capacityBytes() const = 0;
    virtual void write(std::size_t offset, const std::uint8_t* data, std::size_t length) = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> read(std::size_t offset, std::size_t length) const = 0;
    virtual void close() = 0;
};

} // namespace ods::s4::domain
