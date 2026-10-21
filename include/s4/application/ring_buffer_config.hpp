#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "s4/domain/value_objects/buffer_capacity.hpp"

namespace ods::s4::application {

// Parametros de uma instancia de ring buffer (uma por camera, por build).
//
// A build da aplicacao (P3) preenche esta struct; o S4 nao le arquivo de
// configuracao nem variavel de ambiente por conta propria.
struct RingBufferConfig {
    // Identifica a camera e, por consequencia, o segmento de memoria.
    std::string cameraId{"cam0"};
    // Quanto passado guardar. Deve ser >= o maior preSeconds que alguma
    // aplicacao da build vai pedir, mais a duracao de um GOP.
    double windowSeconds{30.0};
    // Bitrate configurado no encoder de P4; junto com a janela define a RAM.
    std::size_t bitrateBps{4000000};
    // Folga para picos de bitrate em cenas com muito movimento.
    double safetyFactor{1.5};
    // Nome do segmento POSIX; vazio = derivado do cameraId.
    std::string shmName;
    // Remove um segmento /dev/shm orfao antes de criar o novo. Util quando o
    // servico e reiniciado apos uma queda; ver SharedMemoryFrameStore.
    bool replaceStaleSegment{false};
    // Dimensoes anunciadas no VideoFrame entregue ao S4.2.
    std::uint32_t width{1920};
    std::uint32_t height{1080};

    [[nodiscard]] domain::BufferCapacity capacity() const {
        return domain::BufferCapacity::forEncodedStream(bitrateBps, windowSeconds, safetyFactor);
    }

    [[nodiscard]] std::string resolvedShmName() const {
        return shmName.empty() ? ("ods_s4_ring_" + cameraId) : shmName;
    }
};

} // namespace ods::s4::application
