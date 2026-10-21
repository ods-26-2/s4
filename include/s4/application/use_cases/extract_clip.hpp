#pragma once

#include <memory>
#include <string>

#include "s4/application/dtos/extracted_segment.hpp"
#include "s4/application/services/capture_buffer_reader.hpp"
#include "s4/application/services/media_muxer.hpp"
#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/repositories/file_storage.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"
#include "s4/domain/services/media_buffer_reader.hpp"
#include "s4/domain/value_objects/capture_window.hpp"
#include "s4/domain/value_objects/time_window.hpp"

namespace ods::s4::application {

// S4.2 — Caso de uso: materializar o clipe de evidencia de um evento.
//
// Dois caminhos, porque ha dois relogios e dois formatos:
//
//   execute(...)              caminho legado: janela em system_clock, leitura
//                             por IMediaBufferReader, bytes concatenados.
//   executeCaptureWindow(...) caminho do fluxo real: janela em relogio de
//                             captura, espera do pos-evento e muxing para um
//                             container valido.
//
// executeCaptureWindow e o caminho do fluxo end-to-end. Ele so funciona se a
// infraestrutura injetar as duas portas (ICaptureBufferReader e IMediaMuxer);
// sem elas o caso de uso se recusa a servir o pedido, em vez de gravar um
// arquivo com extensao .mp4 que nao e MP4. execute() permanece para os testes
// antigos e para quem so tem o contrato de relogio de parede.
class ExtractClipUseCase {
public:
    ExtractClipUseCase(
        std::shared_ptr<domain::IMediaClipRepository> repository,
        std::shared_ptr<domain::IMediaBufferReader> buffer,
        std::shared_ptr<domain::IFileStorage> fileStorage
    );

    // Construtor do fluxo real: recebe as portas de captura e de muxing.
    ExtractClipUseCase(
        std::shared_ptr<domain::IMediaClipRepository> repository,
        std::shared_ptr<domain::IMediaBufferReader> buffer,
        std::shared_ptr<domain::IFileStorage> fileStorage,
        std::shared_ptr<ICaptureBufferReader> captureBuffer,
        std::shared_ptr<IMediaMuxer> muxer
    );

    domain::MediaClip execute(
        const std::string& eventId,
        const domain::TimeWindow& timeWindow,
        const std::string& destinationPath
    );

    // Janela em relogio de captura. waitTimeoutSeconds e o teto de espera pelo
    // pos-evento: enquanto a janela nao for alcancada, a thread do pedido
    // espera em vez de devolver um clipe sem a parte que importa.
    domain::MediaClip executeCaptureWindow(
        const std::string& eventId,
        const domain::CaptureWindow& window,
        const std::string& destinationPath,
        double waitTimeoutSeconds = 0.0
    );

    // Indica se o fluxo real esta disponivel (as duas portas injetadas).
    [[nodiscard]] bool supportsCaptureWindows() const noexcept {
        return m_captureBuffer != nullptr && m_muxer != nullptr;
    }

    [[nodiscard]] std::shared_ptr<ICaptureBufferReader> captureBuffer() const noexcept {
        return m_captureBuffer;
    }

private:
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
    std::shared_ptr<domain::IMediaBufferReader> m_buffer;
    std::shared_ptr<domain::IFileStorage> m_fileStorage;
    std::shared_ptr<ICaptureBufferReader> m_captureBuffer;
    std::shared_ptr<IMediaMuxer> m_muxer;
};

} // namespace ods::s4::application
