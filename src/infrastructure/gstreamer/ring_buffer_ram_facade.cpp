#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"
#include "s4/infrastructure/ringbuffer/shared_memory_frame_store.hpp"

namespace ods::s4::infrastructure {

using domain::CaptureWindow;
using domain::CapturedFrame;
using domain::Nanoseconds;

RingBufferRAMFacade::RingBufferRAMFacade(
    application::RingBufferConfig config,
    std::unique_ptr<domain::IFrameSource> frameSource,
    std::unique_ptr<domain::IFrameStore> frameStore
) : m_config(std::move(config)),
    m_source(std::move(frameSource)),
    m_store(std::move(frameStore)),
    m_ringBuffer(std::make_unique<domain::RingBuffer>(m_config.capacity())),
    m_ingest(std::make_unique<application::IngestFrameUseCase>(*m_ringBuffer, *m_store)),
    m_extract(std::make_unique<application::ExtractCaptureWindowUseCase>(*m_ringBuffer, *m_store)) {}

RingBufferRAMFacade::RingBufferRAMFacade(
    application::RingBufferConfig config,
    std::unique_ptr<domain::IFrameSource> frameSource
) : RingBufferRAMFacade(
        config,
        std::move(frameSource),
        std::make_unique<SharedMemoryFrameStore>(
            config.resolvedShmName(), config.capacity().totalBytes(), true,
            config.replaceStaleSegment
        )
    ) {}

RingBufferRAMFacade::~RingBufferRAMFacade() {
    stopCapture();
}

// -------------------------------------------------------------- ciclo de vida

void RingBufferRAMFacade::startCapture(const std::string& /*pipelineDesc*/) {
    if (m_isCapturing.exchange(true)) {
        return;
    }
    m_source->start([this](const CapturedFrame& frame) { onFrame(frame); });
}

void RingBufferRAMFacade::stopCapture() {
    {
        // O flag muda SOB o mutex: uma extracao que acabou de avaliar o
        // predicado ainda nao dormiu e, sem isso, perderia o notify abaixo e
        // ficaria pendurada ate o timeout inteiro.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isCapturing.exchange(false)) {
            return;
        }
    }
    // Acorda quem espera pelo pos-evento ANTES de derrubar a fonte, para que
    // nenhuma extracao fique pendurada ate o timeout.
    m_frameArrived.notify_all();
    m_source->stop();
    // Fecha a arena sob o mesmo mutex das leituras: uma extracao em curso
    // termina antes do munmap, e as seguintes ja veem m_isCapturing == false.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_store->close();
}

// ------------------------------------------------------------------ ingestao

void RingBufferRAMFacade::onFrame(const CapturedFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        try {
            const domain::Allocation allocation = m_ingest->execute(frame);
            if (allocation.sessionRestarted) {
                // Sessao nova: a ancora entre relogio de captura e relogio de
                // parede precisa ser refeita, senao o replay traduziria tempos
                // da sessao anterior.
                m_clock.reset();
                std::fprintf(stderr, "[S4.1] session switched to %s; buffer restarted\n",
                             frame.sessionId.c_str());
            }
            m_clock.anchorIfNeeded(frame.captureTsNs);
        } catch (const domain::DomainError& error) {
            // Quadro invalido nao derruba a captura: e contado e registrado,
            // porque B2 exige que nada seja descartado silenciosamente.
            m_framesRejectedTotal.fetch_add(1);
            std::fprintf(stderr, "[S4.1] frame rejected: %s\n", error.what());
            return;
        }
    }
    m_frameArrived.notify_all();
}

// ------------------------------------------------------- contratos do S4.2

