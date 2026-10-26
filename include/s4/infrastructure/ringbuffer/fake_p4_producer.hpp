#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "s4/domain/value_objects/p4_frame_metadata.hpp"
#include "s4/domain/value_objects/session_started.hpp"

namespace ods::s4::infrastructure {

struct FakeP4Options {
    std::string sourceId{"cam0"};
    domain::RawVideoFormat format{1280, 720, 30.0};
    // captured_at do primeiro quadro, em ns UTC; 0 = agora (system_clock).
    domain::Nanoseconds startCapturedAtUtcNs{0};
    domain::SessionMode mode{domain::SessionMode::Live};
    // Quantas imagens distintas pre-renderizar e repetir em ciclo. Renderizar
    // 1080p a cada quadro custaria mais CPU que o encoder que se quer medir.
    std::size_t distinctImages{30};
};

// Um quadro como P4 o entrega: pixels crus + metadados ods.p4.frame.
struct FakeP4Frame {
    domain::P4FrameMetadata metadata;
    const std::vector<std::uint8_t>* pixels{nullptr};
};

// Produtor falso de P4 (e de P6 em replay), exigido pelo manual como
// evidencia minima: entrega o MESMO contrato da camera real — quadro NV12 com
// captured_at, session_id e frame_index — sem camera.
//
// A imagem e um padrao em movimento com ruido, para que o x264enc trabalhe
// como trabalharia com uma cena real (um quadro estatico custaria quase nada
// para codificar e a medicao de CPU sairia otimista).
//
// Sincrono de proposito: o teste chama nextFrame() e startNewSession() na ordem
// que quiser. FakeP4Feed (abaixo) e o laco com relogio para o daemon.
class FakeP4Producer {
public:
    explicit FakeP4Producer(FakeP4Options options);

    // ods.session.started da sessao corrente (START na criacao).
    [[nodiscard]] const domain::SessionStarted& currentSession() const noexcept { return m_session; }

    // Proximo quadro: captured_at avanca 1/fps, frame_index soma 1. O ponteiro
    // de pixels vale ate a proxima chamada que destrua o produtor.
    [[nodiscard]] FakeP4Frame nextFrame();

    // Simula o marcador de P6: session_id novo e frame_index zerado. Em SEEK e
    // LOOP o captured_at VOLTA (rewindToUtcNs, ou o inicio da sessao anterior);
    // nos demais motivos ele continua de onde parou.
    domain::SessionStarted startNewSession(
        domain::SessionChangeReason reason,
        std::optional<domain::Nanoseconds> rewindToUtcNs = std::nullopt
    );

    [[nodiscard]] const FakeP4Options& options() const noexcept { return m_options; }

private:
    [[nodiscard]] std::vector<std::uint8_t> renderImage(std::size_t index) const;

    FakeP4Options m_options;
    std::vector<std::vector<std::uint8_t>> m_images;
    domain::SessionStarted m_session;
    domain::Nanoseconds m_nextCapturedAtUtcNs{0};
    std::uint64_t m_nextFrameIndex{0};
    std::uint64_t m_framesProduced{0};
};

// Laco com relogio sobre o FakeP4Producer: entrega o marcador de sessao e
// depois um quadro a cada 1/fps, como P4 ao vivo. Erros do consumidor sao
// registrados e o laco segue — e o que P4 faria com um consumidor lento.
class FakeP4Feed {
public:
    using FrameSink = std::function<void(const domain::P4FrameMetadata&, const std::uint8_t*, std::size_t)>;
    using SessionSink = std::function<void(const domain::SessionStarted&)>;

    FakeP4Feed(FakeP4Options options, FrameSink frames, SessionSink sessions,
               std::size_t maxFrames = 0, bool realtime = true);
    ~FakeP4Feed();

    FakeP4Feed(const FakeP4Feed&) = delete;
    FakeP4Feed& operator=(const FakeP4Feed&) = delete;

    void start();
    void stop();
    void waitUntilFinished();

    [[nodiscard]] std::uint64_t framesDelivered() const noexcept { return m_delivered.load(); }
    [[nodiscard]] std::uint64_t framesFailed() const noexcept { return m_failed.load(); }

private:
    void run();

    FakeP4Producer m_producer;
    FrameSink m_frames;
    SessionSink m_sessions;
    std::size_t m_maxFrames;
    bool m_realtime;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<std::uint64_t> m_delivered{0};
    std::atomic<std::uint64_t> m_failed{0};
};

} // namespace ods::s4::infrastructure
