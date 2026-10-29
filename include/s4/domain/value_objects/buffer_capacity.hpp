#pragma once

#include <cstddef>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

constexpr int kBitsPerByte = 8;

// Converte "quero N segundos de video" em bytes de RAM.
//
// As fabricas existem para que a decisao do tamanho da janela seja tomada em
// termos de TEMPO (15 s? 30 s?), que e como o professor e o guia a descrevem,
// e nao em bytes crus escolhidos no chute.
class BufferCapacity {
public:
    explicit BufferCapacity(std::size_t totalBytes) : m_totalBytes(totalBytes) {
        if (totalBytes == 0) {
            throw InvalidCapacityError();
        }
    }

    // Stream comprimido (H.264/H.265): bytes = bitrate x segundos x folga.
    // A folga cobre picos de bitrate em cenas com muito movimento, quando o
    // encoder produz mais bytes do que o bitrate medio configurado.
    [[nodiscard]] static BufferCapacity forEncodedStream(
        std::size_t bitrateBps,
        double windowSeconds,
        double safetyFactor = 1.5
    ) {
        const double bytes =
            static_cast<double>(bitrateBps) / kBitsPerByte * windowSeconds * safetyFactor;
        return BufferCapacity(static_cast<std::size_t>(bytes));
    }

    // Stream sem compressao: bytes = tamanho do quadro x fps x segundos.
    // Existe para deixar explicito, em numero, por que NAO guardamos video cru.
    [[nodiscard]] static BufferCapacity forRawStream(
        std::size_t width,
        std::size_t height,
        double bytesPerPixel,
        double fps,
        double windowSeconds
    ) {
        const double frameBytes = static_cast<double>(width * height) * bytesPerPixel;
        return BufferCapacity(static_cast<std::size_t>(frameBytes * fps * windowSeconds));
    }

    [[nodiscard]] std::size_t totalBytes() const noexcept { return m_totalBytes; }

    [[nodiscard]] double megabytes() const noexcept {
        return static_cast<double>(m_totalBytes) / (1024.0 * 1024.0);
    }

private:
    std::size_t m_totalBytes;
};

} // namespace ods::s4::domain