std::vector<VideoFrame> RingBufferRAMFacade::extractWindow(
    domain::IMediaBufferReader::TimePoint start,
    domain::IMediaBufferReader::TimePoint end
) {
    std::vector<VideoFrame> result;
    if (!m_clock.isAnchored()) {
        return result;
    }
    try {
        const CaptureWindow window(m_clock.toCaptureTsNs(start), m_clock.toCaptureTsNs(end));
        const application::ExtractedSegment segment = extractCaptureWindow(window);
        result.reserve(segment.frames.size());
        for (const auto& frame : segment.frames) {
            VideoFrame videoFrame;
            videoFrame.data = frame.data;
            videoFrame.timestamp = m_clock.toWallClock(frame.captureTsNs);
            videoFrame.width = m_config.width;
            videoFrame.height = m_config.height;
            result.push_back(std::move(videoFrame));
        }
    } catch (const domain::DomainError&) {
        // Janela ausente ou sem keyframe: o contrato do S4.2 e "lista vazia",
        // e o ExtractClipUseCase converte isso em MediaBufferEmptyError.
        result.clear();
    } catch (const domain::BufferNotRunningError&) {
        // Captura parada (desligamento): mesma resposta, a arena ja foi fechada.
        result.clear();
    }
    return result;
}

std::vector<std::vector<std::uint8_t>> RingBufferRAMFacade::readWindow(
    domain::IMediaBufferReader::TimePoint start,
    domain::IMediaBufferReader::TimePoint end
) const {
    std::vector<std::vector<std::uint8_t>> payloads;
    if (!m_clock.isAnchored()) {
        return payloads;
    }
    try {
        const CaptureWindow window(m_clock.toCaptureTsNs(start), m_clock.toCaptureTsNs(end));
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isCapturing.load()) {
            // Arena ja fechada por stopCapture(): nada a ler, e o contrato do
            // S4.2 e lista vazia (nunca FrameStoreError).
            return payloads;
        }
        const application::ExtractedSegment segment = m_extract->execute(window);
        payloads.reserve(segment.frames.size());
        for (const auto& frame : segment.frames) {
            payloads.push_back(frame.data);
        }
    } catch (const domain::DomainError&) {
        payloads.clear();
    }
    return payloads;
}

// -------------------------------------------------------- API nativa do S4.1

application::ExtractedSegment RingBufferRAMFacade::extractAround(
    Nanoseconds eventCaptureTsNs,
    double preSeconds,
    double postSeconds,
    double waitTimeoutSeconds
) {
    const CaptureWindow window = CaptureWindow::around(eventCaptureTsNs, preSeconds, postSeconds);
    return extractCaptureWindow(window, waitTimeoutSeconds);
}

application::ExtractedSegment RingBufferRAMFacade::extractCaptureWindow(
    const CaptureWindow& window,
    double waitTimeoutSeconds
) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_isCapturing.load()) {
        throw domain::BufferNotRunningError("call startCapture() before extracting segments");
    }
    const auto timeout = std::chrono::duration<double>(waitTimeoutSeconds);
    m_frameArrived.wait_for(lock, timeout, [this, &window] {
        return !m_isCapturing.load() || hasReached(window.endNs());
    });
    // stopCapture() pode ter fechado a arena enquanto esperavamos o pos-evento.
    if (!m_isCapturing.load()) {
        throw domain::BufferNotRunningError("capture stopped while waiting for the post-event");
    }
    return m_extract->execute(window);
}

bool RingBufferRAMFacade::hasReached(Nanoseconds targetTsNs) const {
    return !m_ringBuffer->empty() && m_ringBuffer->newestCaptureTsNs() >= targetTsNs;
}

// ------------------------------------------------------------------ inspecao

domain::BufferStats RingBufferRAMFacade::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ringBuffer->stats();
}

domain::Nanoseconds RingBufferRAMFacade::newestCaptureTsNs() const {
    const domain::BufferStats snapshot = stats();
    // hasFrames e o unico campo que distingue "nenhum quadro" de "o primeiro
    // quadro tem capture_ts 0" (que acontece se a ancora cair em zero).
    return snapshot.hasFrames ? snapshot.newestCaptureTsNs : 0;
}

std::chrono::system_clock::time_point RingBufferRAMFacade::toWallClock(
    domain::Nanoseconds captureTsNs
) const {
    if (!m_clock.isAnchored()) {
        // Sem ancora nao existe traducao: devolver um 1970 silencioso produziria
        // um descritor de clipe com data absurda em vez de um erro visivel.
        throw domain::BufferNotRunningError(
            "capture clock is not anchored yet; no frame has been ingested"
        );
    }
    return m_clock.toWallClock(captureTsNs);
}

} // namespace ods::s4::infrastructure
