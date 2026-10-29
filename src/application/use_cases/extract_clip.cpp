#include "s4/application/use_cases/extract_clip.hpp"

#include "s4/infrastructure/hashing/sha256_hasher.hpp"

#include <uuid/uuid.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace ods::s4::application {

namespace {

std::string generate_clip_id() {
    uuid_t uuid;
    char buffer[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, buffer);
    return std::string("clip-") + buffer;
}

} // namespace

ExtractClipUseCase::ExtractClipUseCase(
    std::shared_ptr<domain::IMediaClipRepository> repository,
    std::shared_ptr<domain::IMediaBufferReader> buffer,
    std::shared_ptr<domain::IFileStorage> fileStorage
) : m_repository(std::move(repository)),
    m_buffer(std::move(buffer)),
    m_fileStorage(std::move(fileStorage)) {}

ExtractClipUseCase::ExtractClipUseCase(
    std::shared_ptr<domain::IMediaClipRepository> repository,
    std::shared_ptr<domain::IMediaBufferReader> buffer,
    std::shared_ptr<domain::IFileStorage> fileStorage,
    std::shared_ptr<ICaptureBufferReader> captureBuffer,
    std::shared_ptr<IMediaMuxer> muxer
) : m_repository(std::move(repository)),
    m_buffer(std::move(buffer)),
    m_fileStorage(std::move(fileStorage)),
    m_captureBuffer(std::move(captureBuffer)),
    m_muxer(std::move(muxer)) {}

domain::MediaClip ExtractClipUseCase::execute(
    const std::string& eventId,
    const domain::TimeWindow& timeWindow,
    const std::string& destinationPath
) {
    const auto frames = m_buffer->readWindow(timeWindow.startTime(), timeWindow.endTime());
    if (frames.empty()) {
        throw domain::MediaBufferEmptyError("no frames available in the requested time window");
    }

    std::vector<std::uint8_t> payload;
    std::size_t totalSize = 0;
    for (const auto& frame : frames) {
        totalSize += frame.size();
    }
    payload.reserve(totalSize);
    for (const auto& frame : frames) {
        payload.insert(payload.end(), frame.begin(), frame.end());
    }

    m_fileStorage->write(destinationPath, payload);
    const std::string sha256Hash = infrastructure::compute_sha256_hex(payload);

    domain::MediaClip clip(generate_clip_id(), eventId, destinationPath, sha256Hash, timeWindow);
    m_repository->save(clip);
    return clip;
}

domain::MediaClip ExtractClipUseCase::executeCaptureWindow(
    const std::string& eventId,
    const domain::CaptureWindow& window,
    const std::string& destinationPath,
    double waitTimeoutSeconds
) {
    if (!supportsCaptureWindows()) {
        // Falhar e melhor que o caminho legado: o legado gravaria bytes
        // Annex-B com extensao .mp4, que e exatamente o defeito que esta
        // entrega vem para remover.
        throw MediaMuxError(
            "capture-window extraction requires an ICaptureBufferReader and an IMediaMuxer"
        );
    }

    // A espera pelo fim da janela acontece DENTRO da porta: o pos-evento ainda
    // esta sendo capturado quando o pedido chega.
    const ExtractedSegment segment = m_captureBuffer->extractCaptureWindow(window, waitTimeoutSeconds);
    if (segment.frames.empty()) {
        throw domain::MediaBufferEmptyError("no frames available in the requested capture window");
    }

    // O muxer e quem decide o container. Se o segmento nao virar um MP4 valido,
    // o pedido falha em vez de deixar um arquivo com o nome errado em disco.
    const std::vector<std::uint8_t> container = m_muxer->mux(segment);
    if (container.empty()) {
        throw MediaMuxError("muxer produced an empty container");
    }

    m_fileStorage->write(destinationPath, container);
    const std::string sha256Hash = infrastructure::compute_sha256_hex(container);

    // O descritor e em relogio de parede (ISO-8601 do evento), mas a janela
    // pedida vive no relogio de captura. A conversao fica na porta.
    const domain::TimeWindow wallWindow(
        m_captureBuffer->toWallClock(window.startNs()),
        m_captureBuffer->toWallClock(window.endNs())
    );

    domain::MediaClip clip(
        generate_clip_id(),
        eventId,
        destinationPath,
        sha256Hash,
        wallWindow
    );
    m_repository->save(clip);
    return clip;
}

} // namespace ods::s4::application
