#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "s4/application/dtos/extracted_segment.hpp"
#include "s4/application/ring_buffer_config.hpp"
#include "s4/application/services/capture_buffer_reader.hpp"
#include "s4/application/use_cases/extract_capture_window.hpp"
#include "s4/application/use_cases/ingest_frame.hpp"
#include "s4/domain/entities/ring_buffer.hpp"
#include "s4/domain/repositories/frame_store.hpp"
#include "s4/domain/services/frame_source.hpp"
#include "s4/domain/services/media_buffer_reader.hpp"
#include "s4/infrastructure/gstreamer/ram_buffer_facade.hpp"
#include "s4/infrastructure/ringbuffer/capture_clock_bridge.hpp"

namespace ods::s4::infrastructure {

// S4.1 — Ring Buffer Continuo: a implementacao REAL do buffer de RAM.
//
// Substitui MockRAMBufferFacade mantendo exatamente os mesmos dois contratos:
//   * IRAMBufferFacade        -> ciclo de captura e extracao por janela;
//   * domain::IMediaBufferReader -> o que o ExtractClipUseCase (S4.2) consome.
//
// Padrao GoF Facade (guia §6): esconde offsets, volta do buffer circular,
// threads do GStreamer e memoria compartilhada atras de startCapture(),
// extractWindow()/readWindow() e stopCapture().
//
// O que ela acrescenta em relacao ao mock:
//   * memoria limitada de verdade (a arena e circular e expulsa o mais antigo);
//   * alinhamento a keyframe — o trecho devolvido e DECODIFICAVEL;
//   * espera pelo pos-evento, que no instante do evento ainda nao foi capturado;
//   * rejeicao de relogio retrocedendo e reinicio por troca de sessao (B2);
//   * bytes em /dev/shm, legiveis por outro processo sem copia.
class RingBufferRAMFacade
    : public IRAMBufferFacade
    , public domain::IMediaBufferReader
    , public application::ICaptureBufferReader {
public:
    // A fachada e DONA da fonte, do indice e da arena: quando ela morre, o
    // segmento /dev/shm e liberado junto (RAII, sem coletor de lixo).
    RingBufferRAMFacade(
        application::RingBufferConfig config,
        std::unique_ptr<domain::IFrameSource> frameSource,
        std::unique_ptr<domain::IFrameStore> frameStore
    );

    // Sobrecarga de conveniencia: cria a arena em /dev/shm a partir da config.
    RingBufferRAMFacade(
        application::RingBufferConfig config,
        std::unique_ptr<domain::IFrameSource> frameSource
    );

    ~RingBufferRAMFacade() override;

    RingBufferRAMFacade(const RingBufferRAMFacade&) = delete;
    RingBufferRAMFacade& operator=(const RingBufferRAMFacade&) = delete;

    // --- IRAMBufferFacade ---------------------------------------------------

    // pipelineDesc e repassado a fonte quando ela for um pipeline GStreamer;
    // fontes sintetica e de replay o ignoram (ja sabem o que reproduzir).
    void startCapture(const std::string& pipelineDesc) override;
    void stopCapture() override;
    [[nodiscard]] std::vector<VideoFrame> extractWindow(
        domain::IMediaBufferReader::TimePoint start,
        domain::IMediaBufferReader::TimePoint end
    ) override;

    // --- domain::IMediaBufferReader ----------------------------------------

    // Contrato do S4.2: devolve os payloads da janela, ou uma lista VAZIA se
    // nada houver — o ExtractClipUseCase e quem decide lancar
    // MediaBufferEmptyError. Por isso as excecoes do dominio do buffer sao
    // convertidas aqui, e nao propagadas para fora do contrato.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> readWindow(
        domain::IMediaBufferReader::TimePoint start,
        domain::IMediaBufferReader::TimePoint end
    ) const override;

    // --- API nativa do S4.1 (usada por quem conhece o relogio de captura) ---

    // Trecho [T - pre, T + post] em torno do evento, no relogio de captura.
    //
    // O "antes" ja esta no buffer; o "depois" AINDA ESTA SENDO CAPTURADO, por
    // isso o metodo espera o buffer alcancar T + post (ou o timeout). Com
    // timeout, devolve o que existe e marca isTruncatedAtEnd, em vez de falhar
    // ou de o chamador ter que fazer sleep no escuro.
    [[nodiscard]] application::ExtractedSegment extractAround(
        domain::Nanoseconds eventCaptureTsNs,
        double preSeconds,
        double postSeconds,
        double waitTimeoutSeconds = 0.0
    );

    [[nodiscard]] application::ExtractedSegment extractCaptureWindow(
        const domain::CaptureWindow& window,
        double waitTimeoutSeconds = 0.0
    );

    // --- inspecao -----------------------------------------------------------

    [[nodiscard]] domain::BufferStats stats() const;

    // --- application::ICaptureBufferReader ---------------------------------

    // Os tres metodos abaixo ja existiam na fachada por causa do S4.1; o que
    // mudou foi expor a mesma capacidade por uma porta, para que o caso de uso
    // de extracao dependa do CONTRATO e nao da classe concreta de
    // infraestrutura.

    // stats() e o nome historico (S4.1); bufferStats() e o nome da porta. Os
    // dois coexistem para nao reescrever os testes antigos.
    [[nodiscard]] domain::BufferStats bufferStats() const override { return stats(); }

    // Instante de captura mais recente, ou 0 se nada foi capturado ainda.
    // A API usa isso para tratar "agora" em relogio de captura: sem ele, um
    // cliente so teria como chutar um capture_ts e o resultado seria um 500
    // dificil de explicar.
    [[nodiscard]] domain::Nanoseconds newestCaptureTsNs() const override;

    // Traduz captura -> parede pelo mesmo ponto unico do componente.
    [[nodiscard]] std::chrono::system_clock::time_point toWallClock(
        domain::Nanoseconds captureTsNs
    ) const override;

    [[nodiscard]] std::uint64_t framesRejectedTotal() const noexcept {
        return m_framesRejectedTotal.load();
    }
    [[nodiscard]] bool isCapturing() const noexcept { return m_isCapturing.load(); }
    [[nodiscard]] const application::RingBufferConfig& config() const noexcept { return m_config; }

private:
    void onFrame(const domain::CapturedFrame& frame);
    [[nodiscard]] bool hasReached(domain::Nanoseconds targetTsNs) const;

    application::RingBufferConfig m_config;
    std::unique_ptr<domain::IFrameSource> m_source;
    std::unique_ptr<domain::IFrameStore> m_store;
    std::unique_ptr<domain::RingBuffer> m_ringBuffer;
    std::unique_ptr<application::IngestFrameUseCase> m_ingest;
    std::unique_ptr<application::ExtractCaptureWindowUseCase> m_extract;
    CaptureClockBridge m_clock;

    // Um unico mutex serializa indice + arena: a fonte entrega quadros na
    // thread do GStreamer enquanto o S4.2 pede extracoes na thread dele.
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_frameArrived;
    std::atomic<bool> m_isCapturing{false};
    std::atomic<std::uint64_t> m_framesRejectedTotal{0};
};

} // namespace ods::s4::infrastructure
