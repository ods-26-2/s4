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
    m_extract(std::make_unique<application::ExtractCaptureWindowUseCase>(*m_ringBuffer, *m_store)) {
    rejectIfDepthInsufficient();
}

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

// Item 5: a janela precisa cobrir pre-rolagem + persistencia + latencia +
// post_s + GOP. Checado no construtor, antes de qualquer quadro: subir com
// menos gravaria clipes sem o comeco, e ninguem perceberia ate precisar da
// evidencia. Se lancar, a arena ja criada e liberada pelo destrutor do membro.
void RingBufferRAMFacade::rejectIfDepthInsufficient() const {
    if (m_config.depthRequirement.has_value() &&
        !m_config.depthRequirement->isSatisfiedBy(m_config.windowSeconds)) {
        throw domain::InsufficientBufferDepthError(
            "window of " + std::to_string(m_config.windowSeconds) +
            " s is shorter than the required depth: " + m_config.depthRequirement->describe()
        );
    }
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
            if (frame.sessionId.empty()) {
                // Fonte sem metadados de sessao (ex.: shmsrc de P4 sem o
                // ods.p4.frame): o quadro pertence a sessao anunciada.
                CapturedFrame inherited = frame;
                inherited.sessionId = m_announcedSessionId.empty()
                    ? m_config.cameraId + "-local"
                    : m_announcedSessionId;
                ingest(inherited);
            } else {
                ingest(frame);
            }
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

// Chamado com m_mutex travado.
void RingBufferRAMFacade::ingest(const CapturedFrame& frame) {
    const domain::Allocation allocation = m_ingest->execute(frame);
    if (allocation.sessionRestarted) {
        // Sessao nova: a ancora entre relogio de captura e relogio de
        // parede precisa ser refeita, senao o replay traduziria tempos
        // da sessao anterior.
        m_clock.reset();
        ++m_sessionGeneration;
        std::fprintf(stderr, "[S4.1] session switched to %s; buffer restarted\n",
                     frame.sessionId.c_str());
    }
    m_clock.anchorIfNeeded(frame.captureTsNs, frame.timeBase);
}

// ------------------------------------------------------------ sessao oficial

bool RingBufferRAMFacade::onSessionStarted(const domain::SessionStarted& marker) {
    if (!marker.concerns(m_config.cameraId) || marker.sessionId.empty()) {
        return false;
    }
    bool restarted = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ringBuffer->hasEnded(marker.sessionId)) {
            // Marcador reentregue fora de ordem: a sessao ja acabou.
            return false;
        }
        m_announcedSessionId = marker.sessionId;
        restarted = m_ringBuffer->startSession(marker.sessionId);
        if (restarted) {
            m_clock.reset();
            ++m_sessionGeneration;
            std::fprintf(stderr, "[S4.1] ods.session.started %s (%s); buffer restarted\n",
                         marker.sessionId.c_str(), domain::to_string(marker.reason));
        }
    }
    if (restarted) {
        // Quem espera o pos-evento da sessao velha nao vai mais recebe-lo.
        m_frameArrived.notify_all();
    }
    return restarted;
}

std::string RingBufferRAMFacade::inheritedSessionId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_announcedSessionId.empty() ? m_config.cameraId + "-local" : m_announcedSessionId;
}

domain::Nanoseconds RingBufferRAMFacade::toCaptureTsNs(
    std::chrono::system_clock::time_point wallClock
) const {
    if (!m_clock.isAnchored()) {
        throw domain::BufferNotRunningError(
            "capture clock is not anchored yet; no frame has been ingested"
        );
    }
    return m_clock.toCaptureTsNs(wallClock);
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
    // Recusa ANTES de esperar: um pedido que nunca vai ser atendido nao deve
    // prender a thread do chamador ate o timeout.
    rejectIfOutOfBuffer(window);
    const std::uint64_t generation = m_sessionGeneration;
    const auto timeout = std::chrono::duration<double>(waitTimeoutSeconds);
    m_frameArrived.wait_for(lock, timeout, [this, &window, generation] {
        return !m_isCapturing.load() || m_sessionGeneration != generation ||
               hasReached(window.endNs());
    });
    // stopCapture() pode ter fechado a arena enquanto esperavamos o pos-evento.
    if (!m_isCapturing.load()) {
        throw domain::BufferNotRunningError("capture stopped while waiting for the post-event");
    }
    if (m_sessionGeneration != generation) {
        throw domain::OutOfBufferError(
            "session changed to " + m_ringBuffer->sessionId() +
            " while waiting for the post-event; the window was discarded with the old session"
        );
    }
    return m_extract->execute(window);
}

// Chamado com m_mutex travado. Buffer vazio nao tem "mais antigo" nem "mais
// recente" para comparar: a decisao fica para a espera (que pode trazer os
// primeiros quadros) e para o segmentFor().
void RingBufferRAMFacade::rejectIfOutOfBuffer(const CaptureWindow& window) const {
    if (m_ringBuffer->empty()) {
        return;
    }
    const Nanoseconds oldest = m_ringBuffer->oldestCaptureTsNs();
    const Nanoseconds newest = m_ringBuffer->newestCaptureTsNs();
    if (window.endNs() < oldest) {
        throw domain::WindowNotInBufferError(
            "window ends at " + std::to_string(window.endNs()) +
            " but the oldest frame in the buffer is " + std::to_string(oldest) +
            "; it has already been evicted (buffer depth " +
            std::to_string(m_config.windowSeconds) + " s)"
        );
    }
    const auto maxPostNs = static_cast<Nanoseconds>(
        m_config.maxPostSeconds * domain::kNanosecondsPerSecond);
    if (window.endNs() - newest > maxPostNs) {
        throw domain::PostWindowTooLongError(
            "window ends " +
            std::to_string(static_cast<double>(window.endNs() - newest) /
                           domain::kNanosecondsPerSecond) +
            " s after the newest frame; the maximum post_s is " +
            std::to_string(m_config.maxPostSeconds) + " s"
        );
    }
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
