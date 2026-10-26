#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "s4/domain/errors/domain_error.hpp"
#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::domain {

// 6c, ods.p4.frame: pixel_format do quadro entregue por P4 (sempre cru).
enum class RawPixelFormat { NV12, RGB, BGR, GRAY };

[[nodiscard]] inline const char* to_string(RawPixelFormat format) noexcept {
    switch (format) {
        case RawPixelFormat::NV12:
            return "NV12";
        case RawPixelFormat::RGB:
            return "RGB";
        case RawPixelFormat::BGR:
            return "BGR";
        case RawPixelFormat::GRAY:
            return "GRAY";
    }
    return "NV12";
}

// Formato do stream cru combinado com P4: o mesmo para todos os quadros da
// fonte. E o que decide os caps do appsrc e o tamanho esperado de cada buffer.
class RawVideoFormat {
public:
    RawVideoFormat(std::uint32_t width, std::uint32_t height, double fps,
                   RawPixelFormat pixelFormat = RawPixelFormat::NV12)
        : m_width(width), m_height(height), m_fps(fps), m_pixelFormat(pixelFormat) {
        if (width == 0 || height == 0 || fps <= 0.0) {
            throw InvalidRawFrameError("raw video format needs positive width, height and fps");
        }
        // NV12 subamostra o croma em 2x2: dimensao impar nao tem plano UV inteiro.
        if (pixelFormat == RawPixelFormat::NV12 && (width % 2 != 0 || height % 2 != 0)) {
            throw InvalidRawFrameError("NV12 needs even width and height");
        }
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] double fps() const noexcept { return m_fps; }
    [[nodiscard]] RawPixelFormat pixelFormat() const noexcept { return m_pixelFormat; }

    // Bytes de um quadro sem padding de linha (o que P4 entrega no buffer).
    // 1080p NV12 = 3 110 400 B: e esse numero que torna inviavel guardar cru.
    [[nodiscard]] std::size_t frameBytes() const noexcept {
        const std::size_t pixels = static_cast<std::size_t>(m_width) * m_height;
        switch (m_pixelFormat) {
            case RawPixelFormat::NV12:
                return pixels * 3 / 2;
            case RawPixelFormat::RGB:
            case RawPixelFormat::BGR:
                return pixels * 3;
            case RawPixelFormat::GRAY:
                return pixels;
        }
        return pixels * 3 / 2;
    }

    [[nodiscard]] Nanoseconds frameDurationNs() const noexcept {
        return static_cast<Nanoseconds>(static_cast<double>(kNanosecondsPerSecond) / m_fps);
    }

private:
    std::uint32_t m_width;
    std::uint32_t m_height;
    double m_fps;
    RawPixelFormat m_pixelFormat;
};

// 6c, ods.p4.frame: metadados que acompanham cada quadro cru de P4 (e de P6 em
// replay, no mesmo formato). Viajam junto do buffer de video, fora do broker.
//
// Daqui saem as duas coisas que o S4.1 nao pode inventar:
//   * capturedAtUtcNs: o instante oficial da captura (base de tempo de B2),
//     que vira a chave do indice do buffer;
//   * sessionId: a sessao oficial, que zera o buffer em SEEK, LOOP e RESTART.
struct P4FrameMetadata {
    // captured_at do envelope, em ns desde a epoca Unix (UTC).
    Nanoseconds capturedAtUtcNs{0};
    std::string sourceId;
    std::string sessionId;
    // Contador de quadros da sessao, zerado em cada ods.session.started.
    std::uint64_t frameIndex{0};
    std::uint32_t imageWidth{0};
    std::uint32_t imageHeight{0};
    RawPixelFormat pixelFormat{RawPixelFormat::NV12};
    double fps{0.0};
    // Sempre true na 1.0: P4 entrega o quadro sem correcao de distorcao.
    bool raw{true};

    // O quadro descrito cabe no formato combinado? Diferenca de dimensao ou de
    // formato faria o encoder ler o buffer com a geometria errada.
    [[nodiscard]] bool matches(const RawVideoFormat& format) const noexcept {
        return imageWidth == format.width() && imageHeight == format.height() &&
               pixelFormat == format.pixelFormat();
    }
};

} // namespace ods::s4::domain
