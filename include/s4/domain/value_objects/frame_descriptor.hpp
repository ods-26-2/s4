#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::domain {

// A "ficha de catalogo" de um quadro: e a tupla (timestamp, pointer, length)
// pedida pelo guia para a saida do S4.1.
//
// O ring buffer guarda APENAS descritores; os bytes ficam na arena
// (IFrameStore). "offset" e a posicao relativa ao inicio da arena, e nunca um
// endereco de processo: assim o descritor continua valido em qualquer processo
// que mapeie o mesmo segmento de memoria compartilhada.
struct FrameDescriptor {
    std::uint64_t sequence{0};      // ordem global de chegada, nunca reiniciada
    Nanoseconds captureTsNs{0};     // instante da captura em P4
    std::size_t offset{0};          // "pointer": posicao na arena
    std::size_t length{0};          // bytes do quadro
    bool isKeyframe{false};         // I-frame (decodificavel sozinho)?
    std::string sessionId;          // sessao de execucao ou de replay (B2)

    [[nodiscard]] std::size_t endOffset() const noexcept { return offset + length; }

    // True se o quadro compartilha ao menos um byte com a regiao [start, end).
    // E o teste que decide quem precisa ser expulso quando um quadro novo e
    // escrito por cima.
    [[nodiscard]] bool occupies(std::size_t start, std::size_t end) const noexcept {
        return offset < end && start < endOffset();
    }
};

} // namespace ods::s4::domain
